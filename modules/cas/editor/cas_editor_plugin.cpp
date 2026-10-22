/**************************************************************************/
/*  cas_editor_plugin.cpp                                                 */
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

#include "cas_editor_plugin.h"

#include "cas_import_store.h"

#include "core/object/callable_mp.h"
#include "editor/gui/editor_toaster.h"
#include "editor/settings/editor_settings.h"

void CASEditorPlugin::_clean_import_cache() {
	CASImportStore::get_singleton()->collect_garbage(0);
	EditorToaster::get_singleton()->popup_str(TTR("Removed imported files that aren't used by the current imports."));
}

void CASEditorPlugin::_settings_changed() {
	if (EditorSettings::get_singleton()->check_changed_settings_in_group("editor/import/cas")) {
		CASImportStore::get_singleton()->update_backends_from_settings();
	}
}

CASEditorPlugin::CASEditorPlugin() {
	EDITOR_DEF("editor/import/cas/max_cache_size_mb", 10240);
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::INT, "editor/import/cas/max_cache_size_mb", PROPERTY_HINT_RANGE, "0,1048576,1,or_greater,suffix:MiB"));
	EDITOR_DEF("editor/import/cas/shared_cache_directory", "");
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::STRING, "editor/import/cas/shared_cache_directory", PROPERTY_HINT_GLOBAL_DIR));
	EDITOR_DEF("editor/import/cas/shared_cache_read_only", false);

	CASImportStore::get_singleton()->update_backends_from_settings();
	EditorSettings::get_singleton()->connect("settings_changed", callable_mp(this, &CASEditorPlugin::_settings_changed));

	// Keep the import cache within its budget (imported files used by the project are always kept).
	int max_size_mb = EDITOR_GET("editor/import/cas/max_cache_size_mb");
	if (max_size_mb > 0) {
		CASImportStore::get_singleton()->collect_garbage(uint64_t(max_size_mb) * 1024 * 1024);
	}

	add_tool_menu_item(TTR("Clean Import Cache"), callable_mp(this, &CASEditorPlugin::_clean_import_cache));
}
