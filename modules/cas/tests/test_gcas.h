/**************************************************************************/
/*  test_gcas.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "../cas_layout.h"
#include "../fastcdc.h"
#include "../gcas_packer.h"
#include "../gcas_writer.h"

#include "core/crypto/cas_hash.h"
#include "core/io/dir_access.h"
#include "core/io/file_access_gcas.h"
#include "core/io/file_access_pack.h"
#include "core/math/random_pcg.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

namespace TestGCAS {

inline Vector<uint8_t> make_data(uint64_t p_size, uint64_t p_seed, bool p_compressible) {
	Vector<uint8_t> data;
	data.resize(p_size);
	RandomPCG rng(p_seed);
	uint8_t *w = data.ptrw();
	for (uint64_t i = 0; i < p_size; i++) {
		if (p_compressible) {
			// Repetitive text-like data, with some noise so that chunking has something to work with.
			w[i] = (rng.rand() % 16 == 0) ? uint8_t(rng.rand()) : uint8_t('a' + (i * 7 + i / 13) % 26);
		} else {
			w[i] = uint8_t(rng.rand());
		}
	}
	return data;
}

inline Vector<uint8_t> read_all(const String &p_path) {
	Error err;
	Vector<uint8_t> data = FileAccess::get_file_as_bytes(p_path, &err);
	CHECK(err == OK);
	return data;
}

class TestLayoutProvider : public CASLayoutProvider {
	GDSOFTCLASS(TestLayoutProvider, CASLayoutProvider);

public:
	virtual String get_provider_name() const override { return "test_provider"; }
	virtual bool handles_file(const String &p_path) const override { return p_path.get_extension() == "layout"; }
	virtual Ref<CASLayout> get_layout(const String &p_path, const Vector<uint8_t> &p_data) const override {
		Ref<CASLayout> layout;
		layout.instantiate();
		layout->add_cut(1000);
		layout->add_cut(50000);
		layout->add_group(100000, 200000); // Larger than the CDC maximum, but kept in one chunk.
		layout->add_paged_range(400000, 128 * 1024, 32 * 1024);
		layout->set_range_codec(600000, 10000, CASLayout::CODEC_RAW);
		PackedByteArray hint;
		hint.push_back(42);
		layout->set_hint_data(hint);
		return layout;
	}
};

TEST_CASE("[CAS] BLAKE3 test vectors") {
	CHECK(CASHash::compute(nullptr, 0).to_string() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
	CharString abc = String("abc").utf8();
	CASHash h = CASHash::compute((const uint8_t *)abc.get_data(), 3);
	CHECK(h.to_string() == "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
	CHECK(CASHash::from_string(h.to_string()) == h);
	CHECK(CASHash::from_string("not a hash").is_zero());

	// Streaming in pieces gives the same result as hashing at once (and exercises the SIMD paths).
	Vector<uint8_t> data = make_data(100000, 1, false);
	Blake3Context ctx;
	ctx.start();
	ctx.update(data.ptr(), 1234);
	ctx.update(data.ptr() + 1234, data.size() - 1234);
	CHECK(ctx.finish() == CASHash::compute(data));
}

TEST_CASE("[CAS] FastCDC chunk sizes and determinism") {
	Vector<uint8_t> data = make_data(4 * 1024 * 1024, 2, false);
	FastCDC::Params params;
	LocalVector<uint64_t> ends;
	FastCDC::split(data.ptr(), data.size(), params, ends);
	REQUIRE(ends.size() > 0);
	CHECK(ends[ends.size() - 1] == uint64_t(data.size()));

	uint64_t prev = 0;
	for (uint32_t i = 0; i < ends.size(); i++) {
		uint64_t size = ends[i] - prev;
		CHECK(size <= params.max_size);
		if (i + 1 < ends.size()) {
			CHECK(size >= params.min_size);
		}
		prev = ends[i];
	}
	// Average chunk size should be in the right ballpark.
	uint64_t avg = data.size() / ends.size();
	CHECK(avg > params.avg_size / 2);
	CHECK(avg < params.avg_size * 2);

	LocalVector<uint64_t> ends2;
	FastCDC::split(data.ptr(), data.size(), params, ends2);
	REQUIRE(ends.size() == ends2.size());
	for (uint32_t i = 0; i < ends.size(); i++) {
		CHECK(ends[i] == ends2[i]);
	}
}

TEST_CASE("[CAS] FastCDC boundaries resynchronize after an insertion") {
	Vector<uint8_t> data = make_data(4 * 1024 * 1024, 3, false);
	Vector<uint8_t> edited = data;
	Vector<uint8_t> insertion = make_data(777, 4, false);
	int64_t at = 1000000;
	for (int64_t i = 0; i < insertion.size(); i++) {
		edited.insert(at + i, insertion[i]);
	}

	FastCDC::Params params;
	auto chunk_hashes = [&](const Vector<uint8_t> &p_data) {
		LocalVector<uint64_t> ends;
		FastCDC::split(p_data.ptr(), p_data.size(), params, ends);
		HashSet<CASHash> hashes;
		uint64_t start = 0;
		for (uint64_t end : ends) {
			hashes.insert(CASHash::compute(p_data.ptr() + start, end - start));
			start = end;
		}
		return hashes;
	};
	HashSet<CASHash> a = chunk_hashes(data);
	HashSet<CASHash> b = chunk_hashes(edited);
	uint32_t shared = 0;
	for (const CASHash &h : a) {
		shared += b.has(h) ? 1 : 0;
	}
	// Only the chunks around the insertion change.
	CHECK(shared >= a.size() - 3);
}

TEST_CASE("[GCAS] Write and read back files") {
	const String pack_path = TestUtils::get_temp_path("gcas_test_basic.gcas");
	Vector<uint8_t> text = String("Hello, content-addressed world!").to_utf8_buffer();
	Vector<uint8_t> random = make_data(1500000, 10, false);
	// Constant sizes, as compilers can't tell that Vector sizes used as divisors aren't zero.
	constexpr uint64_t compressible_size = 2500000;
	Vector<uint8_t> compressible = make_data(compressible_size, 11, true);

	GCASWriter writer;
	writer.get_settings().zstd_level = 3;
	REQUIRE(writer.start(pack_path) == OK);
	CHECK(writer.add_file("res://gcas_test/hello.txt", text, 1234567) == OK);
	CHECK(writer.add_file("res://gcas_test/random.bin", random) == OK);
	CHECK(writer.add_file("res://gcas_test/sub/compressible.bin", compressible) == OK);
	CHECK(writer.add_file("res://gcas_test/sub/copy.bin", compressible) == OK);
	CHECK(writer.add_file("res://gcas_test/empty.bin", Vector<uint8_t>()) == OK);
	CHECK(writer.get_deduplicated_size() >= uint64_t(compressible.size()));
	REQUIRE(writer.finish() == OK);

	// Duplicate files are stored once, and compressible data is compressed.
	Ref<FileAccess> pf = FileAccess::open(pack_path, FileAccess::READ);
	REQUIRE(pf.is_valid());
	CHECK(pf->get_length() < uint64_t(random.size() + compressible.size()));
	pf.unref();

	PackedSourceGCAS::PackInfo info;
	REQUIRE(PackedSourceGCAS::read_pack_info(pack_path, info) == OK);
	CHECK(info.files.size() == 5);
	CHECK(info.uids.size() == 1);

	REQUIRE(PackedData::get_singleton()->add_pack(pack_path, true, 0) == OK);

	CHECK(read_all("res://gcas_test/hello.txt") == text);
	CHECK(read_all("res://gcas_test/random.bin") == random);
	CHECK(read_all("res://gcas_test/sub/compressible.bin") == compressible);
	CHECK(read_all("res://gcas_test/sub/copy.bin") == compressible);
	CHECK(FileAccess::exists("res://gcas_test/empty.bin"));
	CHECK(FileAccess::get_file_as_bytes("res://gcas_test/empty.bin").is_empty());

	// Random access.
	Ref<FileAccess> f = FileAccess::open("res://gcas_test/sub/compressible.bin", FileAccess::READ);
	REQUIRE(f.is_valid());
	CHECK(f->get_length() == uint64_t(compressible.size()));
	RandomPCG rng(12);
	for (int i = 0; i < 200; i++) {
		uint64_t pos = rng.rand() % compressible_size;
		uint64_t len = MIN(uint64_t(rng.rand() % 300000), compressible_size - pos);
		Vector<uint8_t> buf;
		buf.resize(len);
		f->seek(pos);
		CHECK(f->get_buffer(buf.ptrw(), len) == len);
		CHECK(memcmp(buf.ptr(), compressible.ptr() + pos, len) == 0);
	}
	// Small sequential reads.
	f->seek(123456);
	for (int i = 0; i < 1000; i++) {
		CHECK(f->get_8() == compressible[123456 + i]);
	}
	// Reading past the end.
	f->seek(compressible.size() - 10);
	uint8_t tail[20];
	CHECK(f->get_buffer(tail, 20) == 10);
	CHECK(f->eof_reached());
	f.unref();

	// Directory listing (ProjectSettings::load_resource_pack() also makes DirAccess use packs).
	Ref<DirAccess> da = PackedData::get_singleton()->try_open_directory("res://gcas_test/sub");
	REQUIRE(da.is_valid());
	CHECK(da->get_files().size() == 2);

	// UIDs are registered from the pack's own table whenever the UID cache is loaded (as when loading packs), and
	// survive a reset of the cache (as on startup).
	ResourceUID::get_singleton()->load_from_cache(false);
	CHECK(ResourceUID::get_singleton()->has_id(1234567));
	CHECK(ResourceUID::get_singleton()->get_id_path(1234567) == "res://gcas_test/hello.txt");
	ResourceUID::get_singleton()->load_from_cache(true);
	CHECK(ResourceUID::get_singleton()->get_id_path(1234567) == "res://gcas_test/hello.txt");
	CHECK(read_all(ResourceUID::get_singleton()->id_to_text(1234567)) == text);

	PackedData::get_singleton()->clear();
	ResourceUID::get_singleton()->remove_id(1234567);
	DirAccess::remove_absolute(pack_path);
}

TEST_CASE("[GCAS] Patch packs reference chunks of their base") {
	const String base_path = TestUtils::get_temp_path("gcas_test_base.gcas");
	const String patch_path = TestUtils::get_temp_path("gcas_test_patch.gcas");
	Vector<uint8_t> big = make_data(3000000, 20, false);
	Vector<uint8_t> big_edited = big;
	for (int i = 0; i < 100; i++) {
		big_edited.write[2000000 + i] = ~big[2000000 + i];
	}
	Vector<uint8_t> other = String("removed in the patch").to_utf8_buffer();
	Vector<uint8_t> added = String("added in the patch").to_utf8_buffer();

	{
		GCASWriter writer;
		writer.get_settings().zstd_level = 3;
		REQUIRE(writer.start(base_path) == OK);
		writer.add_file("res://gcas_patch/big.bin", big);
		writer.add_file("res://gcas_patch/other.txt", other);
		REQUIRE(writer.finish() == OK);
	}
	{
		GCASWriter writer;
		writer.get_settings().zstd_level = 3;
		REQUIRE(writer.start(patch_path) == OK);
		REQUIRE(writer.add_base_pack(base_path) == OK);
		writer.add_file("res://gcas_patch/big.bin", big_edited);
		writer.add_file("res://gcas_patch/added.txt", added);
		writer.add_removal("res://gcas_patch/other.txt");
		REQUIRE(writer.finish() == OK);
	}

	// Only the edited chunks are in the patch.
	Ref<FileAccess> pf = FileAccess::open(patch_path, FileAccess::READ);
	REQUIRE(pf.is_valid());
	CHECK(pf->get_length() < 1000000);
	pf.unref();

	// The patch can't be loaded without its base.
	ERR_PRINT_OFF;
	CHECK(PackedData::get_singleton()->add_pack(patch_path, true, 0) != OK);
	ERR_PRINT_ON;

	REQUIRE(PackedData::get_singleton()->add_pack(base_path, true, 0) == OK);
	CHECK(read_all("res://gcas_patch/big.bin") == big);
	REQUIRE(PackedData::get_singleton()->add_pack(patch_path, true, 0) == OK);
	CHECK(read_all("res://gcas_patch/big.bin") == big_edited);
	CHECK(read_all("res://gcas_patch/added.txt") == added);
	CHECK_FALSE(FileAccess::exists("res://gcas_patch/other.txt"));

	PackedData::get_singleton()->clear();
	DirAccess::remove_absolute(base_path);
	DirAccess::remove_absolute(patch_path);
}

TEST_CASE("[GCAS] Layout providers control chunk boundaries") {
	const String pack_path = TestUtils::get_temp_path("gcas_test_layout.gcas");
	Vector<uint8_t> data = make_data(800000, 30, true);

	GCASWriter writer;
	writer.get_settings().zstd_level = 3;
	Ref<TestLayoutProvider> provider;
	provider.instantiate();
	writer.add_layout_provider(provider);
	REQUIRE(writer.start(pack_path) == OK);
	writer.add_file("res://gcas_layout/data.layout", data);
	REQUIRE(writer.finish() == OK);

	REQUIRE(PackedData::get_singleton()->add_pack(pack_path, true, 0) == OK);
	Ref<FileAccess> f = FileAccess::open("res://gcas_layout/data.layout", FileAccess::READ);
	REQUIRE(f.is_valid());
	Ref<FileAccessGCAS> gf = f;
	REQUIRE(gf.is_valid());

	HashSet<uint64_t> starts;
	bool group_in_one_chunk = false;
	bool raw_range = false;
	for (uint32_t i = 0; i < gf->get_chunk_count(); i++) {
		const FileAccessGCAS::Chunk &c = gf->get_chunk(i);
		starts.insert(c.start);
		if (c.start == 100000 && c.location.size == 200000) {
			group_in_one_chunk = true;
		}
		if (c.start == 600000 && c.location.size == 10000 && c.location.codec == GCAS_CHUNK_CODEC_RAW) {
			raw_range = true;
		}
	}
	CHECK(starts.has(1000));
	CHECK(starts.has(50000));
	CHECK(group_in_one_chunk);
	for (uint64_t page = 400000; page < 400000 + 128 * 1024; page += 32 * 1024) {
		CHECK(starts.has(page));
	}
	CHECK(raw_range);
	CHECK(read_all("res://gcas_layout/data.layout") == data);

	Vector<uint8_t> hint = PackedSourceGCAS::get_singleton()->get_layout_hints("res://gcas_layout/data.layout", "test_provider");
	REQUIRE(hint.size() == 1);
	CHECK(hint[0] == 42);

	f.unref();
	gf.unref();
	PackedData::get_singleton()->clear();
	DirAccess::remove_absolute(pack_path);
}

TEST_CASE("[GCAS] Header flags") {
	const String pack_path = TestUtils::get_temp_path("gcas_test_flags.gcas");
	Ref<GCASPacker> packer;
	packer.instantiate();
	REQUIRE(packer->start(pack_path, 3) == OK);
	packer->add_file_from_buffer("res://gcas_flags/a.txt", String("a").to_utf8_buffer());
	REQUIRE(packer->flush() == OK);

	auto set_flag = [&](int p_offset, uint8_t p_value) {
		Ref<FileAccess> f = FileAccess::open(pack_path, FileAccess::READ_WRITE);
		f->seek(p_offset);
		f->store_8(p_value);
	};

	// Unknown compatible flags are ignored.
	set_flag(24 + 7, 0x80);
	CHECK(PackedData::get_singleton()->add_pack(pack_path, true, 0) == OK);
	CHECK(FileAccess::exists("res://gcas_flags/a.txt"));
	PackedData::get_singleton()->clear();

	// Unknown incompatible flags make the pack unreadable.
	set_flag(32 + 7, 0x80);
	ERR_PRINT_OFF;
	CHECK(PackedData::get_singleton()->add_pack(pack_path, true, 0) != OK);
	ERR_PRINT_ON;

	PackedData::get_singleton()->clear();
	DirAccess::remove_absolute(pack_path);
}

} // namespace TestGCAS
