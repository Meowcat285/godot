/**************************************************************************/
/*  cas_backend.h                                                         */
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

#include "core/object/gdvirtual.gen.h"
#include "core/object/ref_counted.h"

// A shared store for editor import results (a network drive, a cache server...), queried by the import cache when
// a result isn't available locally, and given results of imports made locally.
//
// Content is addressed by BLAKE3 hashes (as lowercase hexadecimal strings):
// - Manifests describe the result of an import, keyed by the hash of the import inputs.
// - Objects (imported files) are stored as lists of content-defined chunks, so that near-identical files only
//   transfer the chunks that differ.
// Implementations must be thread-safe, as imports run on multiple threads.
class CASBackend : public RefCounted {
	GDCLASS(CASBackend, RefCounted);

protected:
	static void _bind_methods();

	GDVIRTUAL0RC(bool, _is_writable)
	GDVIRTUAL1R(PackedByteArray, _get_manifest, String)
	GDVIRTUAL2R(Error, _put_manifest, String, PackedByteArray)
	GDVIRTUAL1R(PackedStringArray, _get_object_chunks, String)
	GDVIRTUAL2R(Error, _put_object_chunks, String, PackedStringArray)
	GDVIRTUAL1R(PackedByteArray, _has_chunks, PackedStringArray)
	GDVIRTUAL1R(PackedByteArray, _get_chunk, String)
	GDVIRTUAL2R(Error, _put_chunk, String, PackedByteArray)

public:
	// Adds a backend to the editor's import cache, which is queried in order.
	static void add_import_cache_backend(const Ref<CASBackend> &p_backend);
	static void remove_import_cache_backend(const Ref<CASBackend> &p_backend);

	virtual bool is_writable() const;
	// Empty if missing.
	virtual Vector<uint8_t> get_manifest(const String &p_key);
	virtual Error put_manifest(const String &p_key, const Vector<uint8_t> &p_data);
	// Hashes of the chunks of an object, in order. Empty if missing.
	virtual Vector<String> get_object_chunks(const String &p_hash);
	virtual Error put_object_chunks(const String &p_hash, const Vector<String> &p_chunks);
	// One byte per hash, non-zero if the chunk is available.
	virtual Vector<uint8_t> has_chunks(const Vector<String> &p_hashes);
	virtual Vector<uint8_t> get_chunk(const String &p_hash);
	virtual Error put_chunk(const String &p_hash, const Vector<uint8_t> &p_data);
};

// Backend storing everything in a directory, e.g. on a network drive shared by a team.
class CASLocalDirectoryBackend : public CASBackend {
	GDCLASS(CASLocalDirectoryBackend, CASBackend);

	String path;
	bool writable = true;

	String _get_path(const String &p_kind, const String &p_hash) const;
	Error _write_atomic(const String &p_path, const Vector<uint8_t> &p_data) const;

protected:
	static void _bind_methods();

public:
	void set_path(const String &p_path) { path = p_path; }
	String get_path() const { return path; }
	void set_writable(bool p_writable) { writable = p_writable; }

	virtual bool is_writable() const override { return writable && !path.is_empty(); }
	virtual Vector<uint8_t> get_manifest(const String &p_key) override;
	virtual Error put_manifest(const String &p_key, const Vector<uint8_t> &p_data) override;
	virtual Vector<String> get_object_chunks(const String &p_hash) override;
	virtual Error put_object_chunks(const String &p_hash, const Vector<String> &p_chunks) override;
	virtual Vector<uint8_t> has_chunks(const Vector<String> &p_hashes) override;
	virtual Vector<uint8_t> get_chunk(const String &p_hash) override;
	virtual Error put_chunk(const String &p_hash, const Vector<uint8_t> &p_data) override;
};
