/**************************************************************************/
/*  cas_backend.cpp                                                       */
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

#include "cas_backend.h"

#include "cas_import_store.h"

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/random_number_generator.h"
#include "core/object/class_db.h"

bool CASBackend::is_writable() const {
	bool ret = false;
	GDVIRTUAL_CALL(_is_writable, ret);
	return ret;
}

Vector<uint8_t> CASBackend::get_manifest(const String &p_key) {
	PackedByteArray ret;
	GDVIRTUAL_CALL(_get_manifest, p_key, ret);
	return ret;
}

Error CASBackend::put_manifest(const String &p_key, const Vector<uint8_t> &p_data) {
	Error ret = ERR_UNAVAILABLE;
	GDVIRTUAL_CALL(_put_manifest, p_key, p_data, ret);
	return ret;
}

Vector<String> CASBackend::get_object_chunks(const String &p_hash) {
	PackedStringArray ret;
	GDVIRTUAL_CALL(_get_object_chunks, p_hash, ret);
	return ret;
}

Error CASBackend::put_object_chunks(const String &p_hash, const Vector<String> &p_chunks) {
	Error ret = ERR_UNAVAILABLE;
	GDVIRTUAL_CALL(_put_object_chunks, p_hash, p_chunks, ret);
	return ret;
}

Vector<uint8_t> CASBackend::has_chunks(const Vector<String> &p_hashes) {
	PackedByteArray ret;
	if (!GDVIRTUAL_CALL(_has_chunks, p_hashes, ret)) {
		ret.resize(p_hashes.size());
		ret.fill(0);
	}
	return ret;
}

Vector<uint8_t> CASBackend::get_chunk(const String &p_hash) {
	PackedByteArray ret;
	GDVIRTUAL_CALL(_get_chunk, p_hash, ret);
	return ret;
}

Error CASBackend::put_chunk(const String &p_hash, const Vector<uint8_t> &p_data) {
	Error ret = ERR_UNAVAILABLE;
	GDVIRTUAL_CALL(_put_chunk, p_hash, p_data, ret);
	return ret;
}

void CASBackend::add_import_cache_backend(const Ref<CASBackend> &p_backend) {
	ERR_FAIL_NULL(CASImportStore::get_singleton());
	CASImportStore::get_singleton()->add_backend(p_backend);
}

void CASBackend::remove_import_cache_backend(const Ref<CASBackend> &p_backend) {
	ERR_FAIL_NULL(CASImportStore::get_singleton());
	CASImportStore::get_singleton()->remove_backend(p_backend);
}

void CASBackend::_bind_methods() {
	ClassDB::bind_static_method("CASBackend", D_METHOD("add_import_cache_backend", "backend"), &CASBackend::add_import_cache_backend);
	ClassDB::bind_static_method("CASBackend", D_METHOD("remove_import_cache_backend", "backend"), &CASBackend::remove_import_cache_backend);

	GDVIRTUAL_BIND(_is_writable);
	GDVIRTUAL_BIND(_get_manifest, "key");
	GDVIRTUAL_BIND(_put_manifest, "key", "data");
	GDVIRTUAL_BIND(_get_object_chunks, "hash");
	GDVIRTUAL_BIND(_put_object_chunks, "hash", "chunks");
	GDVIRTUAL_BIND(_has_chunks, "hashes");
	GDVIRTUAL_BIND(_get_chunk, "hash");
	GDVIRTUAL_BIND(_put_chunk, "hash", "data");
}

//////////////////////////////////////////////////////////////////

String CASLocalDirectoryBackend::_get_path(const String &p_kind, const String &p_hash) const {
	if (p_hash.length() < 2 || !p_hash.is_valid_hex_number(false)) {
		return String();
	}
	return path.path_join(p_kind).path_join(p_hash.substr(0, 2)).path_join(p_hash);
}

Error CASLocalDirectoryBackend::_write_atomic(const String &p_path, const Vector<uint8_t> &p_data) const {
	if (FileAccess::exists(p_path)) {
		return OK; // Content addressed, already there.
	}
	Error err = DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	ERR_FAIL_COND_V(err != OK && err != ERR_ALREADY_EXISTS, err);

	// Write to a temporary file and rename it, so that other editors never see partial files.
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	String tmp = p_path + ".tmp" + itos(rng->randi());
	{
		Ref<FileAccess> f = FileAccess::open(tmp, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V(f.is_null(), err);
		f->store_buffer(p_data.ptr(), p_data.size());
		if (f->get_error() != OK) {
			f.unref();
			DirAccess::remove_absolute(tmp);
			return ERR_FILE_CANT_WRITE;
		}
	}
	err = DirAccess::rename_absolute(tmp, p_path);
	if (err != OK) {
		DirAccess::remove_absolute(tmp);
		return FileAccess::exists(p_path) ? OK : err;
	}
	return OK;
}

Vector<uint8_t> CASLocalDirectoryBackend::get_manifest(const String &p_key) {
	String p = _get_path("manifests", p_key);
	if (p.is_empty() || !FileAccess::exists(p)) {
		return Vector<uint8_t>();
	}
	return FileAccess::get_file_as_bytes(p);
}

Error CASLocalDirectoryBackend::put_manifest(const String &p_key, const Vector<uint8_t> &p_data) {
	String p = _get_path("manifests", p_key);
	ERR_FAIL_COND_V(p.is_empty() || !is_writable(), ERR_UNAVAILABLE);
	return _write_atomic(p, p_data);
}

Vector<String> CASLocalDirectoryBackend::get_object_chunks(const String &p_hash) {
	String p = _get_path("objects", p_hash);
	if (p.is_empty() || !FileAccess::exists(p)) {
		return Vector<String>();
	}
	return FileAccess::get_file_as_string(p).split("\n", false);
}

Error CASLocalDirectoryBackend::put_object_chunks(const String &p_hash, const Vector<String> &p_chunks) {
	String p = _get_path("objects", p_hash);
	ERR_FAIL_COND_V(p.is_empty() || !is_writable(), ERR_UNAVAILABLE);
	return _write_atomic(p, String("\n").join(p_chunks).to_utf8_buffer());
}

Vector<uint8_t> CASLocalDirectoryBackend::has_chunks(const Vector<String> &p_hashes) {
	Vector<uint8_t> ret;
	ret.resize(p_hashes.size());
	for (int i = 0; i < p_hashes.size(); i++) {
		String p = _get_path("chunks", p_hashes[i]);
		ret.write[i] = (!p.is_empty() && FileAccess::exists(p)) ? 1 : 0;
	}
	return ret;
}

Vector<uint8_t> CASLocalDirectoryBackend::get_chunk(const String &p_hash) {
	String p = _get_path("chunks", p_hash);
	if (p.is_empty() || !FileAccess::exists(p)) {
		return Vector<uint8_t>();
	}
	// Chunks are stored as: u32 size, zstd data.
	Vector<uint8_t> stored = FileAccess::get_file_as_bytes(p);
	if (stored.size() < 4) {
		return Vector<uint8_t>();
	}
	uint32_t size = uint32_t(stored[0]) | (uint32_t(stored[1]) << 8) | (uint32_t(stored[2]) << 16) | (uint32_t(stored[3]) << 24);
	Vector<uint8_t> data;
	data.resize_uninitialized(size);
	int64_t ret = Compression::decompress(data.ptrw(), size, stored.ptr() + 4, stored.size() - 4, Compression::MODE_ZSTD);
	if (ret != int64_t(size)) {
		return Vector<uint8_t>();
	}
	return data;
}

Error CASLocalDirectoryBackend::put_chunk(const String &p_hash, const Vector<uint8_t> &p_data) {
	String p = _get_path("chunks", p_hash);
	ERR_FAIL_COND_V(p.is_empty() || !is_writable(), ERR_UNAVAILABLE);
	if (FileAccess::exists(p)) {
		return OK;
	}
	Vector<uint8_t> stored;
	stored.resize(4 + Compression::get_max_compressed_buffer_size(p_data.size(), Compression::MODE_ZSTD));
	uint32_t size = p_data.size();
	for (int i = 0; i < 4; i++) {
		stored.write[i] = (size >> (i * 8)) & 0xFF;
	}
	int64_t csize = Compression::compress(stored.ptrw() + 4, p_data.ptr(), p_data.size(), Compression::MODE_ZSTD);
	ERR_FAIL_COND_V(csize < 0, ERR_BUG);
	stored.resize(4 + csize);
	return _write_atomic(p, stored);
}

void CASLocalDirectoryBackend::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_path", "path"), &CASLocalDirectoryBackend::set_path);
	ClassDB::bind_method(D_METHOD("get_path"), &CASLocalDirectoryBackend::get_path);
	ClassDB::bind_method(D_METHOD("set_writable", "writable"), &CASLocalDirectoryBackend::set_writable);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "path", PROPERTY_HINT_GLOBAL_DIR), "set_path", "get_path");
}
