# Viewer for the lightmap test scenes.
# Drag with the left mouse button to orbit. Press 1 for the baked lightmap, 2 for real-time lights only (reference).
# Command line (after "--"): --capture=<path.png> saves a screenshot and quits, --realtime starts in mode 2.
extends Node3D

const TARGET := Vector3(-1.0, 0.6, -0.5)

var _lightmap: LightmapGI
var _capture_path := ""
var _frames := 0


func _ready() -> void:
	_lightmap = $LightmapGI
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--capture="):
			_capture_path = arg.trim_prefix("--capture=")
		elif arg == "--realtime":
			_set_realtime(true)


func _exit_tree() -> void:
	# The LightmapGI is not freed with the scene while it is out of the tree.
	if not _lightmap.is_inside_tree():
		_lightmap.free()


func _set_realtime(enabled: bool) -> void:
	# Removing the LightmapGI from the tree unassigns the lightmap, so static lights render in real time.
	if enabled and _lightmap.is_inside_tree():
		remove_child(_lightmap)
	elif not enabled and not _lightmap.is_inside_tree():
		add_child(_lightmap)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_1:
			_set_realtime(false)
		elif event.keycode == KEY_2:
			_set_realtime(true)
	elif event is InputEventMouseMotion and event.button_mask & MOUSE_BUTTON_MASK_LEFT:
		var camera: Camera3D = $Camera3D
		var offset := camera.position - TARGET
		offset = offset.rotated(Vector3.UP, -event.relative.x * 0.005)
		camera.transform = Transform3D(Basis.looking_at(-offset), TARGET + offset)


func _process(_delta: float) -> void:
	if _capture_path.is_empty():
		return
	_frames += 1
	# Give the reflection probe and temporal effects time to settle.
	if _frames == 60:
		get_viewport().get_texture().get_image().save_png(_capture_path)
		get_tree().quit()
