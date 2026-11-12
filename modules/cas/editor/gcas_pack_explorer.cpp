/**************************************************************************/
/*  gcas_pack_explorer.cpp                                                */
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

#include "gcas_pack_explorer.h"

#include "core/io/dir_access.h"
#include "core/object/callable_mp.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"

// Pack paths are untrusted: only extract relative paths that stay inside the target directory.
static bool _is_safe_relative_path(const String &p_path) {
	if (p_path.is_empty() || p_path.begins_with("/") || p_path.contains_char('\\') || p_path.contains_char(':')) {
		return false;
	}
	for (const String &part : p_path.split("/")) {
		if (part.is_empty() || part == "." || part == "..") {
			return false;
		}
	}
	return true;
}

void GCASPackExplorer::_open_pressed() {
	open_dialog->popup_file_dialog();
}

void GCASPackExplorer::_pack_selected(const String &p_path) {
	open_pack(p_path);
}

void GCASPackExplorer::open_pack(const String &p_path) {
	pack_path = String();
	info = PackedSourceGCAS::PackInfo();
	tree->clear();
	path_label->set_text(p_path);

	Error err = PackedSourceGCAS::read_pack_info(p_path, info);
	if (err != OK) {
		info = PackedSourceGCAS::PackInfo();
		info_label->set_text(TTR("Not a valid GCAS pack, or it uses unsupported features (see the Output panel)."));
		_update_buttons();
		return;
	}
	pack_path = p_path;

	uint64_t total = 0;
	uint32_t count = 0;
	uint32_t external = 0;
	for (const PackedSourceGCAS::PackInfo::File &f : info.files) {
		if (f.removed) {
			continue;
		}
		count++;
		total += f.size;
		if (f.external) {
			external++;
		}
	}

	String text = vformat(TTR("%d files, %s. Created with Godot %d.%d.%d."), count, String::humanize_size(total), info.header.ver_major, info.header.ver_minor, info.header.ver_patch);
	if (!info.base_pack_ids.is_empty()) {
		text += " " + vformat(TTR("Patch for %d base pack(s): %d files use data of the base packs and can't be extracted."), info.base_pack_ids.size(), external);
	}
	info_label->set_text(text);

	_populate_tree();
	_update_buttons();
}

void GCASPackExplorer::_populate_tree() {
	tree->clear();
	TreeItem *root = tree->create_item();
	root->set_metadata(0, -1);

	struct SortEntry {
		String path;
		uint32_t index = 0;
		bool operator<(const SortEntry &p_other) const { return path.naturalnocasecmp_to(p_other.path) < 0; }
	};
	const LocalVector<PackedSourceGCAS::PackInfo::File> &files = info.files;
	LocalVector<SortEntry> order;
	order.resize(files.size());
	for (uint32_t i = 0; i < order.size(); i++) {
		order[i].path = files[i].path;
		order[i].index = i;
	}
	order.sort();

	const Ref<Texture2D> folder_icon = get_editor_theme_icon(SNAME("Folder"));
	const Ref<Texture2D> file_icon = get_editor_theme_icon(SNAME("File"));
	const Color disabled_color = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));

	HashMap<String, TreeItem *> folders;
	HashMap<TreeItem *, uint64_t> folder_sizes;
	for (const SortEntry &entry : order) {
		const uint32_t index = entry.index;
		const PackedSourceGCAS::PackInfo::File &f = files[index];
		String rel = f.path.trim_prefix("res://");
		Vector<String> parts = rel.split("/", false);
		if (parts.is_empty()) {
			continue;
		}

		TreeItem *parent_item = root;
		String folder_path;
		for (int i = 0; i < parts.size() - 1; i++) {
			folder_path += parts[i] + "/";
			TreeItem **existing = folders.getptr(folder_path);
			if (existing) {
				parent_item = *existing;
				continue;
			}
			TreeItem *folder = tree->create_item(parent_item);
			folder->set_text(0, parts[i]);
			folder->set_icon(0, folder_icon);
			folder->set_metadata(0, -1);
			folder->set_collapsed(true);
			folders[folder_path] = folder;
			parent_item = folder;
		}

		TreeItem *item = tree->create_item(parent_item);
		item->set_text(0, parts[parts.size() - 1]);
		item->set_icon(0, file_icon);
		item->set_metadata(0, index);
		item->set_tooltip_text(0, f.path);
		if (f.removed) {
			item->set_text(1, TTR("Removed"));
			item->set_tooltip_text(0, f.path + "\n" + TTR("Removed by this patch."));
		} else {
			item->set_text(1, String::humanize_size(f.size));
			for (TreeItem *p = parent_item; p != root; p = p->get_parent()) {
				folder_sizes[p] += f.size;
			}
		}
		if (f.external) {
			item->set_tooltip_text(0, f.path + "\n" + TTR("Uses data of a base pack, can't be extracted from this pack alone."));
		}
		if (f.removed || f.external) {
			item->set_custom_color(0, disabled_color);
			item->set_custom_color(1, disabled_color);
		}
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
	}

	for (const KeyValue<TreeItem *, uint64_t> &E : folder_sizes) {
		E.key->set_text(1, String::humanize_size(E.value));
		E.key->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
	}
}

void GCASPackExplorer::_update_buttons() {
	bool loaded = !pack_path.is_empty();
	extract_all_button->set_disabled(!loaded);
	extract_selected_button->set_disabled(!loaded || tree->get_next_selected(nullptr) == nullptr);
}

void GCASPackExplorer::_collect_files(TreeItem *p_item, LocalVector<bool> &r_marked) {
	int index = p_item->get_metadata(0);
	if (index >= 0) {
		r_marked[index] = true;
		return;
	}
	for (TreeItem *child = p_item->get_first_child(); child; child = child->get_next()) {
		_collect_files(child, r_marked);
	}
}

void GCASPackExplorer::_extract_pressed(bool p_all) {
	LocalVector<bool> marked;
	marked.resize(info.files.size());
	for (uint32_t i = 0; i < marked.size(); i++) {
		marked[i] = p_all;
	}
	if (!p_all) {
		for (TreeItem *item = tree->get_next_selected(nullptr); item; item = tree->get_next_selected(item)) {
			_collect_files(item, marked);
		}
	}

	files_to_extract.clear();
	for (uint32_t i = 0; i < marked.size(); i++) {
		if (marked[i] && !info.files[i].removed) {
			files_to_extract.push_back(i);
		}
	}
	if (files_to_extract.is_empty()) {
		EditorToaster::get_singleton()->popup_str(TTR("The selection has no files to extract."), EditorToaster::SEVERITY_WARNING);
		return;
	}
	extract_dialog->popup_file_dialog();
}

void GCASPackExplorer::_extract_to(const String &p_dir) {
	uint32_t extracted = 0;
	uint32_t failed = 0;
	uint32_t skipped = 0;
	bool canceled = false;
	Vector<uint8_t> buffer;
	buffer.resize_uninitialized(1024 * 1024);

	{
		EditorProgress progress("extract_gcas", TTR("Extracting Files"), files_to_extract.size(), true);
		for (uint32_t i = 0; i < files_to_extract.size(); i++) {
			const PackedSourceGCAS::PackInfo::File &f = info.files[files_to_extract[i]];
			if (progress.step(f.path, i)) {
				canceled = true;
				break;
			}
			if (f.external) {
				skipped++;
				continue;
			}
			String rel = f.path.trim_prefix("res://");
			if (!_is_safe_relative_path(rel)) {
				ERR_PRINT(vformat("Skipping GCAS file with an unsafe path: \"%s\".", f.path));
				failed++;
				continue;
			}

			String dest = p_dir.path_join(rel);
			Ref<FileAccess> src = PackedSourceGCAS::open_pack_info_file(pack_path, info, files_to_extract[i]);
			if (src.is_null() || DirAccess::make_dir_recursive_absolute(dest.get_base_dir()) != OK) {
				ERR_PRINT(vformat("Can't extract \"%s\" to \"%s\".", f.path, dest));
				failed++;
				continue;
			}
			// Write to a temporary file first, so a failed or canceled extraction keeps any existing file.
			const String tmp_dest = dest + ".extracting";
			Ref<FileAccess> dst = FileAccess::open(tmp_dest, FileAccess::WRITE);
			if (dst.is_null()) {
				ERR_PRINT(vformat("Can't write \"%s\".", tmp_dest));
				failed++;
				continue;
			}

			bool ok = true;
			uint64_t remaining = f.size;
			while (remaining > 0) {
				if (remaining < f.size && progress.step(f.path, i)) {
					canceled = true;
					ok = false;
					break;
				}
				uint64_t want = MIN(remaining, uint64_t(buffer.size()));
				uint64_t got = src->get_buffer(buffer.ptrw(), want);
				if (got != want || !dst->store_buffer(buffer.ptr(), got)) {
					ok = false;
					break;
				}
				remaining -= got;
			}
			dst.unref();
			if (ok && FileAccess::exists(dest) && DirAccess::remove_absolute(dest) != OK) {
				ok = false;
			}
			if (ok && DirAccess::rename_absolute(tmp_dest, dest) != OK) {
				ok = false;
			}
			if (ok) {
				extracted++;
			} else {
				DirAccess::remove_absolute(tmp_dest);
				if (canceled) {
					break;
				}
				ERR_PRINT(vformat("Can't extract \"%s\": read or write error.", f.path));
				failed++;
			}
		}
	}

	String message = vformat(TTR("Extracted %d files to \"%s\"."), extracted, p_dir);
	if (canceled) {
		message += " " + TTR("Canceled before extracting the other files.");
	}
	if (skipped > 0) {
		message += " " + vformat(TTR("Skipped %d files that use data of a base pack."), skipped);
	}
	if (failed > 0) {
		message += " " + vformat(TTR("%d files couldn't be extracted (see the Output panel)."), failed);
	}
	EditorToaster::get_singleton()->popup_str(message, failed > 0 ? EditorToaster::SEVERITY_WARNING : EditorToaster::SEVERITY_INFO);
}

GCASPackExplorer::GCASPackExplorer() {
	set_title(TTR("GCAS Pack Explorer"));
	set_ok_button_text(TTR("Close"));

	VBoxContainer *vbox = memnew(VBoxContainer);
	add_child(vbox);

	HBoxContainer *path_hbox = memnew(HBoxContainer);
	vbox->add_child(path_hbox);
	Button *open_button = memnew(Button);
	open_button->set_text(TTR("Open Pack..."));
	open_button->connect(SceneStringName(pressed), callable_mp(this, &GCASPackExplorer::_open_pressed));
	path_hbox->add_child(open_button);
	path_label = memnew(Label);
	path_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	path_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	path_hbox->add_child(path_label);

	info_label = memnew(Label);
	info_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	vbox->add_child(info_label);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_columns(2);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, TTR("Path"));
	tree->set_column_title(1, TTR("Size"));
	tree->set_column_expand(1, false);
	tree->set_column_custom_minimum_width(1, 100 * EDSCALE);
	tree->connect("multi_selected", callable_mp(this, &GCASPackExplorer::_update_buttons).unbind(3));
	vbox->add_child(tree);

	HBoxContainer *buttons = memnew(HBoxContainer);
	buttons->set_alignment(BoxContainer::ALIGNMENT_END);
	vbox->add_child(buttons);
	extract_selected_button = memnew(Button);
	extract_selected_button->set_text(TTR("Extract Selected..."));
	extract_selected_button->connect(SceneStringName(pressed), callable_mp(this, &GCASPackExplorer::_extract_pressed).bind(false));
	buttons->add_child(extract_selected_button);
	extract_all_button = memnew(Button);
	extract_all_button->set_text(TTR("Extract All..."));
	extract_all_button->connect(SceneStringName(pressed), callable_mp(this, &GCASPackExplorer::_extract_pressed).bind(true));
	buttons->add_child(extract_all_button);

	open_dialog = memnew(EditorFileDialog);
	open_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	open_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	open_dialog->add_filter("*.gcas", TTR("GCAS Pack"));
	open_dialog->add_filter("*", TTR("All Files"));
	open_dialog->connect("file_selected", callable_mp(this, &GCASPackExplorer::_pack_selected));
	add_child(open_dialog);

	extract_dialog = memnew(EditorFileDialog);
	extract_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	extract_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	extract_dialog->set_title(TTR("Extract To Folder"));
	extract_dialog->connect("dir_selected", callable_mp(this, &GCASPackExplorer::_extract_to));
	add_child(extract_dialog);

	_update_buttons();
}
