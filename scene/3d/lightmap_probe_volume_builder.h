/**************************************************************************/
/*  lightmap_probe_volume_builder.h                                       */
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

#include "core/math/aabb.h"
#include "core/math/color.h"
#include "core/math/vector3i.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

// Lays out an adaptive probe volume for LightmapGI and packs the baked probes for the renderer.
//
// The volume is a grid of cells, each the size of one brick of 4 x 4 x 4 probes at the coarsest level. A cell near
// geometry is split like an octree, down to 2^(levels - 1) bricks per axis, and only the bricks near geometry are split
// further. Probes that would mostly see the back of faces (inside walls) are replaced by their valid neighbors after the bake.
class LightmapProbeVolumeBuilder {
	// A leaf brick of a cell, in units of the cell's finest slots.
	struct Brick {
		Vector3i origin;
		int size = 1;
	};
	struct Cell {
		uint32_t first_brick = 0;
		uint32_t first_slot = 0; // Only used by split cells.
		LocalVector<uint8_t> near_geometry; // Per finest slot; empty if no geometry is near the cell.
		LocalVector<Brick> bricks;
	};

	AABB bounds;
	Vector3 origin;
	float cell_size = 1.0;
	float spacing = 1.0;
	int max_level = 0;
	int slots_per_axis = 1; // Finest bricks per cell axis.
	Vector3i grid_size;
	LocalVector<Cell> cells;
	uint32_t brick_count = 0;
	uint32_t slot_count = 0;

	LocalVector<Vector3> probe_positions;
	HashMap<Vector3i, uint32_t> probe_map; // Probe position in units of spacing -> index.

	_FORCE_INLINE_ uint32_t _cell_index(const Vector3i &p_cell) const { return (p_cell.z * grid_size.y + p_cell.y) * grid_size.x + p_cell.x; }
	_FORCE_INLINE_ Vector3i _cell_probe_origin(const Vector3i &p_cell) const { return p_cell * (3 << max_level); }
	_FORCE_INLINE_ uint32_t _slot_index(const Vector3i &p_slot) const { return (p_slot.z * slots_per_axis + p_slot.y) * slots_per_axis + p_slot.x; }
	void _split(Cell &p_cell, const Vector3i &p_origin, int p_size);

public:
	// Returns false if the volume would be too large; the error is printed.
	bool begin(const AABB &p_bounds, float p_min_spacing, int p_levels);
	void plot_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c);
	// Picks subdivisions and generates the probe positions, in the same space as the bounds.
	void finish_layout();

	const LocalVector<Vector3> &get_probe_positions() const { return probe_positions; }

	// Bumped when the packed layout changes, so older bakes are ignored instead of misread.
	static constexpr int DATA_VERSION = 2;

	// p_sh holds 9 SH colors per probe, in the order of get_probe_positions(). The alpha of the first color is
	// the fraction of rays that hit a backface. Returns the data for LightmapGIData's probe volume.
	Dictionary build_data(const LocalVector<Color> &p_sh) const;

	// Unpacks data made by build_data(), for debug drawing. Positions are in the space of the bounds; each probe
	// gets 4 SH colors (L0, L1y, L1z, L1x) and the probe spacing around it. Each debug cell gives its bounds and its
	// density as bricks per cell axis: one entry per leaf brick, or per cell if there are too many bricks to draw.
	struct DebugCell {
		AABB aabb;
		int bricks_per_axis = 1;
	};
	// True if the data has the current version, its sizes are positive and fit the limits the builder bakes with, so the
	// packed texture size can't overflow, and the data size matches. r_texture_size gets the size of the packed texture.
	static bool is_valid_layout(const Dictionary &p_data, Vector3i *r_texture_size = nullptr);
	// Decodes at most p_max_probes probes; past that, the probe outputs are left empty and only the bricks are returned.
	static bool decode_data(const Dictionary &p_data, LocalVector<Vector3> &r_positions, LocalVector<Color> &r_sh, LocalVector<float> &r_spacing, LocalVector<DebugCell> &r_cells, uint32_t p_max_probes);
};
