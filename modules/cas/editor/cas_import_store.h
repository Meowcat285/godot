/**************************************************************************/
/*  cas_import_store.h                                                    */
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

#include "cas_backend.h"

#include "core/crypto/cas_hash.h"
#include "core/io/resource_importer.h"
#include "core/os/mutex.h"
#include "core/os/rw_lock.h"
#include "core/templates/safe_refcount.h"
#include "editor/file_system/editor_import_artifact_store.h"

// Content-addressed store for import results (in `res://.godot/cas`).
//
// - objects/: imported files, named by the BLAKE3 hash of their contents (so identical results are stored once).
// - manifests/: the result of an import (outputs, metadata...), named by its import key: the hash of everything the
//   import depends on (source contents, importer and its version, options, ...). Importing again with the same inputs,
//   e.g. after reverting a change or switching branches, only needs the manifest.
// - sources.log: the import key currently used by each source file, and its hash (to avoid hashing unchanged files).
//
// Importers still write to their import base path (in `res://.godot/imported`), which is used as a staging area.
// Their outputs are then moved here. Loading uses logical paths (import base path + suffix), resolved to objects.
class CASImportStore : public EditorImportArtifactStore, public ResourceImportArtifactProvider {
public:
	struct Manifest {
		CASHash key;
		String importer;
		String type;
		String save_extension;
		bool valid = false;
		Variant metadata;
		Vector<String> variants;
		Vector<String> gen_files;
		HashMap<String, CASHash> outputs; // Suffix of the import base path -> object.

		String to_text() const;
		bool from_text(const String &p_text, bool p_untrusted);
	};

private:
	struct Source {
		uint64_t modified_time = 0;
		int64_t size = 0;
		CASHash source_hash;
		CASHash key;
	};

	static inline CASImportStore *singleton = nullptr;

	RWLock lock;
	bool loaded = false;
	bool enabled = false;
	bool read_only = true;
	String root;
	String imported_dir;
	HashMap<String, Source> sources; // Source path -> current import.
	HashMap<String, String> base_to_source; // Import base path -> source path.
	HashMap<CASHash, Manifest> manifests; // Loaded manifests.

	Mutex log_mutex;
	Ref<FileAccess> source_log;
	uint32_t source_log_entries = 0;

	Mutex backends_mutex;
	Vector<Ref<CASBackend>> backends;

	SafeNumeric<uint64_t> stats_hits;
	SafeNumeric<uint64_t> stats_remote_hits;
	SafeNumeric<uint64_t> stats_misses;

	void _ensure_loaded();
	void _load_sources();
	void _compact_source_log();
	void _record_source(const String &p_path, const Source &p_source);

	bool _hash_file(const String &p_path, Source &r_source);
	bool _compute_key(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, Source &r_source);

	bool _get_manifest(const CASHash &p_key, Manifest &r_manifest);
	Error _save_manifest(const Manifest &p_manifest);
	bool _is_manifest_usable(const Manifest &p_manifest);
	void _clear_staged_files(const String &p_path);
	bool _split_logical_path(const String &p_logical_path, String &r_base, String &r_suffix) const;

	bool _fetch_from_backends(const CASHash &p_key, Manifest &r_manifest);
	void _upload_to_backends(const Manifest &p_manifest);
	Ref<CASBackend> _get_writable_backend();

public:
	static CASImportStore *get_singleton() { return singleton; }

	String get_object_path(const CASHash &p_hash, const String &p_suffix) const;
	String get_manifest_path(const CASHash &p_key) const;

	void add_backend(const Ref<CASBackend> &p_backend);
	void remove_backend(const Ref<CASBackend> &p_backend);
	void update_backends_from_settings();

	// Removes objects and manifests not used by the current imports, oldest first, until the store is at most
	// p_max_size bytes (0 removes everything not used by current imports).
	void collect_garbage(uint64_t p_max_size);
	Dictionary get_stats();

	// EditorImportArtifactStore.
	virtual bool test_for_reimport(const String &p_path) override;
	virtual bool fetch_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, List<String> &r_variants, List<String> &r_gen_files, Variant &r_metadata) override;
	virtual void store_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, const List<String> &p_variants, const List<String> &p_gen_files, const Variant &p_metadata, Error p_import_error, bool p_group = false) override;

	// ResourceImportArtifactProvider.
	virtual Ref<FileAccess> open_import_file(const String &p_source_path, Error *r_error) override;
	virtual String resolve_artifact_path(const String &p_logical_path) override;

	CASImportStore();
	~CASImportStore();
};
