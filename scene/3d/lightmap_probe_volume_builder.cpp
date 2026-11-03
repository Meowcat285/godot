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

bool LightmapProbeVolumeBuilder::begin(const AABB &p_bounds, float p_min_spacing, int p_levels) {
	bounds = p_bounds;
	spacing = MAX(p_min_spacing, 0.05f);
	max_level = CLAMP(p_levels, 1, 4) - 1;
	cell_size = spacing * 3 * (1 << max_level);

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
	return true;
}

void LightmapProbeVolumeBuilder::plot_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
	if (max_level == 0) {
		return;
	}
	const Face3 face(p_a, p_b, p_c);
	// Cells within one probe spacing of the triangle get the finest level, so the probes around it are dense on both sides.
	AABB aabb = face.get_aabb().grow(spacing);
	Vector3i from = ((aabb.position - origin) / cell_size).floor();
	Vector3i to = ((aabb.get_end() - origin) / cell_size).floor();
	from = from.clamp(Vector3i(), grid_size - Vector3i(1, 1, 1));
	to = to.clamp(Vector3i(), grid_size - Vector3i(1, 1, 1));

	for (int z = from.z; z <= to.z; z++) {
		for (int y = from.y; y <= to.y; y++) {
			for (int x = from.x; x <= to.x; x++) {
				Cell &cell = cells[_cell_index(Vector3i(x, y, z))];
				if (cell.level == max_level) {
					continue;
				}
				AABB cell_aabb(origin + Vector3(x, y, z) * cell_size, Vector3(cell_size, cell_size, cell_size));
				if (face.intersects_aabb(cell_aabb.grow(spacing))) {
					cell.level = max_level;
				}
			}
		}
	}
}

void LightmapProbeVolumeBuilder::finish_layout() {
	// Fall off by one level per cell away from geometry, so neighboring cells differ by at most one level.
	for (int level = max_level; level > 1; level--) {
		for (int z = 0; z < grid_size.z; z++) {
			for (int y = 0; y < grid_size.y; y++) {
				for (int x = 0; x < grid_size.x; x++) {
					if (cells[_cell_index(Vector3i(x, y, z))].level != level) {
						continue;
					}
					for (int nz = MAX(z - 1, 0); nz <= MIN(z + 1, grid_size.z - 1); nz++) {
						for (int ny = MAX(y - 1, 0); ny <= MIN(y + 1, grid_size.y - 1); ny++) {
							for (int nx = MAX(x - 1, 0); nx <= MIN(x + 1, grid_size.x - 1); nx++) {
								Cell &n = cells[_cell_index(Vector3i(nx, ny, nz))];
								n.level = MAX(n.level, uint8_t(level - 1));
							}
						}
					}
				}
			}
		}
	}

	// Assign bricks and generate the probes they need, sharing probes between bricks and cells.
	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const Vector3i cell_pos(x, y, z);
				Cell &cell = cells[_cell_index(cell_pos)];
				const int n = 1 << cell.level;
				const int step = 1 << (max_level - cell.level); // Probe step in units of spacing.
				cell.first_brick = brick_count;
				brick_count += n * n * n;

				const Vector3i base = _cell_probe_origin(cell_pos);
				for (int pz = 0; pz <= 3 * n; pz++) {
					for (int py = 0; py <= 3 * n; py++) {
						for (int px = 0; px <= 3 * n; px++) {
							const Vector3i key = base + Vector3i(px, py, pz) * step;
							if (!probe_map.has(key)) {
								probe_map.insert(key, probe_positions.size());
								probe_positions.push_back(origin + Vector3(key) * spacing);
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

	// Pack the atlas and indirection grid. See RendererRD::LightStorage::lightmap_set_probe_volume() for the layout.
	const int atlas_xy = CLAMP(int(Math::ceil(Math::pow(double(brick_count), 1.0 / 3.0))), 1, MAX_ATLAS_BRICKS_XY);
	const Vector3i atlas_bricks(atlas_xy, atlas_xy, int((brick_count + atlas_xy * atlas_xy - 1) / (atlas_xy * atlas_xy)));
	const Vector3i tex_size(MAX(atlas_bricks.x * 4, grid_size.x), MAX(atlas_bricks.y * 4, grid_size.y), atlas_bricks.z * 4 * 3 + grid_size.z);
	ERR_FAIL_COND_V_MSG(tex_size.z > MAX_TEXTURE_SIZE, Dictionary(), "LightmapGI probe volume is too large. Increase the probe volume spacing or lower its subdivisions.");

	Vector<uint8_t> data;
	data.resize(int64_t(tex_size.x) * tex_size.y * tex_size.z * 4 * sizeof(uint16_t));
	memset(data.ptrw(), 0, data.size());
	uint16_t *texels = (uint16_t *)data.ptrw();
	const int slab_depth = atlas_bricks.z * 4;
	auto texel = [&](int p_x, int p_y, int p_z) -> uint16_t * {
		return &texels[((int64_t(p_z) * tex_size.y + p_y) * tex_size.x + p_x) * 4];
	};

	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const Vector3i cell_pos(x, y, z);
				const Cell &cell = cells[_cell_index(cell_pos)];
				const int n = 1 << cell.level;
				const int step = 1 << (max_level - cell.level);

				uint16_t *ind = texel(x, y, slab_depth * 3 + z);
				ind[0] = Math::make_half_float(float(cell.first_brick % 2048));
				ind[1] = Math::make_half_float(float(cell.first_brick / 2048));
				ind[2] = Math::make_half_float(float(n));
				ind[3] = Math::make_half_float(1.0);

				const Vector3i base = _cell_probe_origin(cell_pos);
				for (int bz = 0; bz < n; bz++) {
					for (int by = 0; by < n; by++) {
						for (int bx = 0; bx < n; bx++) {
							const uint32_t brick = cell.first_brick + (bz * n + by) * n + bx;
							const Vector3i atlas_pos = Vector3i(brick % atlas_bricks.x, (brick / atlas_bricks.x) % atlas_bricks.y, brick / (atlas_bricks.x * atlas_bricks.y)) * 4;
							for (int pz = 0; pz < 4; pz++) {
								for (int py = 0; py < 4; py++) {
									for (int px = 0; px < 4; px++) {
										const Vector3i key = base + (Vector3i(bx, by, bz) * 3 + Vector3i(px, py, pz)) * step;
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
		}
	}

	// Maps the space of the bounds to grid space, where each cell is one unit.
	Transform3D grid_xform;
	grid_xform.basis.scale(Vector3(1, 1, 1) / cell_size);
	grid_xform.origin = -origin / cell_size;

	Dictionary d;
	d["data"] = data;
	d["grid_size"] = grid_size;
	d["atlas_bricks"] = atlas_bricks;
	d["grid_xform"] = grid_xform;
	d["probe_count"] = probe_count;
	return d;
}

bool LightmapProbeVolumeBuilder::decode_data(const Dictionary &p_data, LocalVector<Vector3> &r_positions, LocalVector<Color> &r_sh, LocalVector<float> &r_spacing, LocalVector<DebugCell> &r_cells) {
	r_positions.clear();
	r_sh.clear();
	r_spacing.clear();
	r_cells.clear();
	if (!p_data.has("data")) {
		return false;
	}
	const Vector<uint8_t> data = p_data["data"];
	const Vector3i grid_size = p_data["grid_size"];
	const Vector3i atlas_bricks = p_data["atlas_bricks"];
	const Transform3D grid_to_local = Transform3D(p_data["grid_xform"]).affine_inverse();
	const Vector3i tex_size(MAX(atlas_bricks.x * 4, grid_size.x), MAX(atlas_bricks.y * 4, grid_size.y), atlas_bricks.z * 4 * 3 + grid_size.z);
	ERR_FAIL_COND_V(int64_t(data.size()) != int64_t(tex_size.x) * tex_size.y * tex_size.z * 4 * int64_t(sizeof(uint16_t)), false);

	const uint16_t *texels = (const uint16_t *)data.ptr();
	const int slab_depth = atlas_bricks.z * 4;
	auto texel = [&](int p_x, int p_y, int p_z) -> const uint16_t * {
		return &texels[((int64_t(p_z) * tex_size.y + p_y) * tex_size.x + p_x) * 4];
	};
	const float cell_size = grid_to_local.basis.get_scale().x;

	// Probes shared by bricks and cells are drawn once. Keys are in units of the finest possible spacing.
	HashSet<Vector3i> seen;
	for (int z = 0; z < grid_size.z; z++) {
		for (int y = 0; y < grid_size.y; y++) {
			for (int x = 0; x < grid_size.x; x++) {
				const uint16_t *ind = texel(x, y, slab_depth * 3 + z);
				const uint32_t first_brick = uint32_t(Math::half_to_float(ind[0])) + uint32_t(Math::half_to_float(ind[1])) * 2048;
				const int n = MAX(1, int(Math::half_to_float(ind[2])));

				DebugCell cell;
				cell.aabb = grid_to_local.xform(AABB(Vector3(x, y, z), Vector3(1, 1, 1)));
				cell.bricks_per_axis = n;
				r_cells.push_back(cell);

				const int key_step = 8 / MIN(n, 8);
				for (int bz = 0; bz < n; bz++) {
					for (int by = 0; by < n; by++) {
						for (int bx = 0; bx < n; bx++) {
							const uint32_t brick = first_brick + (bz * n + by) * n + bx;
							const Vector3i atlas_pos = Vector3i(brick % atlas_bricks.x, (brick / atlas_bricks.x) % atlas_bricks.y, brick / (atlas_bricks.x * atlas_bricks.y)) * 4;
							for (int pz = 0; pz < 4; pz++) {
								for (int py = 0; py < 4; py++) {
									for (int px = 0; px < 4; px++) {
										const Vector3i local = Vector3i(bx, by, bz) * 3 + Vector3i(px, py, pz);
										const Vector3i key = Vector3i(x, y, z) * 24 + local * key_step;
										if (seen.has(key)) {
											continue;
										}
										seen.insert(key);
										r_positions.push_back(grid_to_local.xform(Vector3(x, y, z) + Vector3(local) / float(3 * n)));
										r_spacing.push_back(cell_size / (3 * n));
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
			}
		}
	}
	return true;
}
