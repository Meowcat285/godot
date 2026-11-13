/**************************************************************************/
/*  lightmap_probe_volume_builder.cpp                                     */
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

#include "lightmap_probe_volume_builder.h"

#include "core/math/face3.h"
#include "core/math/math_funcs.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/templates/hash_set.h"
#include "core/variant/variant.h"

// Probes that see more backfaces than this are inside geometry.
static constexpr float BACKFACE_RATIO_INVALID = 0.25;
// Limits of the packed texture; 2048 is the smallest 3D texture size the renderer targets.
static constexpr int MAX_TEXTURE_SIZE = 2048;
static constexpr int MAX_ATLAS_BRICKS_XY = 128;
// Each cell needs at least 27 probes of its own, so more cells than this would not fit in MAX_PROBES anyway.
static constexpr int64_t MAX_CELLS = 64 * 1024;
// Keeps the lightmapper probe dispatch and buffer within limits (144 bytes per probe on the GPU).
static constexpr uint32_t MAX_PROBES = 2 * 1024 * 1024;
// Brick and slot indices are stored as two RGBA16F channels holding whole numbers below 2048.
static constexpr int64_t MAX_INDEX = 2048 * 2048;

bool LightmapProbeVolumeBuilder::begin(const AABB &p_bounds, float p_min_spacing, int p_levels) {
	bounds = p_bounds;
	spacing = MAX(p_min_spacing, 0.05f);
	max_level = CLAMP(p_levels, 1, 4) - 1;
	slots_per_axis = 1 << max_level;
	cell_size = spacing * 3 * slots_per_axis;

	for (int i = 0; i < 3; i++) {
		grid_size[i] = MAX(1, int(Math::ceil(bounds.size[i] / cell_size)));
	}
	// Center the grid on the bounds.
	origin = bounds.position - (Vector3(grid_size) * cell_size - bounds.size) * 0.5;

	if (grid_size.x > MAX_TEXTURE_SIZE || grid_size.y > MAX_TEXTURE_SIZE || grid_size.z > MAX_TEXTURE_SIZE / 2 || int64_t(grid_size.x) * grid_size.y * grid_size.z > MAX_CELLS) {
		ERR_PRINT(vformat("LightmapGI probe volume grid %v is too large. Increase the probe volume spacing or lower its subdivisions.", grid_size));
		return false;
	}

	cells.clear();
	cells.resize(grid_size.x * grid_size.y * grid_size.z);
	probe_positions.clear();
	probe_map.clear();
	brick_count = 0;
	slot_count = 0;
	return true;
}

void LightmapProbeVolumeBuilder::plot_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
	if (max_level == 0) {
		return;
	}
	const Face3 face(p_a, p_b, p_c);
	// Finest slots within one probe spacing of the triangle get the finest bricks, so the probes around it are dense on both sides.
	const float slot_size = cell_size / slots_per_axis;
	const Vector3i slot_grid = grid_size * slots_per_axis;
	AABB aabb = face.get_aabb().grow(spacing);
	Vector3i from = ((aabb.position - origin) / slot_size).floor();
	Vector3i to = ((aabb.get_end() - origin) / slot_size).floor();
	from = from.clamp(Vector3i(), slot_grid - Vector3i(1, 1, 1));
	to = to.clamp(Vector3i(), slot_grid - Vector3i(1, 1, 1));

	for (int z = from.z; z <= to.z; z++) {
		for (int y = from.y; y <= to.y; y++) {
			for (int x = from.x; x <= to.x; x++) {
				const Vector3i slot(x, y, z);
				const Vector3i cell_pos = slot / slots_per_axis;
				Cell &cell = cells[_cell_index(cell_pos)];
				const uint32_t slot_index = _slot_index(slot - cell_pos * slots_per_axis);
				if (!cell.near_geometry.is_empty() && cell.near_geometry[slot_index]) {
					continue;
				}
				AABB slot_aabb(origin + Vector3(slot) * slot_size, Vector3(slot_size, slot_size, slot_size));
				if (face.intersects_aabb(slot_aabb.grow(spacing))) {
					if (cell.near_geometry.is_empty()) {
						cell.near_geometry.resize_initialized(slots_per_axis * slots_per_axis * slots_per_axis);
					}
					cell.near_geometry[slot_index] = 1;
				}
			}
		}
	}
}

void LightmapProbeVolumeBuilder::_split(Cell &p_cell, const Vector3i &p_origin, int p_size) {
	bool near = false;
	for (int z = 0; z < p_size && !near; z++) {
		for (int y = 0; y < p_size && !near; y++) {
			for (int x = 0; x < p_size && !near; x++) {
				near = p_cell.near_geometry[_slot_index(p_origin + Vector3i(x, y, z))];
			}
		}
	}
	if (!near || p_size == 1) {
		Brick brick;
		brick.origin = p_origin;
		brick.size = p_size;
		p_cell.bricks.push_back(brick);
		return;
	}
	const int half = p_size / 2;
	for (int i = 0; i < 8; i++) {
		_split(p_cell, p_origin + Vector3i(i & 1, (i >> 1) & 1, (i >> 2) & 1) * half, half);
	}
}

void LightmapProbeVolumeBuilder::finish_layout() {
	// Split cells near geometry, assign bricks, and generate the probes they need, sharing probes between bricks and cells.
	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const Vector3i cell_pos(x, y, z);
				Cell &cell = cells[_cell_index(cell_pos)];
				cell.bricks.clear();
				if (cell.near_geometry.is_empty()) {
					cell.bricks.push_back(Brick{ Vector3i(), slots_per_axis });
				} else {
					_split(cell, Vector3i(), slots_per_axis);
					cell.first_slot = slot_count;
					slot_count += slots_per_axis * slots_per_axis * slots_per_axis;
				}
				cell.first_brick = brick_count;
				brick_count += cell.bricks.size();

				const Vector3i base = _cell_probe_origin(cell_pos);
				for (const Brick &brick : cell.bricks) {
					// Probe step in units of spacing; a brick spans 3 steps.
					const int step = brick.size;
					for (int pz = 0; pz < 4; pz++) {
						for (int py = 0; py < 4; py++) {
							for (int px = 0; px < 4; px++) {
								const Vector3i key = base + (brick.origin * 3 + Vector3i(px, py, pz) * step);
								if (!probe_map.has(key)) {
									probe_map.insert(key, probe_positions.size());
									probe_positions.push_back(origin + Vector3(key) * spacing);
								}
							}
						}
					}
				}
				if (probe_positions.size() > MAX_PROBES) {
					ERR_PRINT(vformat("LightmapGI probe volume needs more than %d probes. Increase the probe volume spacing or lower its subdivisions.", MAX_PROBES));
					probe_positions.clear();
					probe_map.clear();
					return;
				}
			}
		}
	}
}

Dictionary LightmapProbeVolumeBuilder::build_data(const LocalVector<Color> &p_sh) const {
	ERR_FAIL_COND_V(probe_positions.is_empty(), Dictionary());
	ERR_FAIL_COND_V(p_sh.size() != probe_positions.size() * 9, Dictionary());

	const uint32_t probe_count = probe_positions.size();

	// L1 SH (12 floats) per probe, and whether the probe is outside geometry.
	LocalVector<Color> sh;
	sh.resize(probe_count * 4);
	LocalVector<uint8_t> valid;
	valid.resize(probe_count);
	for (uint32_t i = 0; i < probe_count; i++) {
		for (int j = 0; j < 4; j++) {
			sh[i * 4 + j] = p_sh[i * 9 + j];
			sh[i * 4 + j].a = 0.0;
		}
		valid[i] = p_sh[i * 9].a <= BACKFACE_RATIO_INVALID;
	}

	// Replace probes inside geometry with a distance weighted average of valid neighbors, growing outward a few times
	// so thick walls fill in too. Neighbors are searched at every probe step, coarsest last.
	for (int pass = 0; pass < 4; pass++) {
		LocalVector<uint32_t> filled;
		for (const KeyValue<Vector3i, uint32_t> &E : probe_map) {
			if (valid[E.value]) {
				continue;
			}
			Color accum[4];
			float weight = 0.0;
			for (int level = max_level; level >= 0 && weight == 0.0; level--) {
				const int step = 1 << (max_level - level);
				for (int z = -1; z <= 1; z++) {
					for (int y = -1; y <= 1; y++) {
						for (int x = -1; x <= 1; x++) {
							const Vector3i offset(x, y, z);
							const uint32_t *nb = probe_map.getptr(E.key + offset * step);
							if (!nb || !valid[*nb] || offset == Vector3i()) {
								continue;
							}
							const float w = 1.0 / float(offset.length_squared() * step * step);
							for (int j = 0; j < 4; j++) {
								accum[j] += sh[*nb * 4 + j] * w;
							}
							weight += w;
						}
					}
				}
			}
			if (weight > 0.0) {
				for (int j = 0; j < 4; j++) {
					sh[E.value * 4 + j] = accum[j] / weight;
				}
				filled.push_back(E.value);
			}
		}
		if (filled.is_empty()) {
			break;
		}
		for (uint32_t index : filled) {
			valid[index] = true;
		}
	}

	// Pack the atlas, indirection grid and slot tables. See RendererRD::LightStorage::lightmap_set_probe_volume() for the layout.
	const int atlas_xy = CLAMP(int(Math::ceil(Math::pow(double(brick_count), 1.0 / 3.0))), 1, MAX_ATLAS_BRICKS_XY);
	const Vector3i atlas_bricks(atlas_xy, atlas_xy, int((brick_count + atlas_xy * atlas_xy - 1) / (atlas_xy * atlas_xy)));
	const int tex_width = MAX(atlas_bricks.x * 4, grid_size.x);
	const int tex_height = MAX(atlas_bricks.y * 4, grid_size.y);
	const int64_t slot_layer = int64_t(tex_width) * tex_height;
	const int slot_depth = int((slot_count + slot_layer - 1) / slot_layer);
	const Vector3i tex_size(tex_width, tex_height, atlas_bricks.z * 4 * 3 + grid_size.z + slot_depth);
	ERR_FAIL_COND_V_MSG(tex_size.z > MAX_TEXTURE_SIZE || slot_count > MAX_INDEX, Dictionary(), "LightmapGI probe volume is too large. Increase the probe volume spacing or lower its subdivisions.");

	Vector<uint8_t> data;
	data.resize(int64_t(tex_size.x) * tex_size.y * tex_size.z * 4 * sizeof(uint16_t));
	memset(data.ptrw(), 0, data.size());
	uint16_t *texels = (uint16_t *)data.ptrw();
	const int slab_depth = atlas_bricks.z * 4;
	const int slot_base = slab_depth * 3 + grid_size.z;
	auto texel = [&](int p_x, int p_y, int p_z) -> uint16_t * {
		return &texels[((int64_t(p_z) * tex_size.y + p_y) * tex_size.x + p_x) * 4];
	};
	auto write_entry = [&](uint16_t *p_texel, uint32_t p_index, int p_size, float p_w) {
		p_texel[0] = Math::make_half_float(float(p_index % 2048));
		p_texel[1] = Math::make_half_float(float(p_index / 2048));
		p_texel[2] = Math::make_half_float(float(p_size));
		p_texel[3] = Math::make_half_float(p_w);
	};

	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const Vector3i cell_pos(x, y, z);
				const Cell &cell = cells[_cell_index(cell_pos)];
				const bool split = !cell.near_geometry.is_empty();
				// A whole cell brick points at the brick; a split cell points at its slot table.
				write_entry(texel(x, y, slab_depth * 3 + z), split ? cell.first_slot : cell.first_brick, split ? slots_per_axis : 1, 1.0);

				const Vector3i base = _cell_probe_origin(cell_pos);
				for (uint32_t i = 0; i < cell.bricks.size(); i++) {
					const Brick &b = cell.bricks[i];
					const uint32_t brick = cell.first_brick + i;
					if (split) {
						// Each slot the brick covers points at the brick, with the brick size in slots.
						for (int sz = 0; sz < b.size; sz++) {
							for (int sy = 0; sy < b.size; sy++) {
								for (int sx = 0; sx < b.size; sx++) {
									const int64_t slot = cell.first_slot + _slot_index(b.origin + Vector3i(sx, sy, sz));
									write_entry(texel(slot % tex_size.x, (slot / tex_size.x) % tex_size.y, slot_base + slot / slot_layer), brick, b.size, 1.0);
								}
							}
						}
					}
					const Vector3i atlas_pos = Vector3i(brick % atlas_bricks.x, (brick / atlas_bricks.x) % atlas_bricks.y, brick / (atlas_bricks.x * atlas_bricks.y)) * 4;
					for (int pz = 0; pz < 4; pz++) {
						for (int py = 0; py < 4; py++) {
							for (int px = 0; px < 4; px++) {
								const Vector3i key = base + b.origin * 3 + Vector3i(px, py, pz) * b.size;
								const uint32_t *index = probe_map.getptr(key);
								ERR_FAIL_NULL_V(index, Dictionary());
								const Color *c = &sh[*index * 4];
								const float values[12] = { c[0].r, c[0].g, c[0].b, c[1].r, c[1].g, c[1].b, c[2].r, c[2].g, c[2].b, c[3].r, c[3].g, c[3].b };
								for (int slab = 0; slab < 3; slab++) {
									uint16_t *t = texel(atlas_pos.x + px, atlas_pos.y + py, slab * slab_depth + atlas_pos.z + pz);
									for (int k = 0; k < 4; k++) {
										t[k] = Math::make_half_float(values[slab * 4 + k]);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	// Maps the space of the bounds to grid space, where each cell is one unit.
	Transform3D grid_xform;
	grid_xform.basis.scale(Vector3(1, 1, 1) / cell_size);
	grid_xform.origin = -origin / cell_size;

	Dictionary d;
	d["version"] = DATA_VERSION;
	d["data"] = data;
	d["grid_size"] = grid_size;
	d["atlas_bricks"] = atlas_bricks;
	d["grid_xform"] = grid_xform;
	d["probe_count"] = probe_count;
	return d;
}

bool LightmapProbeVolumeBuilder::is_valid_layout(const Dictionary &p_data, Vector3i *r_texture_size) {
	if (int(p_data.get("version", 1)) != DATA_VERSION || p_data.get("data", Variant()).get_type() != Variant::PACKED_BYTE_ARRAY) {
		return false;
	}
	const Vector3i grid_size = p_data.get("grid_size", Vector3i());
	const Vector3i atlas_bricks = p_data.get("atlas_bricks", Vector3i());
	for (int i = 0; i < 3; i++) {
		if (grid_size[i] < 1 || grid_size[i] > MAX_TEXTURE_SIZE || atlas_bricks[i] < 1) {
			return false;
		}
	}
	if (int64_t(grid_size.x) * grid_size.y * grid_size.z > MAX_CELLS || atlas_bricks.x > MAX_ATLAS_BRICKS_XY || atlas_bricks.y > MAX_ATLAS_BRICKS_XY || int64_t(atlas_bricks.z) * 4 * 3 + grid_size.z > MAX_TEXTURE_SIZE) {
		return false;
	}
	// The slot tables take whatever depth is left after the atlas and indirection grid.
	const int64_t layer_bytes = int64_t(MAX(atlas_bricks.x * 4, grid_size.x)) * MAX(atlas_bricks.y * 4, grid_size.y) * 4 * int64_t(sizeof(uint16_t));
	const int64_t data_size = PackedByteArray(p_data["data"]).size();
	const int64_t depth = data_size / layer_bytes;
	if (data_size % layer_bytes != 0 || depth < int64_t(atlas_bricks.z) * 4 * 3 + grid_size.z || depth > MAX_TEXTURE_SIZE) {
		return false;
	}
	if (r_texture_size) {
		*r_texture_size = Vector3i(MAX(atlas_bricks.x * 4, grid_size.x), MAX(atlas_bricks.y * 4, grid_size.y), int(depth));
	}
	return true;
}

bool LightmapProbeVolumeBuilder::decode_data(const Dictionary &p_data, LocalVector<Vector3> &r_positions, LocalVector<Color> &r_sh, LocalVector<float> &r_spacing, LocalVector<DebugCell> &r_cells, uint32_t p_max_probes) {
	r_positions.clear();
	r_sh.clear();
	r_spacing.clear();
	r_cells.clear();
	if (!p_data.has("data")) {
		return false;
	}
	Vector3i tex_size;
	ERR_FAIL_COND_V(!is_valid_layout(p_data, &tex_size), false);
	const Vector<uint8_t> data = p_data["data"];
	const Vector3i grid_size = p_data["grid_size"];
	const Vector3i atlas_bricks = p_data["atlas_bricks"];
	const Transform3D grid_to_local = Transform3D(p_data["grid_xform"]).affine_inverse();

	const uint16_t *texels = (const uint16_t *)data.ptr();
	const int slab_depth = atlas_bricks.z * 4;
	const int slot_base = slab_depth * 3 + grid_size.z;
	const int64_t slot_layer = int64_t(tex_size.x) * tex_size.y;
	const int64_t total_slots = slot_layer * (tex_size.z - slot_base);
	const int64_t total_bricks = int64_t(atlas_bricks.x) * atlas_bricks.y * atlas_bricks.z;
	auto texel = [&](int64_t p_x, int64_t p_y, int64_t p_z) -> const uint16_t * {
		return &texels[((p_z * tex_size.y + p_y) * tex_size.x + p_x) * 4];
	};
	auto entry_index = [](const uint16_t *p_texel) -> uint32_t {
		return uint32_t(Math::half_to_float(p_texel[0])) + uint32_t(Math::half_to_float(p_texel[1])) * 2048;
	};
	const float cell_size = grid_to_local.basis.get_scale().x;

	// Brick outlines past this are replaced by cell outlines, so the gizmo doesn't stall.
	constexpr uint32_t MAX_DEBUG_BRICKS = 65536;
	LocalVector<DebugCell> cell_outlines;
	bool include_bricks = true;
	bool include_probes = p_max_probes > 0;
	// Probes shared by bricks and cells are drawn once. Keys are in units of the finest possible spacing.
	HashSet<Vector3i> seen;
	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const Vector3i cell_pos(x, y, z);
				const uint16_t *ind = texel(x, y, slab_depth * 3 + z);
				const uint32_t cell_index = entry_index(ind);
				const int n = int(Math::half_to_float(ind[2])); // Slots per axis; 1 if the cell is one brick.
				ERR_FAIL_COND_V(n != 1 && n != 2 && n != 4 && n != 8, false);
				ERR_FAIL_COND_V(n > 1 && int64_t(cell_index) + n * n * n > total_slots, false);

				DebugCell outline;
				outline.aabb = grid_to_local.xform(AABB(Vector3(cell_pos), Vector3(1, 1, 1)));
				outline.bricks_per_axis = 1;

				// Visit each leaf brick once, at its first slot.
				for (int sz = 0; sz < n; sz++) {
					for (int sy = 0; sy < n; sy++) {
						for (int sx = 0; sx < n; sx++) {
							const Vector3i slot(sx, sy, sz);
							uint32_t brick = cell_index;
							int size = 1;
							if (n > 1) {
								const int64_t t = cell_index + (sz * n + sy) * n + sx;
								const uint16_t *e = texel(t % tex_size.x, (t / tex_size.x) % tex_size.y, slot_base + t / slot_layer);
								brick = entry_index(e);
								size = int(Math::half_to_float(e[2]));
								// Reject data that would read outside the atlas.
								ERR_FAIL_COND_V(size != 1 && size != 2 && size != 4, false);
								ERR_FAIL_COND_V(size >= n, false);
								if (sx % size || sy % size || sz % size) {
									continue;
								}
							}
							ERR_FAIL_COND_V(int64_t(brick) >= total_bricks, false);
							const int bricks_per_axis = n / size;
							const Vector3i brick_origin = slot; // In slots.
							outline.bricks_per_axis = MAX(outline.bricks_per_axis, bricks_per_axis);

							if (include_bricks) {
								DebugCell cell;
								cell.aabb = grid_to_local.xform(AABB(Vector3(cell_pos) + Vector3(brick_origin) / float(n), Vector3(1, 1, 1) * (float(size) / n)));
								cell.bricks_per_axis = bricks_per_axis;
								r_cells.push_back(cell);
								if (r_cells.size() > MAX_DEBUG_BRICKS) {
									include_bricks = false;
									r_cells.clear();
								}
							}
							if (!include_probes) {
								continue;
							}

							const Vector3i atlas_pos = Vector3i(brick % atlas_bricks.x, (brick / atlas_bricks.x) % atlas_bricks.y, brick / (atlas_bricks.x * atlas_bricks.y)) * 4;
							const int key_step = 8 / n;
							for (int pz = 0; pz < 4 && include_probes; pz++) {
								for (int py = 0; py < 4 && include_probes; py++) {
									for (int px = 0; px < 4 && include_probes; px++) {
										// In units of a third of a slot.
										const Vector3i local = brick_origin * 3 + Vector3i(px, py, pz) * size;
										const Vector3i key = cell_pos * 24 + local * key_step;
										if (seen.has(key)) {
											continue;
										}
										if (r_positions.size() >= p_max_probes) {
											include_probes = false;
											r_positions.clear();
											r_sh.clear();
											r_spacing.clear();
											seen.clear();
											continue;
										}
										seen.insert(key);
										r_positions.push_back(grid_to_local.xform(Vector3(cell_pos) + Vector3(local) / float(3 * n)));
										r_spacing.push_back(cell_size * size / (3 * n));
										float values[12];
										for (int slab = 0; slab < 3; slab++) {
											const uint16_t *t = texel(atlas_pos.x + px, atlas_pos.y + py, slab * slab_depth + atlas_pos.z + pz);
											for (int k = 0; k < 4; k++) {
												values[slab * 4 + k] = Math::half_to_float(t[k]);
											}
										}
										for (int j = 0; j < 4; j++) {
											r_sh.push_back(Color(values[j * 3 + 0], values[j * 3 + 1], values[j * 3 + 2]));
										}
									}
								}
							}
						}
					}
				}
				cell_outlines.push_back(outline);
			}
		}
	}
	if (!include_bricks) {
		r_cells = cell_outlines;
	}
	return true;
}
