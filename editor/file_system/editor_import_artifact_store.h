/**************************************************************************/
/*  editor_import_artifact_store.h                                        */
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

#include "core/io/resource_importer.h"

// Optional storage for import results (e.g. a content-addressed store, see modules/cas).
// When registered, EditorFileSystem asks it whether files need to be reimported and whether an import result for the
// same inputs already exists, and hands it the files written by importers. `.import` files then only store settings.
class EditorImportArtifactStore {
	static inline EditorImportArtifactStore *singleton = nullptr;

public:
	static EditorImportArtifactStore *get_singleton() { return singleton; }
	static void set_singleton(EditorImportArtifactStore *p_store) { singleton = p_store; }

	// Replaces the `.import`/`.md5` based check of EditorFileSystem.
	virtual bool test_for_reimport(const String &p_path) = 0;

	// Called before importing. Returns true if the result of an import with the same inputs is available, in which case
	// the outputs are filled in and the importer isn't run.
	virtual bool fetch_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, List<String> &r_variants, List<String> &r_gen_files, Variant &r_metadata) = 0;

	// Called after importing, to take the files written by the importer to its import base path.
	virtual void store_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, const List<String> &p_variants, const List<String> &p_gen_files, const Variant &p_metadata, Error p_import_error, bool p_group = false) = 0;

	virtual ~EditorImportArtifactStore() {}
};
