/**************************************************************************/
/*  lightmap_gi_gizmo_plugin.cpp                                          */
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

#include "lightmap_gi_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/lightmap_probe_volume_builder.h"

LightmapGIGizmoPlugin::LightmapGIGizmoPlugin() {
	// NOTE: This gizmo only renders solid spheres for previewing indirect lighting on dynamic objects.
	// The wireframe representation for LightmapProbe nodes is handled in LightmapProbeGizmoPlugin.
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/lightmap_lines");
	probe_size = EDITOR_GET("editors/3d_gizmos/gizmo_settings/lightmap_gi_probe_size");

	gizmo_color.a = 0.1;
	create_material("lightmap_lines", gizmo_color);
	create_material("lightmap_probe_volume_lines", Color(1, 1, 1, 0.3));

	Ref<StandardMaterial3D> mat = memnew(StandardMaterial3D);
	mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	// Fade out probes when camera gets too close to them.
	mat->set_distance_fade(StandardMaterial3D::DISTANCE_FADE_PIXEL_DITHER);
	mat->set_distance_fade_min_distance(probe_size * 0.5);
	mat->set_distance_fade_max_distance(probe_size * 1.5);
	mat->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	mat->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, false);
	mat->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);

	add_material("lightmap_probe_material", mat);

	create_icon_material("baked_indirect_light_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLightmapGI"), EditorStringName(EditorIcons)));
}

bool LightmapGIGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LightmapGI>(p_spatial) != nullptr;
}

String LightmapGIGizmoPlugin::get_gizmo_name() const {
	return "LightmapGI";
}

int LightmapGIGizmoPlugin::get_priority() const {
	return -1;
}

// Draws the adaptive probe volume: brick outlines colored by density (blue: 1 brick per cell axis, green: 2, yellow: 4,
// red: 8) and each probe as a small sphere shaded by its irradiance. Returns false if there is no volume.
static bool _draw_lightmap_probe_volume(EditorNode3DGizmo *p_gizmo, const Ref<LightmapGIData> &p_data, float p_probe_size, const Ref<Material> &p_lines_material, const Ref<Material> &p_probe_material) {
	LocalVector<Vector3> positions;
	LocalVector<Color> sh;
	LocalVector<float> spacing;
	LocalVector<LightmapProbeVolumeBuilder::DebugCell> cells;
	// Drawing millions of probes would stall the editor; the brick outlines still show the layout.
	constexpr int MAX_DRAWN_PROBES = 50000;
	const Dictionary volume = p_data->_get_probe_volume_data();
	const bool draw_probes = int(volume.get("probe_count", 0)) <= MAX_DRAWN_PROBES && !Math::is_zero_approx(p_probe_size);
	if (!LightmapProbeVolumeBuilder::decode_data(volume, positions, sh, spacing, cells, draw_probes ? MAX_DRAWN_PROBES : 0)) {
		return false;
	}

	const Color level_colors[4] = { Color(0.3, 0.5, 1.0), Color(0.3, 1.0, 0.4), Color(1.0, 0.9, 0.2), Color(1.0, 0.3, 0.2) };
	Vector<Vector3> level_lines[4];
	for (const LightmapProbeVolumeBuilder::DebugCell &cell : cells) {
		const int level = CLAMP(int(Math::log2(float(cell.bricks_per_axis))), 0, 3);
		for (int i = 0; i < 12; i++) {
			Vector3 a, b;
			cell.aabb.get_edge(i, a, b);
			level_lines[level].push_back(a);
			level_lines[level].push_back(b);
		}
	}
	for (int i = 0; i < 4; i++) {
		if (!level_lines[i].is_empty()) {
			p_gizmo->add_lines(level_lines[i], p_lines_material, false, level_colors[i]);
		}
	}

	if (positions.is_empty()) {
		return true;
	}

	// A low-poly UV sphere per probe, shaded with the probe's L1 irradiance in each vertex direction.
	constexpr int STACKS = 6;
	constexpr int SECTORS = 8;
	LocalVector<Vector3> unit_vertices;
	for (int i = 0; i <= STACKS; i++) {
		const float stack_angle = Math::PI / 2 - i * Math::PI / STACKS;
		for (int j = 0; j <= SECTORS; j++) {
			const float sector_angle = j * Math::PI * 2.0 / SECTORS;
			unit_vertices.push_back(Vector3(Math::cos(stack_angle) * Math::cos(sector_angle), Math::sin(stack_angle), Math::cos(stack_angle) * Math::sin(sector_angle)));
		}
	}
	LocalVector<int> unit_indices;
	for (int i = 0; i < STACKS; i++) {
		for (int j = 0; j < SECTORS; j++) {
			const int k1 = i * (SECTORS + 1) + j;
			const int k2 = k1 + SECTORS + 1;
			if (i != 0) {
				unit_indices.push_back(k1);
				unit_indices.push_back(k2);
				unit_indices.push_back(k1 + 1);
			}
			if (i != STACKS - 1) {
				unit_indices.push_back(k1 + 1);
				unit_indices.push_back(k2);
				unit_indices.push_back(k2 + 1);
			}
		}
	}
	// Same L1 convolution as the runtime.
	const float c0 = 0.886227;
	const float c1 = 1.023327;

	const int vertex_count = unit_vertices.size();
	const int index_count = unit_indices.size();
	Vector<Vector3> vertices;
	Vector<Color> colors;
	Vector<int> indices;
	vertices.resize(positions.size() * vertex_count);
	colors.resize(positions.size() * vertex_count);
	indices.resize(positions.size() * index_count);
	Vector3 *vw = vertices.ptrw();
	Color *cw = colors.ptrw();
	int *iw = indices.ptrw();
	for (uint32_t p = 0; p < positions.size(); p++) {
		// Keep probes well apart from each other, so dense areas stay readable.
		const float radius = MIN(p_probe_size * 0.25f, spacing[p] * 0.12f);
		const Color *c = &sh[p * 4];
		for (int i = 0; i < vertex_count; i++) {
			const Vector3 &n = unit_vertices[i];
			const Color light = c[0] * c0 + (c[1] * n.y + c[2] * n.z + c[3] * n.x) * c1;
			cw[p * vertex_count + i] = Color(MAX(light.r, 0.0f), MAX(light.g, 0.0f), MAX(light.b, 0.0f), 1.0);
			vw[p * vertex_count + i] = positions[p] + n * radius;
		}
		for (int i = 0; i < index_count; i++) {
			iw[p * index_count + i] = p * vertex_count + unit_indices[i];
		}
	}

	Array array;
	array.resize(RSE::ARRAY_MAX);
	array[RSE::ARRAY_VERTEX] = vertices;
	array[RSE::ARRAY_INDEX] = indices;
	array[RSE::ARRAY_COLOR] = colors;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, array, Array(), Dictionary(), 0);
	mesh->surface_set_material(0, p_probe_material);
	p_gizmo->add_mesh(mesh);
	return true;
}

void LightmapGIGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Ref<Material> icon = get_material("baked_indirect_light_icon", p_gizmo);
	LightmapGI *baker = Object::cast_to<LightmapGI>(p_gizmo->get_node_3d());
	Ref<LightmapGIData> data = baker->get_light_data();

	p_gizmo->clear();

	p_gizmo->add_unscaled_billboard(icon, 0.05);

	if (data.is_null() || !p_gizmo->is_selected()) {
		return;
	}

	Ref<Material> material_lines = get_material("lightmap_lines", p_gizmo);
	Ref<Material> material_probes = get_material("lightmap_probe_material", p_gizmo);

	// When there is a probe volume, it replaces the classic probes as the lighting dynamic objects get.
	if (_draw_lightmap_probe_volume(p_gizmo, data, probe_size, get_material("lightmap_probe_volume_lines", p_gizmo), material_probes)) {
		return;
	}

	Vector<Vector3> lines;
	HashSet<Vector2i> lines_found;

	Vector<Vector3> points = data->get_capture_points();
	if (points.is_empty()) {
		return;
	}
	Vector<Color> sh = data->get_capture_sh();
	if (sh.size() != points.size() * 9) {
		return;
	}

	Vector<int> tetrahedrons = data->get_capture_tetrahedra();

	for (int i = 0; i < tetrahedrons.size(); i += 4) {
		for (int j = 0; j < 4; j++) {
			for (int k = j + 1; k < 4; k++) {
				Vector2i pair;
				pair.x = tetrahedrons[i + j];
				pair.y = tetrahedrons[i + k];

				if (pair.y < pair.x) {
					SWAP(pair.x, pair.y);
				}
				if (lines_found.has(pair)) {
					continue;
				}
				lines_found.insert(pair);
				lines.push_back(points[pair.x]);
				lines.push_back(points[pair.y]);
			}
		}
	}

	p_gizmo->add_lines(lines, material_lines);

	int stack_count = 8;
	int sector_count = 16;

	float sector_step = (Math::PI * 2.0) / sector_count;
	float stack_step = Math::PI / stack_count;

	LocalVector<Vector3> vertices;
	LocalVector<Color> colors;
	LocalVector<int> indices;
	float radius = probe_size * 0.5f;

	if (!Math::is_zero_approx(radius)) {
		// L2 Spherical Harmonics evaluation and diffuse convolution coefficients.
		const float sh_coeffs[5] = {
			static_cast<float>(sqrt(1.0 / (4.0 * Math::PI)) * Math::PI),
			static_cast<float>(sqrt(3.0 / (4.0 * Math::PI)) * Math::PI * 2.0 / 3.0),
			static_cast<float>(sqrt(15.0 / (4.0 * Math::PI)) * Math::PI * 1.0 / 4.0),
			static_cast<float>(sqrt(5.0 / (16.0 * Math::PI)) * Math::PI * 1.0 / 4.0),
			static_cast<float>(sqrt(15.0 / (16.0 * Math::PI)) * Math::PI * 1.0 / 4.0)
		};

		for (int p = 0; p < points.size(); p++) {
			int vertex_base = vertices.size();
			Vector3 sh_col[9];
			for (int i = 0; i < 9; i++) {
				sh_col[i].x = sh[p * 9 + i].r;
				sh_col[i].y = sh[p * 9 + i].g;
				sh_col[i].z = sh[p * 9 + i].b;
			}

			for (int i = 0; i <= stack_count; ++i) {
				float stack_angle = Math::PI / 2 - i * stack_step; // starting from pi/2 to -pi/2
				float xy = radius * Math::cos(stack_angle); // r * cos(u)
				float z = radius * Math::sin(stack_angle); // r * sin(u)

				// add (sector_count+1) vertices per stack
				// the first and last vertices have same position and normal, but different tex coords
				for (int j = 0; j <= sector_count; ++j) {
					float sector_angle = j * sector_step; // starting from 0 to 2pi

					// vertex position (x, y, z)
					float x = xy * Math::cos(sector_angle); // r * cos(u) * cos(v)
					float y = xy * Math::sin(sector_angle); // r * cos(u) * sin(v)

					Vector3 n = Vector3(x, z, y);
					vertices.push_back(points[p] + n);
					n.normalize();

					const Vector3 light = (sh_coeffs[0] * sh_col[0] +
							sh_coeffs[1] * sh_col[1] * n.y +
							sh_coeffs[1] * sh_col[2] * n.z +
							sh_coeffs[1] * sh_col[3] * n.x +
							sh_coeffs[2] * sh_col[4] * n.x * n.y +
							sh_coeffs[2] * sh_col[5] * n.y * n.z +
							sh_coeffs[3] * sh_col[6] * (3.0 * n.z * n.z - 1.0) +
							sh_coeffs[2] * sh_col[7] * n.x * n.z +
							sh_coeffs[4] * sh_col[8] * (n.x * n.x - n.y * n.y));

					colors.push_back(Color(light.x, light.y, light.z, 1));
				}
			}

			for (int i = 0; i < stack_count; ++i) {
				int k1 = i * (sector_count + 1); // beginning of current stack
				int k2 = k1 + sector_count + 1; // beginning of next stack

				for (int j = 0; j < sector_count; ++j, ++k1, ++k2) {
					// 2 triangles per sector excluding first and last stacks
					// k1 => k2 => k1+1
					if (i != 0) {
						indices.push_back(vertex_base + k1);
						indices.push_back(vertex_base + k2);
						indices.push_back(vertex_base + k1 + 1);
					}

					// k1+1 => k2 => k2+1
					if (i != (stack_count - 1)) {
						indices.push_back(vertex_base + k1 + 1);
						indices.push_back(vertex_base + k2);
						indices.push_back(vertex_base + k2 + 1);
					}
				}
			}
		}

		Array array;
		array.resize(RSE::ARRAY_MAX);
		array[RSE::ARRAY_VERTEX] = Vector<Vector3>(vertices);
		array[RSE::ARRAY_INDEX] = Vector<int>(indices);
		array[RSE::ARRAY_COLOR] = Vector<Color>(colors);

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, array, Array(), Dictionary(), 0); //no compression
		mesh->surface_set_material(0, material_probes);

		p_gizmo->add_mesh(mesh);
	}
}
