/**************************************************************************/
/*  gcas_pack_explorer.h                                                  */
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

#include "core/io/file_access_gcas.h"
#include "scene/gui/dialogs.h"

class Button;
class EditorFileDialog;
class Label;
class Tree;
class TreeItem;

// Lists the files of a GCAS pack and extracts them, without mounting the pack.
class GCASPackExplorer : public AcceptDialog {
	GDCLASS(GCASPackExplorer, AcceptDialog);

	Label *path_label = nullptr;
	Label *info_label = nullptr;
	Tree *tree = nullptr;
	Button *extract_selected_button = nullptr;
	Button *extract_all_button = nullptr;
	EditorFileDialog *open_dialog = nullptr;
	EditorFileDialog *extract_dialog = nullptr;

	String pack_path;
	PackedSourceGCAS::PackInfo info;
	LocalVector<uint32_t> files_to_extract;

	void _open_pressed();
	void _pack_selected(const String &p_path);
	void _populate_tree();
	void _update_buttons();
	void _collect_files(TreeItem *p_item, LocalVector<bool> &r_marked);
	void _extract_pressed(bool p_all);
	void _extract_to(const String &p_dir);

public:
	void open_pack(const String &p_path);

	GCASPackExplorer();
};
