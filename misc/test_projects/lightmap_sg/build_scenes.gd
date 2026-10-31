# Generates the test scenes. Run from this folder with:
#   godot --headless --script build_scenes.gd
# All textures are procedural (NoiseTexture2D), so nothing needs to be downloaded.
extends SceneTree


func _init() -> void:
	var room := _build_room()
	_save(room, "res://room.tscn")

	_save(_build_variant(true), "res://sg.tscn")
	_save(_build_variant(false), "res://sh.tscn")
	quit()


func _save(root: Node, path: String) -> void:
	var packed := PackedScene.new()
	packed.pack(root)
	var err := ResourceSaver.save(packed, path)
	assert(err == OK, "Failed to save %s" % path)
	root.free()


func _own(node: Node, owner_node: Node) -> void:
	for child in node.get_children():
		child.owner = owner_node
		_own(child, owner_node)


func _noise_texture(seed_value: int, frequency: float, from: Color, to: Color, seamless := true) -> NoiseTexture2D:
	var noise := FastNoiseLite.new()
	noise.seed = seed_value
	noise.frequency = frequency
	noise.fractal_octaves = 4
	var ramp := Gradient.new()
	ramp.set_color(0, from)
	ramp.set_color(1, to)
	var tex := NoiseTexture2D.new()
	tex.width = 256
	tex.height = 256
	tex.seamless = seamless
	tex.noise = noise
	tex.color_ramp = ramp
	return tex


func _material(albedo: Texture2D, roughness: Texture2D, metallic := 0.0, uv_scale := 1.0) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = albedo
	mat.roughness_texture = roughness
	mat.roughness = 1.0
	mat.metallic = metallic
	# Triplanar mapping keeps the textures from stretching across the box faces. uv_scale is in tiles per meter.
	mat.uv1_triplanar = true
	mat.uv1_scale = Vector3(uv_scale, uv_scale, uv_scale)
	return mat


func _box(parent: Node, name: String, size: Vector3, pos: Vector3, mat: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.add_uv2 = true
	mesh.material = mat
	var mi := MeshInstance3D.new()
	mi.name = name
	mi.mesh = mesh
	mi.position = pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mi)
	return mi


func _build_room() -> Node3D:
	var root := Node3D.new()
	root.name = "Room"

	# Glossy, slightly uneven floor; rough plaster walls; dark ceiling.
	var floor_mat := _material(
			_noise_texture(1, 0.02, Color(0.35, 0.33, 0.3), Color(0.6, 0.58, 0.55)),
			_noise_texture(2, 0.05, Color(0.15, 0.15, 0.15), Color(0.5, 0.5, 0.5)), 0.0, 0.25)
	var wall_mat := _material(
			_noise_texture(3, 0.08, Color(0.7, 0.68, 0.62), Color(0.85, 0.83, 0.78)),
			_noise_texture(4, 0.1, Color(0.7, 0.7, 0.7), Color(0.95, 0.95, 0.95)), 0.0, 0.5)
	var red_mat := _material(
			_noise_texture(5, 0.08, Color(0.6, 0.08, 0.05), Color(0.75, 0.15, 0.1)),
			_noise_texture(6, 0.1, Color(0.6, 0.6, 0.6), Color(0.8, 0.8, 0.8)), 0.0, 0.5)

	_box(root, "Floor", Vector3(12, 0.2, 12), Vector3(0, -0.1, 0), floor_mat)
	_box(root, "Ceiling", Vector3(12, 0.2, 12), Vector3(0, 4.1, 0), wall_mat)
	_box(root, "WallBack", Vector3(12, 4, 0.2), Vector3(0, 2, -6.1), wall_mat)
	_box(root, "WallLeft", Vector3(0.2, 4, 12), Vector3(-6.1, 2, 0), red_mat)
	# The right wall has a large opening so the sun and sky can light the room.
	_box(root, "WallRightA", Vector3(0.2, 4, 4), Vector3(6.1, 2, -4), wall_mat)
	_box(root, "WallRightB", Vector3(0.2, 4, 4), Vector3(6.1, 2, 4), wall_mat)
	_box(root, "WallRightTop", Vector3(0.2, 1, 4), Vector3(6.1, 3.5, 0), wall_mat)
	_box(root, "Pillar", Vector3(0.8, 4, 0.8), Vector3(-2.5, 2, -2.5), wall_mat)

	# A row of spheres from smooth to rough, metallic in front and dielectric behind.
	var flat_roughness := _noise_texture(7, 0.2, Color(1, 1, 1), Color(1, 1, 1))
	for i in 5:
		var roughness := 0.1 + 0.2 * i
		for metallic in [1.0, 0.0]:
			var mat := _material(_noise_texture(8 + i, 0.05, Color(0.9, 0.9, 0.9), Color(0.95, 0.95, 0.95)), flat_roughness, metallic)
			mat.roughness = roughness
			var mesh := SphereMesh.new()
			mesh.radius = 0.4
			mesh.height = 0.8
			mesh.add_uv2 = true
			mesh.material = mat
			var mi := MeshInstance3D.new()
			mi.name = "Sphere%d%s" % [i, "Metal" if metallic > 0.5 else "Dielectric"]
			mi.mesh = mesh
			mi.position = Vector3(-3.0 + 1.5 * i, 0.4, 1.0 if metallic > 0.5 else -0.5)
			mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
			# Small curved objects need more lightmap texels to show directional detail.
			mi.gi_lightmap_texel_scale = 4.0
			root.add_child(mi)

	# Lights, all baked as static so the lightmap is the only source of their light on static geometry.
	var warm := OmniLight3D.new()
	warm.name = "WarmLamp"
	warm.light_color = Color(1.0, 0.75, 0.5)
	warm.light_energy = 3.0
	warm.omni_range = 9.0
	warm.light_size = 0.2
	warm.light_bake_mode = Light3D.BAKE_STATIC
	warm.shadow_enabled = true
	warm.position = Vector3(-3.5, 3.0, 3.5)
	root.add_child(warm)

	var cool := SpotLight3D.new()
	cool.name = "CoolSpot"
	cool.light_color = Color(0.55, 0.7, 1.0)
	cool.light_energy = 6.0
	cool.spot_range = 10.0
	cool.spot_angle = 35.0
	cool.light_bake_mode = Light3D.BAKE_STATIC
	cool.shadow_enabled = true
	cool.position = Vector3(3.0, 3.8, -4.0)
	cool.rotation_degrees = Vector3(-60, 30, 0)
	root.add_child(cool)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 1.5
	sun.light_bake_mode = Light3D.BAKE_STATIC
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-35, 70, 0)
	root.add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = ProceduralSkyMaterial.new()
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = env
	root.add_child(world_env)

	var probe := ReflectionProbe.new()
	probe.name = "ReflectionProbe"
	probe.size = Vector3(12, 4.2, 12)
	probe.position = Vector3(0, 2, 0)
	probe.box_projection = true
	probe.interior = true
	root.add_child(probe)

	_own(root, root)
	return root


func _build_variant(spherical_gaussians: bool) -> Node3D:
	var root := Node3D.new()
	root.name = "SG" if spherical_gaussians else "SH"
	root.set_script(load("res://viewer.gd"))

	var room: Node3D = (load("res://room.tscn") as PackedScene).instantiate()
	root.add_child(room)

	var lightmap := LightmapGI.new()
	lightmap.name = "LightmapGI"
	lightmap.directional = true
	lightmap.use_spherical_gaussians = spherical_gaussians
	lightmap.interior = true
	lightmap.quality = LightmapGI.BAKE_QUALITY_HIGH
	lightmap.texel_scale = 2.0
	if not spherical_gaussians:
		# Show what the old approximation gives, for comparison.
		lightmap.specular_intensity = 1.0
	root.add_child(lightmap)

	var camera := Camera3D.new()
	camera.name = "Camera3D"
	var eye := Vector3(4.5, 1.7, 4.5)
	camera.transform = Transform3D(Basis.looking_at(Vector3(-1.0, 0.6, -0.5) - eye), eye)
	camera.fov = 65.0
	root.add_child(camera)

	room.owner = root
	lightmap.owner = root
	camera.owner = root
	return root
