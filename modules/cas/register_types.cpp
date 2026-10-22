/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "cas_layout.h"
#include "gcas_packer.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/cas_backend.h"
#include "editor/cas_editor_plugin.h"
#include "editor/cas_import_store.h"
#include "editor/gcas_export.h"
#include "editor/plugins/editor_plugin.h"

static CASImportStore *import_store = nullptr;
#endif

void initialize_cas_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(CASLayout);
		GDREGISTER_CLASS(CASLayoutProvider);
		GDREGISTER_CLASS(GCASPacker);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/export/gcas/compression_level", PROPERTY_HINT_RANGE, "1,22,1"), 19);
		EditorExportPlatform::save_gcas_function = GCASExport::save_gcas;

		GDREGISTER_CLASS(CASBackend);
		GDREGISTER_CLASS(CASLocalDirectoryBackend);

		// Also used when running the project from the editor, to find imported files.
		import_store = memnew(CASImportStore);
		ResourceFormatImporter::artifact_provider = import_store;
		EditorImportArtifactStore::set_singleton(import_store);

		EditorPlugins::add_by_type<CASEditorPlugin>();
	}
#endif
}

void uninitialize_cas_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorExportPlatform::save_gcas_function = nullptr;
		if (import_store) {
			ResourceFormatImporter::artifact_provider = nullptr;
			EditorImportArtifactStore::set_singleton(nullptr);
			memdelete(import_store);
			import_store = nullptr;
		}
	}
#endif
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GCASPacker::clear_export_layout_providers();
	}
}
