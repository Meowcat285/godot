/**************************************************************************/
/*  pck_packer.cpp                                                        */
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

#include "pck_packer.h"

#include "core/crypto/crypto_core.h"
#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_pack.h" // PACK_HEADER_MAGIC, PACK_FORMAT_VERSION
#include "core/version.h"

static int _get_pad(int p_alignment, int p_n) {
	int rest = p_n % p_alignment;
	int pad = 0;
	if (rest > 0) {
		pad = p_alignment - rest;
	}

	return pad;
}

void PCKPacker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pck_start", "pck_path", "alignment", "key", "encrypt_directory"), &PCKPacker::pck_start, DEFVAL(32), DEFVAL("0000000000000000000000000000000000000000000000000000000000000000"), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("add_file", "target_path", "source_path", "encrypt", "compress"), &PCKPacker::add_file, DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("add_file_removal", "target_path"), &PCKPacker::add_file_removal);
	ClassDB::bind_method(D_METHOD("flush", "verbose"), &PCKPacker::flush, DEFVAL(false));
}

Error PCKPacker::pck_start(const String &p_pck_path, int p_alignment, const String &p_key, bool p_encrypt_directory) {
	ERR_FAIL_COND_V_MSG((p_key.is_empty() || !p_key.is_valid_hex_number(false) || p_key.length() != 64), ERR_CANT_CREATE, "Invalid Encryption Key (must be 64 characters long).");
	ERR_FAIL_COND_V_MSG(p_alignment <= 0, ERR_CANT_CREATE, "Invalid alignment, must be greater then 0.");

	String _key = p_key.to_lower();
	key.resize(32);
	for (int i = 0; i < 32; i++) {
		int v = 0;
		if (i * 2 < _key.length()) {
			char32_t ct = _key[i * 2];
			if (is_digit(ct)) {
				ct = ct - '0';
			} else if (ct >= 'a' && ct <= 'f') {
				ct = 10 + ct - 'a';
			}
			v |= ct << 4;
		}

		if (i * 2 + 1 < _key.length()) {
			char32_t ct = _key[i * 2 + 1];
			if (is_digit(ct)) {
				ct = ct - '0';
			} else if (ct >= 'a' && ct <= 'f') {
				ct = 10 + ct - 'a';
			}
			v |= ct;
		}
		key.write[i] = v;
	}
	enc_dir = p_encrypt_directory;

	file = FileAccess::open(p_pck_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_CREATE, vformat("Can't open file to write: '%s'.", String(p_pck_path)));

	alignment = p_alignment;

	file->store_32(PACK_HEADER_MAGIC);
	file->store_32(PACK_FORMAT_VERSION);
	file->store_32(GODOT_VERSION_MAJOR);
	file->store_32(GODOT_VERSION_MINOR);
	file->store_32(GODOT_VERSION_PATCH);

	uint32_t pack_flags = PACK_REL_FILEBASE;
	if (enc_dir) {
		pack_flags |= PACK_DIR_ENCRYPTED;
	}
	file->store_32(pack_flags); // flags

	file_base_ofs = file->get_position();
	file->store_64(0); // Files base.

	dir_base_ofs = file->get_position();
	file->store_64(0); // Directory offset.

	for (int i = 0; i < 16; i++) {
		file->store_32(0); // Reserved.
	}

	// Align for first file.
	int pad = _get_pad(alignment, file->get_position());
	for (int i = 0; i < pad; i++) {
		file->store_8(0);
	}

	file_base = file->get_position();
	file->seek(file_base_ofs);
	file->store_64(file_base); // Update files base.
	file->seek(file_base);

	files.clear();

	return OK;
}

Error PCKPacker::add_file_removal(const String &p_target_path) {
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_INVALID_PARAMETER, "File must be opened before use.");

	File pf;
	// Simplify path here and on every 'files' access so that paths that have extra '/'
	// symbols or 'res://' in them still match the MD5 hash for the saved path.
	pf.path = p_target_path.simplify_path().trim_prefix("res://");
	pf.ofs = file->get_position();
	pf.size = 0;
	pf.removal = true;

	pf.md5.resize_initialized(16);

	files.push_back(pf);

	return OK;
}

Error PCKPacker::add_file(const String &p_target_path, const String &p_source_path, bool p_encrypt, bool p_compress) {
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_INVALID_PARAMETER, "File must be opened before use.");

	Ref<FileAccess> f = FileAccess::open(p_source_path, FileAccess::READ);
	if (f.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}

	File pf;
	// Simplify path here and on every 'files' access so that paths that have extra '/'
	// symbols or 'res://' in them still match the MD5 hash for the saved path.
	pf.path = p_target_path.simplify_path().trim_prefix("res://");
	pf.src_path = p_source_path;
	pf.ofs = file->get_position();
	pf.uncompressed_size = f->get_length();
	pf.compressed = p_compress;

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(p_source_path);
	
	// Compute MD5 of original (uncompressed) data
	{
		unsigned char hash[16];
		CryptoCore::md5(data.ptr(), data.size(), hash);
		pf.md5.resize(16);
		for (int i = 0; i < 16; i++) {
			pf.md5.write[i] = hash[i];
		}
	}
	
	pf.encrypted = p_encrypt;

	Ref<FileAccess> ftmp = file;

	// Handle encryption wrapper if needed
	Ref<FileAccessEncrypted> fae;
	if (p_encrypt) {
		fae.instantiate();
		ERR_FAIL_COND_V(fae.is_null(), ERR_CANT_CREATE);

		Error err = fae->open_and_parse(file, key, FileAccessEncrypted::MODE_WRITE_AES256, false);
		ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);
		ftmp = fae;
	}

	// Write compressed or uncompressed data
	if (p_compress) {
		// Use chunked compression format compatible with FileAccessCompressed
		const uint32_t block_size = 65536; // 64KB blocks
		const Compression::Mode cmode = Compression::MODE_ZSTD;
		
		// Write compressed file header
		CharString magic_str = String("PCMP").utf8();
		ftmp->store_buffer((const uint8_t *)magic_str.get_data(), 4); // Magic
		ftmp->store_32(cmode); // Compression mode
		ftmp->store_32(block_size); // Block size
		ftmp->store_32(uint32_t(data.size())); // Uncompressed size
		
		uint32_t block_count = (data.size() / block_size) + 1;
		uint64_t block_table_pos = ftmp->get_position();
		
		// Reserve space for block size table
		for (uint32_t i = 0; i < block_count; i++) {
			ftmp->store_32(0);
		}
		
		uint32_t last_block_size = data.size() % block_size;
		if (last_block_size == 0 && data.size() > 0) {
			last_block_size = block_size;
			block_count = data.size() / block_size;
		}
		
		// Compress and write blocks
		Vector<uint32_t> block_sizes;
		block_sizes.resize(block_count);
		
		int64_t max_compressed_size = Compression::get_max_compressed_buffer_size(block_size, cmode);
		Vector<uint8_t> compressed_block;
		compressed_block.resize(max_compressed_size);
		
		for (uint32_t i = 0; i < block_count; i++) {
			uint32_t bl = (i == block_count - 1 && last_block_size > 0) ? last_block_size : block_size;
			const uint8_t *bp = data.ptr() + (i * block_size);
			
			int64_t compressed_size = Compression::compress(compressed_block.ptrw(), bp, bl, cmode);
			ERR_FAIL_COND_V_MSG(compressed_size < 0, ERR_CANT_CREATE, "Error compressing data block.");
			
			ftmp->store_buffer(compressed_block.ptr(), compressed_size);
			block_sizes.write[i] = compressed_size;
		}
		
		// Write magic at the end
		ftmp->store_buffer((const uint8_t *)magic_str.get_data(), 4);
		
		// Go back and write block sizes
		uint64_t end_pos = ftmp->get_position();
		ftmp->seek(block_table_pos);
		for (uint32_t i = 0; i < block_count; i++) {
			ftmp->store_32(block_sizes[i]);
		}
		ftmp->seek(end_pos);
		
		pf.size = end_pos - pf.ofs;
	} else {
		// Write uncompressed data
		ftmp->store_buffer(data);
		pf.size = data.size();
	}

	if (fae.is_valid()) {
		ftmp.unref();
		fae.unref();
	}

	int pad = _get_pad(alignment, file->get_position());
	for (int j = 0; j < pad; j++) {
		file->store_8(0);
	}

	files.push_back(pf);

	return OK;
}

Error PCKPacker::flush(bool p_verbose) {
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_INVALID_PARAMETER, "File must be opened before use.");

	int dir_padding = _get_pad(alignment, file->get_position());
	for (int i = 0; i < dir_padding; i++) {
		file->store_8(0);
	}

	// Write directory.
	uint64_t dir_offset = file->get_position();
	file->seek(dir_base_ofs);
	file->store_64(dir_offset);
	file->seek(dir_offset);

	file->store_32(uint32_t(files.size()));

	Ref<FileAccessEncrypted> fae;
	Ref<FileAccess> fhead = file;

	if (enc_dir) {
		fae.instantiate();
		ERR_FAIL_COND_V(fae.is_null(), ERR_CANT_CREATE);

		Error err = fae->open_and_parse(file, key, FileAccessEncrypted::MODE_WRITE_AES256, false);
		ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);

		fhead = fae;
	}

	const int file_num = files.size();
	for (int i = 0; i < file_num; i++) {
		CharString utf8_string = files[i].path.utf8();
		int string_len = utf8_string.length();
		int pad = _get_pad(4, string_len);

		fhead->store_32(uint32_t(string_len + pad));
		fhead->store_buffer((const uint8_t *)utf8_string.get_data(), string_len);
		for (int j = 0; j < pad; j++) {
			fhead->store_8(0);
		}

		fhead->store_64(files[i].ofs - file_base);
		fhead->store_64(files[i].size);
		fhead->store_buffer(files[i].md5.ptr(), 16);

		uint32_t flags = 0;
		if (files[i].encrypted) {
			flags |= PACK_FILE_ENCRYPTED;
		}
		if (files[i].removal) {
			flags |= PACK_FILE_REMOVAL;
		}
		if (files[i].compressed) {
			flags |= PACK_FILE_COMPRESSED;
		}
		fhead->store_32(flags);

		if (files[i].compressed) {
			fhead->store_64(files[i].uncompressed_size);
		}

		if (p_verbose) {
			print_line(vformat("[%d/%d - %d%%] PCKPacker flush: %s -> %s", i, file_num, float(i) / file_num * 100, files[i].src_path, files[i].path));
		}
	}

	if (fae.is_valid()) {
		fhead.unref();
		fae.unref();
	}

	file.unref();
	return OK;
}

PCKPacker::~PCKPacker() {
	if (file.is_valid()) {
		flush();
	}
}
