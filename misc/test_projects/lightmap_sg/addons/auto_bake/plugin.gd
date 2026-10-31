# Bakes the lightmaps of sg.tscn and sh.tscn, then quits, when the editor is started with:
#   godot --editor --path . -- --auto-bake
# Use --auto-bake=res://a.tscn,res://b.tscn to bake other scenes.
@tool
extends EditorPlugin

const SCENES := ["res://sg.tscn", "res://sh.tscn"]


func _enter_tree() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "--auto-bake":
			_bake_all.call_deferred(SCENES)
		elif arg.begins_with("--auto-bake="):
			_bake_all.call_deferred(arg.trim_prefix("--auto-bake=").split(","))


func _bake_all(scenes: Array) -> void:
	# Let the editor finish its first scan and import before baking.
	await get_tree().create_timer(2.0).timeout
	var plugins := EditorInterface.get_base_control().get_tree().root.find_children("*", "LightmapGIEditorPlugin", true, false)
	if plugins.is_empty():
		push_error("LightmapGIEditorPlugin not found.")
		get_tree().quit(1)
		return

	for path in scenes:
		EditorInterface.open_scene_from_path(path)
		await get_tree().process_frame
		var lightmap: LightmapGI = EditorInterface.get_edited_scene_root().get_node("LightmapGI")
		if lightmap.light_data == null:
			# A data resource with a path lets the bake run without asking for a file.
			var data_path: String = path.get_basename() + ".lmbake"
			ResourceSaver.save(LightmapGIData.new(), data_path)
			lightmap.light_data = load(data_path)
		EditorInterface.edit_node(lightmap)
		await get_tree().process_frame
		print("Baking %s..." % path)
		plugins[0].call("_bake")
		EditorInterface.save_scene()
		print("Baked %s." % path)

	get_tree().quit()
