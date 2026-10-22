/**************************************************************************/
/*  cas_import_store.cpp                                                  */
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

#include "cas_import_store.h"

#include "../fastcdc.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_packed_byte_array.h"
#include "core/templates/sort_array.h"
#include "core/variant/variant_parser.h"
#include "core/version.h"
#include "editor/settings/editor_settings.h"

namespace {

constexpr const char *KEY_SCHEMA = "godot-cas-import-1";

bool variant_has_objects(const Variant &p_value, int p_depth = 0) {
	if (p_depth > 32) {
		return true;
	}
	switch (p_value.get_type()) {
		case Variant::OBJECT:
			return true;
		case Variant::ARRAY: {
			Array a = p_value;
			for (const Variant &v : a) {
				if (variant_has_objects(v, p_depth + 1)) {
					return true;
				}
			}
		} break;
		case Variant::DICTIONARY: {
			Dictionary d = p_value;
			for (const KeyValue<Variant, Variant> &kv : d) {
				if (variant_has_objects(kv.key, p_depth + 1) || variant_has_objects(kv.value, p_depth + 1)) {
					return true;
				}
			}
		} break;
		default:
			break;
	}
	return false;
}

String to_string(const Variant &p_value) {
	String s;
	VariantWriter::write_to_string(p_value, s, false);
	return s;
}

void list_files_recursive(const String &p_dir, LocalVector<String> &r_files) {
	if (!DirAccess::dir_exists_absolute(p_dir)) {
		return;
	}
	for (const String &f : DirAccess::get_files_at(p_dir)) {
		r_files.push_back(p_dir.path_join(f));
	}
	for (const String &d : DirAccess::get_directories_at(p_dir)) {
		list_files_recursive(p_dir.path_join(d), r_files);
	}
}

} // namespace

String CASImportStore::Manifest::to_text() const {
	Ref<ConfigFile> cf;
	cf.instantiate();
	cf->set_value("manifest", "key", key.to_string());
	cf->set_value("manifest", "importer", importer);
	cf->set_value("manifest", "type", type);
	cf->set_value("manifest", "save_extension", save_extension);
	cf->set_value("manifest", "valid", valid);
	cf->set_value("manifest", "metadata", metadata);
	cf->set_value("manifest", "variants", variants);
	cf->set_value("manifest", "gen_files", gen_files);
	Dictionary outs;
	for (const KeyValue<String, CASHash> &E : outputs) {
		outs[E.key] = E.value.to_string();
	}
	cf->set_value("manifest", "outputs", outs);
	return cf->encode_to_text();
}

bool CASImportStore::Manifest::from_text(const String &p_text, bool p_untrusted) {
	Ref<ConfigFile> cf;
	cf.instantiate();
	if (cf->parse(p_text) != OK) {
		return false;
	}
	key = CASHash::from_string(cf->get_value("manifest", "key", String()));
	importer = cf->get_value("manifest", "importer", String());
	type = cf->get_value("manifest", "type", String());
	save_extension = cf->get_value("manifest", "save_extension", String());
	valid = cf->get_value("manifest", "valid", false);
	metadata = cf->get_value("manifest", "metadata", Variant());
	variants = cf->get_value("manifest", "variants", Vector<String>());
	gen_files = cf->get_value("manifest", "gen_files", Vector<String>());
	outputs.clear();
	Dictionary outs = cf->get_value("manifest", "outputs", Dictionary());
	for (const KeyValue<Variant, Variant> &kv : outs) {
		String suffix = kv.key;
		CASHash h = CASHash::from_string(kv.value);
		if (h.is_zero() || (p_untrusted && (suffix.contains_char('/') || suffix.contains_char('\\') || suffix.contains("..")))) {
			return false;
		}
		outputs[suffix] = h;
	}
	if (p_untrusted && variant_has_objects(metadata)) {
		return false;
	}
	return !key.is_zero();
}

//////////////////////////////////////////////////////////////////

String CASImportStore::get_object_path(const CASHash &p_hash, const String &p_suffix) const {
	String hex = p_hash.to_string();
	String path = root.path_join("objects").path_join(hex.substr(0, 2)).path_join(hex);
	String ext = p_suffix.get_extension();
	return ext.is_empty() ? path : path + "." + ext;
}

String CASImportStore::get_manifest_path(const CASHash &p_key) const {
	String hex = p_key.to_string();
	return root.path_join("manifests").path_join(hex.substr(0, 2)).path_join(hex + ".manifest");
}

void CASImportStore::_ensure_loaded() {
	{
		RWLockRead r(lock);
		if (loaded) {
			return;
		}
	}
	RWLockWrite w(lock);
	if (loaded) {
		return;
	}
	loaded = true;
	// Nothing to do without a project (e.g. in the project manager), or when running exported packs.
	enabled = !ProjectSettings::get_singleton()->get_resource_path().is_empty() && !ProjectSettings::get_singleton()->is_using_datapack();
	if (!enabled) {
		return;
	}
	root = "res://.godot/cas";
	imported_dir = ProjectSettings::get_singleton()->get_imported_files_path();
	// Projects run from the editor only read the store.
	read_only = !Engine::get_singleton()->is_editor_hint();
	_load_sources();
}

void CASImportStore::_load_sources() {
	String log_path = root.path_join("sources.log");
	if (!read_only) {
		DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(root));
	}

	Ref<FileAccess> f = FileAccess::open(log_path, FileAccess::READ);
	if (f.is_valid()) {
		while (!f->eof_reached()) {
			String line = f->get_line();
			Vector<String> parts = line.split("\t");
			if (parts.size() != 5) {
				continue;
			}
			Source s;
			s.modified_time = parts[1].to_int();
			s.size = parts[2].to_int();
			s.source_hash = CASHash::from_string(parts[3]);
			s.key = CASHash::from_string(parts[4]);
			sources[parts[0]] = s;
			base_to_source[ResourceFormatImporter::get_singleton()->get_import_base_path(parts[0])] = parts[0];
			source_log_entries++;
		}
		f.unref();
	}

	if (read_only) {
		return;
	}
	if (source_log_entries > sources.size() * 2 + 1024) {
		_compact_source_log();
	} else {
		source_log = FileAccess::open(log_path, FileAccess::exists(log_path) ? FileAccess::READ_WRITE : FileAccess::WRITE);
		if (source_log.is_valid()) {
			source_log->seek_end();
		}
	}
}

void CASImportStore::_compact_source_log() {
	String log_path = root.path_join("sources.log");
	String tmp_path = log_path + ".tmp";
	Ref<FileAccess> f = FileAccess::open(tmp_path, FileAccess::WRITE);
	ERR_FAIL_COND(f.is_null());
	for (const KeyValue<String, Source> &E : sources) {
		if (!FileAccess::exists(E.key)) {
			continue; // Removed or moved.
		}
		f->store_line(vformat("%s\t%d\t%d\t%s\t%s", E.key, E.value.modified_time, E.value.size, E.value.source_hash.to_string(), E.value.key.to_string()));
	}
	f.unref();
	source_log.unref();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	da->rename(tmp_path, log_path);
	source_log_entries = sources.size();
	source_log = FileAccess::open(log_path, FileAccess::READ_WRITE);
	if (source_log.is_valid()) {
		source_log->seek_end();
	}
}

void CASImportStore::_record_source(const String &p_path, const Source &p_source) {
	{
		RWLockWrite w(lock);
		sources[p_path] = p_source;
		base_to_source[ResourceFormatImporter::get_singleton()->get_import_base_path(p_path)] = p_path;
	}
	MutexLock l(log_mutex);
	if (source_log.is_valid()) {
		source_log->store_line(vformat("%s\t%d\t%d\t%s\t%s", p_path, p_source.modified_time, p_source.size, p_source.source_hash.to_string(), p_source.key.to_string()));
		source_log->flush();
		source_log_entries++;
	}
}

bool CASImportStore::_hash_file(const String &p_path, Source &r_source) {
	r_source.modified_time = FileAccess::get_modified_time(p_path);
	r_source.size = FileAccess::get_size(p_path);
	if (r_source.size < 0) {
		return false;
	}
	{
		RWLockRead r(lock);
		const Source *s = sources.getptr(p_path);
		if (s && s->modified_time == r_source.modified_time && s->size == r_source.size && !s->source_hash.is_zero()) {
			r_source.source_hash = s->source_hash;
			return true;
		}
	}
	return CASHash::compute_file(p_path, r_source.source_hash) == OK;
}

bool CASImportStore::_compute_key(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, Source &r_source) {
	if (!_hash_file(p_path, r_source)) {
		return false;
	}

	Blake3Context ctx;
	ctx.start();
	ctx.update_string(KEY_SCHEMA);
	ctx.update_u32(GODOT_VERSION_MAJOR);
	ctx.update_u32(GODOT_VERSION_MINOR);
	ctx.update_string(p_importer->get_importer_name());
	ctx.update_u32(p_importer->get_format_version());
	ctx.update_string(p_importer->get_import_settings_string());

	// Options, in the importer's order.
	List<ResourceImporter::ImportOption> options;
	p_importer->get_import_options(p_path, &options);
	for (const ResourceImporter::ImportOption &E : options) {
		const Variant *v = p_params.getptr(E.option.name);
		ctx.update_string(E.option.name);
		ctx.update_string(to_string(v ? *v : E.default_value));
	}
	ctx.update_string(to_string(p_generator_parameters));
	ctx.update(r_source.source_hash.data, CASHash::SIZE);

	// Other files the import depends on.
	HashSet<String> deps;
	p_importer->get_build_dependencies(p_path, &deps);
	LocalVector<String> sorted_deps;
	for (const String &dep : deps) {
		sorted_deps.push_back(dep);
	}
	sorted_deps.sort();
	for (const String &dep : sorted_deps) {
		CASHash h;
		ctx.update_string(dep);
		if (CASHash::compute_file(dep, h) == OK) {
			ctx.update(h.data, CASHash::SIZE);
		}
	}

	if (!p_importer->is_import_path_independent()) {
		ctx.update_string(p_path);
		ctx.update_u64(uint64_t(p_uid));
	}

	r_source.key = ctx.finish();
	return true;
}

bool CASImportStore::_get_manifest(const CASHash &p_key, Manifest &r_manifest) {
	{
		RWLockRead r(lock);
		const Manifest *m = manifests.getptr(p_key);
		if (m) {
			r_manifest = *m;
			return true;
		}
	}
	String path = get_manifest_path(p_key);
	if (!FileAccess::exists(path)) {
		return false;
	}
	Manifest m;
	if (!m.from_text(FileAccess::get_file_as_string(path), false) || m.key != p_key) {
		return false;
	}
	RWLockWrite w(lock);
	manifests[p_key] = m;
	r_manifest = m;
	return true;
}

Error CASImportStore::_save_manifest(const Manifest &p_manifest) {
	String path = get_manifest_path(p_manifest.key);
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(f.is_null(), err, vformat("Can't write import manifest \"%s\".", path));
	f->store_string(p_manifest.to_text());
	f.unref();

	RWLockWrite w(lock);
	manifests[p_manifest.key] = p_manifest;
	return OK;
}

bool CASImportStore::_is_manifest_usable(const Manifest &p_manifest) {
	for (const KeyValue<String, CASHash> &E : p_manifest.outputs) {
		if (!FileAccess::exists(get_object_path(E.value, E.key))) {
			return false;
		}
	}
	// Generated files outside of the imported files directory are project files, written next to the source.
	for (const String &gen : p_manifest.gen_files) {
		if (!gen.begins_with(imported_dir) && !FileAccess::exists(gen)) {
			return false;
		}
	}
	return true;
}

void CASImportStore::_clear_staged_files(const String &p_path) {
	String base = ResourceFormatImporter::get_singleton()->get_import_base_path(p_path);
	String prefix = base.get_file() + ".";
	Ref<DirAccess> da = DirAccess::open(imported_dir);
	if (da.is_null()) {
		return;
	}
	for (const String &f : da->get_files()) {
		if (f.begins_with(prefix)) {
			da->remove(f);
		}
	}
}

bool CASImportStore::_split_logical_path(const String &p_logical_path, String &r_base, String &r_suffix) const {
	if (p_logical_path.get_base_dir() != imported_dir) {
		return false;
	}
	// Import base paths are "<file name>-<md5 of path>" (see ResourceFormatImporter::get_import_base_path()).
	String name = p_logical_path.get_file();
	for (int i = name.length() - 33; i >= 0; i--) {
		if (name[i] != '-' || (i + 33 < name.length() && name[i + 33] != '.')) {
			continue;
		}
		bool hex = true;
		for (int j = i + 1; j < i + 33 && hex; j++) {
			char32_t c = name[j];
			hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		}
		if (hex) {
			r_base = imported_dir.path_join(name.substr(0, i + 33));
			r_suffix = name.substr(i + 33);
			return true;
		}
	}
	return false;
}

//////////////////////////////////////////////////////////////////

bool CASImportStore::test_for_reimport(const String &p_path) {
	_ensure_loaded();
	ERR_FAIL_COND_V(!enabled, true);

	Ref<ConfigFile> cf;
	cf.instantiate();
	if (cf->load(p_path + ".import") != OK) {
		return true;
	}
	String importer_name = cf->get_value("remap", "importer", String());
	if (importer_name == "keep" || importer_name == "skip") {
		return false;
	}
	Ref<ResourceImporter> importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(importer_name);
	if (importer.is_null() || !cf->has_section_key("remap", "uid")) {
		return true;
	}
	ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(cf->get_value("remap", "uid"));

	HashMap<StringName, Variant> params;
	if (cf->has_section("params")) {
		for (const String &k : cf->get_section_keys("params")) {
			params[k] = cf->get_value("params", k);
		}
	}

	Source source;
	if (!_compute_key(p_path, importer, params, uid, cf->get_value("remap", "generator_parameters", Variant()), source)) {
		return true;
	}

	{
		RWLockRead r(lock);
		const Source *current = sources.getptr(p_path);
		if (!current || current->key != source.key) {
			return true; // Inputs changed, the store may still have a result for them.
		}
	}

	Manifest m;
	if (!_get_manifest(source.key, m)) {
		return true;
	}
	if (!m.valid) {
		return false; // Failed import, let the user reimport manually to avoid import loops.
	}
	if (!_is_manifest_usable(m)) {
		return true;
	}
	if (!importer->are_import_settings_valid(p_path, m.metadata)) {
		return true;
	}
	return false;
}

bool CASImportStore::fetch_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, List<String> &r_variants, List<String> &r_gen_files, Variant &r_metadata) {
	_ensure_loaded();
	ERR_FAIL_COND_V(!enabled, false);

	Source source;
	if (!_compute_key(p_path, p_importer, p_params, p_uid, p_generator_parameters, source)) {
		_clear_staged_files(p_path);
		return false;
	}

	// Scene importers can write extracted resources (materials, meshes...) to the project, and script import plugins
	// can have any side effect, so they always run, unless they declare that their results can be shared.
	bool reusable = p_importer->get_import_order() < ResourceImporter::IMPORT_ORDER_SCENE && !p_importer->is_class("EditorImportPlugin");
	reusable = reusable || p_importer->can_share_import_cache();

	Manifest m;
	bool found = false;
	bool remote = false;
	if (reusable) {
		found = _get_manifest(source.key, m) && m.valid && _is_manifest_usable(m);
		if (!found && p_importer->can_share_import_cache()) {
			found = _fetch_from_backends(source.key, m) && m.valid && _is_manifest_usable(m);
			remote = found;
		}
	}
	if (!found) {
		stats_misses.increment();
		// Stale files from previous imports (or from before the store was used) would be taken as outputs.
		_clear_staged_files(p_path);
		return false;
	}

	for (const String &variant : m.variants) {
		r_variants.push_back(variant);
	}
	for (const String &gen : m.gen_files) {
		r_gen_files.push_back(gen);
	}
	r_metadata = m.metadata;
	_record_source(p_path, source);

	if (remote) {
		stats_remote_hits.increment();
	} else {
		stats_hits.increment();
	}
	print_verbose(vformat("CAS: Using %s import result for \"%s\" (%s).", remote ? "shared" : "cached", p_path, source.key.to_string()));
	return true;
}

void CASImportStore::store_import(const String &p_path, const Ref<ResourceImporter> &p_importer, const HashMap<StringName, Variant> &p_params, ResourceUID::ID p_uid, const Variant &p_generator_parameters, const List<String> &p_variants, const List<String> &p_gen_files, const Variant &p_metadata, Error p_import_error, bool p_group) {
	_ensure_loaded();
	ERR_FAIL_COND(!enabled);

	Source source;
	ERR_FAIL_COND_MSG(!_compute_key(p_path, p_importer, p_params, p_uid, p_generator_parameters, source), vformat("Can't compute import key for \"%s\".", p_path));

	Manifest m;
	m.key = source.key;
	m.importer = p_importer->get_importer_name();
	m.type = p_importer->get_resource_type();
	m.save_extension = p_importer->get_save_extension();
	m.valid = p_import_error == OK;
	m.metadata = p_metadata;
	for (const String &variant : p_variants) {
		m.variants.push_back(variant);
	}
	for (const String &gen : p_gen_files) {
		m.gen_files.push_back(gen);
	}

	// Move everything the importer wrote to its import base path to the store.
	String base = ResourceFormatImporter::get_singleton()->get_import_base_path(p_path);
	String prefix = base.get_file() + ".";
	Ref<DirAccess> da = DirAccess::open(imported_dir);
	ERR_FAIL_COND_MSG(da.is_null(), vformat("Can't open \"%s\".", imported_dir));
	for (const String &f : da->get_files()) {
		if (!f.begins_with(prefix)) {
			continue;
		}
		String staged = imported_dir.path_join(f);
		String suffix = "." + f.substr(prefix.length());
		if (suffix == ".md5") {
			da->remove(f);
			continue;
		}
		CASHash h;
		if (CASHash::compute_file(staged, h) != OK) {
			ERR_PRINT(vformat("Can't read imported file \"%s\".", staged));
			m.valid = false;
			continue;
		}
		String object = get_object_path(h, suffix);
		if (FileAccess::exists(object)) {
			da->remove(f);
		} else {
			DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(object.get_base_dir()));
			Error err = da->rename(staged, object);
			if (err != OK) {
				ERR_PRINT(vformat("Can't move imported file \"%s\" to \"%s\".", staged, object));
				m.valid = false;
				continue;
			}
		}
		m.outputs[suffix] = h;
	}

	_save_manifest(m);
	_record_source(p_path, source);

	if (m.valid && p_importer->can_share_import_cache()) {
		_upload_to_backends(m);
	}
}

Ref<FileAccess> CASImportStore::open_import_file(const String &p_source_path, Error *r_error) {
	_ensure_loaded();
	if (!enabled) {
		return Ref<FileAccess>();
	}

	CASHash key;
	{
		RWLockRead r(lock);
		const Source *s = sources.getptr(p_source_path);
		if (!s) {
			return Ref<FileAccess>();
		}
		key = s->key;
	}
	Manifest m;
	if (!_get_manifest(key, m)) {
		return Ref<FileAccess>();
	}

	Error err;
	String text = FileAccess::get_file_as_string(p_source_path + ".import", &err);
	if (err != OK) {
		if (r_error) {
			*r_error = err;
		}
		return Ref<FileAccess>();
	}

	// Rebuild the `[remap]` and `[deps]` keys that `.import` files contain without a store.
	String base = ResourceFormatImporter::get_singleton()->get_import_base_path(p_source_path);
	Vector<String> remap_lines;
	Array dest_files;
	if (m.valid) {
		if (m.save_extension.is_empty()) {
			// No path.
		} else if (!m.variants.is_empty()) {
			for (const String &variant : m.variants) {
				String path = base + "." + variant + "." + m.save_extension;
				remap_lines.push_back("path." + variant + "=" + Variant(path).get_construct_string());
				dest_files.push_back(path);
			}
		} else {
			String path = base + "." + m.save_extension;
			remap_lines.push_back("path=" + Variant(path).get_construct_string());
			dest_files.push_back(path);
		}
	} else {
		remap_lines.push_back("valid=false");
	}
	if (m.metadata.get_type() != Variant::NIL) {
		remap_lines.push_back("metadata=" + to_string(m.metadata));
	}
	Vector<String> deps_lines;
	if (!m.gen_files.is_empty()) {
		Array gen;
		for (const String &g : m.gen_files) {
			gen.push_back(g);
			dest_files.push_back(g);
		}
		deps_lines.push_back("files=" + to_string(gen));
	}
	if (!dest_files.is_empty()) {
		deps_lines.push_back("dest_files=" + to_string(dest_files));
	}

	Vector<String> lines = text.split("\n");
	auto section_end = [&](const String &p_section) -> int {
		int start = lines.find(p_section);
		if (start < 0) {
			return -1;
		}
		for (int i = start + 1; i < lines.size(); i++) {
			if (lines[i].begins_with("[")) {
				return i;
			}
		}
		return lines.size();
	};

	int remap_end = section_end("[remap]");
	if (remap_end < 0) {
		return Ref<FileAccess>();
	}
	for (int i = 0; i < remap_lines.size(); i++) {
		lines.insert(remap_end + i, remap_lines[i]);
	}
	int deps_end = section_end("[deps]");
	if (deps_end < 0) {
		lines.push_back("[deps]");
		deps_end = lines.size();
	}
	for (int i = 0; i < deps_lines.size(); i++) {
		lines.insert(deps_end + i, deps_lines[i]);
	}

	Ref<FileAccessPackedByteArray> fa;
	fa.instantiate();
	fa->open_custom(String("\n").join(lines).to_utf8_buffer());
	return fa;
}

String CASImportStore::resolve_artifact_path(const String &p_logical_path) {
	if (!p_logical_path.begins_with("res://.godot/")) {
		return p_logical_path;
	}
	_ensure_loaded();
	if (!enabled) {
		return p_logical_path;
	}

	String base;
	String suffix;
	if (!_split_logical_path(p_logical_path, base, suffix)) {
		return p_logical_path;
	}
	CASHash key;
	{
		RWLockRead r(lock);
		const String *source = base_to_source.getptr(base);
		if (!source) {
			return p_logical_path;
		}
		const Source *s = sources.getptr(*source);
		if (!s) {
			return p_logical_path;
		}
		key = s->key;
		const Manifest *m = manifests.getptr(key);
		if (m) {
			const CASHash *h = m->outputs.getptr(suffix);
			return h ? get_object_path(*h, suffix) : p_logical_path;
		}
	}
	Manifest m;
	if (!_get_manifest(key, m)) {
		return p_logical_path;
	}
	const CASHash *h = m.outputs.getptr(suffix);
	return h ? get_object_path(*h, suffix) : p_logical_path;
}

//////////////////////////////////////////////////////////////////

void CASImportStore::add_backend(const Ref<CASBackend> &p_backend) {
	ERR_FAIL_COND(p_backend.is_null());
	MutexLock l(backends_mutex);
	if (!backends.has(p_backend)) {
		backends.push_back(p_backend);
	}
}

void CASImportStore::remove_backend(const Ref<CASBackend> &p_backend) {
	MutexLock l(backends_mutex);
	backends.erase(p_backend);
}

void CASImportStore::update_backends_from_settings() {
	if (!EditorSettings::get_singleton()) {
		return;
	}
	String dir = EDITOR_GET("editor/import/cas/shared_cache_directory");
	bool shared_read_only = EDITOR_GET("editor/import/cas/shared_cache_read_only");

	MutexLock l(backends_mutex);
	for (int i = backends.size() - 1; i >= 0; i--) {
		if (backends[i]->has_meta("_cas_from_settings")) {
			backends.remove_at(i);
		}
	}
	if (!dir.is_empty()) {
		Ref<CASLocalDirectoryBackend> backend;
		backend.instantiate();
		backend->set_path(dir);
		backend->set_writable(!shared_read_only);
		backend->set_meta("_cas_from_settings", true);
		backends.push_back(backend);
	}
}

Ref<CASBackend> CASImportStore::_get_writable_backend() {
	MutexLock l(backends_mutex);
	for (const Ref<CASBackend> &b : backends) {
		if (b->is_writable()) {
			return b;
		}
	}
	return Ref<CASBackend>();
}

bool CASImportStore::_fetch_from_backends(const CASHash &p_key, Manifest &r_manifest) {
	Vector<Ref<CASBackend>> list;
	{
		MutexLock l(backends_mutex);
		list = backends;
	}
	String key = p_key.to_string();
	for (const Ref<CASBackend> &backend : list) {
		Vector<uint8_t> data = backend->get_manifest(key);
		if (data.is_empty()) {
			continue;
		}
		Manifest m;
		if (!m.from_text(String::utf8((const char *)data.ptr(), data.size()), true) || m.key != p_key) {
			WARN_PRINT(vformat("CAS: Ignoring invalid manifest %s from shared import cache.", key));
			continue;
		}

		bool ok = true;
		for (const KeyValue<String, CASHash> &E : m.outputs) {
			String object = get_object_path(E.value, E.key);
			if (FileAccess::exists(object)) {
				continue;
			}
			Vector<String> chunks = backend->get_object_chunks(E.value.to_string());
			Vector<uint8_t> object_data;
			for (const String &chunk : chunks) {
				Vector<uint8_t> chunk_data = backend->get_chunk(chunk);
				if (chunk_data.is_empty() || CASHash::compute(chunk_data).to_string() != chunk) {
					ok = false;
					break;
				}
				object_data.append_array(chunk_data);
			}
			// Everything is verified against its hash, so a shared cache can't inject different data.
			if (!ok || chunks.is_empty() || CASHash::compute(object_data) != E.value) {
				ok = false;
				break;
			}
			DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(object.get_base_dir()));
			String tmp = object + ".tmp";
			Ref<FileAccess> f = FileAccess::open(tmp, FileAccess::WRITE);
			if (f.is_null()) {
				ok = false;
				break;
			}
			f->store_buffer(object_data.ptr(), object_data.size());
			f.unref();
			Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
			if (da->rename(tmp, object) != OK) {
				da->remove(tmp);
				ok = FileAccess::exists(object);
				if (!ok) {
					break;
				}
			}
		}
		if (!ok) {
			continue;
		}
		_save_manifest(m);
		r_manifest = m;
		return true;
	}
	return false;
}

void CASImportStore::_upload_to_backends(const Manifest &p_manifest) {
	Ref<CASBackend> backend = _get_writable_backend();
	if (backend.is_null()) {
		return;
	}
	for (const KeyValue<String, CASHash> &E : p_manifest.outputs) {
		String hash = E.value.to_string();
		if (!backend->get_object_chunks(hash).is_empty()) {
			continue;
		}
		Vector<uint8_t> data = FileAccess::get_file_as_bytes(get_object_path(E.value, E.key));
		LocalVector<uint64_t> ends;
		FastCDC::split(data.ptr(), data.size(), FastCDC::Params(), ends);
		Vector<String> chunk_hashes;
		uint64_t start = 0;
		for (uint64_t end : ends) {
			chunk_hashes.push_back(CASHash::compute(data.ptr() + start, end - start).to_string());
			start = end;
		}
		Vector<uint8_t> present = backend->has_chunks(chunk_hashes);
		start = 0;
		for (int i = 0; i < chunk_hashes.size(); i++) {
			uint64_t end = ends[i];
			if (i >= present.size() || !present[i]) {
				Vector<uint8_t> chunk;
				chunk.resize_uninitialized(end - start);
				memcpy(chunk.ptrw(), data.ptr() + start, end - start);
				if (backend->put_chunk(chunk_hashes[i], chunk) != OK) {
					return;
				}
			}
			start = end;
		}
		if (backend->put_object_chunks(hash, chunk_hashes) != OK) {
			return;
		}
	}
	// The manifest goes last, so that other editors never see results with missing objects.
	backend->put_manifest(p_manifest.key.to_string(), p_manifest.to_text().to_utf8_buffer());
}

//////////////////////////////////////////////////////////////////

void CASImportStore::collect_garbage(uint64_t p_max_size) {
	_ensure_loaded();
	if (!enabled || read_only) {
		return;
	}

	// Everything used by the current imports is kept.
	HashSet<CASHash> live_keys;
	{
		RWLockRead r(lock);
		for (const KeyValue<String, Source> &E : sources) {
			if (FileAccess::exists(E.key)) {
				live_keys.insert(E.value.key);
			}
		}
	}
	HashSet<String> live_objects;
	for (const CASHash &key : live_keys) {
		Manifest m;
		if (_get_manifest(key, m)) {
			for (const KeyValue<String, CASHash> &E : m.outputs) {
				live_objects.insert(get_object_path(E.value, E.key));
			}
		}
	}

	struct Entry {
		String path;
		uint64_t time = 0;
		uint64_t size = 0;
		bool operator<(const Entry &p_other) const { return time < p_other.time; }
	};

	LocalVector<String> object_files;
	list_files_recursive(root.path_join("objects"), object_files);
	LocalVector<Entry> candidates;
	uint64_t total = 0;
	for (const String &path : object_files) {
		Entry e;
		e.path = path;
		e.size = FileAccess::get_size(path);
		e.time = FileAccess::get_modified_time(path);
		total += e.size;
		if (!live_objects.has(path)) {
			candidates.push_back(e);
		}
	}
	if (p_max_size > 0 && total <= p_max_size) {
		return;
	}

	candidates.sort();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	uint64_t removed = 0;
	for (const Entry &e : candidates) {
		if (p_max_size > 0 && total <= p_max_size) {
			break;
		}
		if (da->remove(e.path) == OK) {
			total -= e.size;
			removed++;
		}
	}

	// Manifests that aren't current and lost objects are useless.
	LocalVector<String> manifest_files;
	list_files_recursive(root.path_join("manifests"), manifest_files);
	for (const String &path : manifest_files) {
		CASHash key = CASHash::from_string(path.get_file().get_basename());
		if (live_keys.has(key)) {
			continue;
		}
		Manifest m;
		bool usable = _get_manifest(key, m) && _is_manifest_usable(m);
		if (p_max_size == 0 || !usable) {
			da->remove(path);
			RWLockWrite w(lock);
			manifests.erase(key);
		}
	}

	print_verbose(vformat("CAS: Removed %d unused imported files, import cache is now %s.", removed, String::humanize_size(total)));
}

Dictionary CASImportStore::get_stats() {
	Dictionary stats;
	stats["hits"] = stats_hits.get();
	stats["shared_hits"] = stats_remote_hits.get();
	stats["misses"] = stats_misses.get();
	RWLockRead r(lock);
	stats["sources"] = sources.size();
	return stats;
}

CASImportStore::CASImportStore() {
	singleton = this;
}

CASImportStore::~CASImportStore() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
