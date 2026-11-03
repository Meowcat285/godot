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
// The volume is a grid of cells. Each cell holds n x n x n bricks of 4 x 4 x 4 probes, where n is 1 far from geometry
// and up to 2^(levels - 1) in cells that touch geometry, falling off by one level per neighboring cell.
// Probes that would mostly see the back of faces (inside walls) are replaced by their valid neighbors after the bake.
class LightmapProbeVolumeBuilder {
	struct Cell {
		uint32_t first_brick = 0;
		uint8_t level = 0;
	};

	AABB bounds;
	Vector3 origin;
	float cell_size = 1.0;
	float spacing = 1.0;
	int max_level = 0;
	Vector3i grid_size;
	LocalVector<Cell> cells;
	uint32_t brick_count = 0;

	LocalVector<Vector3> probe_positions;
	HashMap<Vector3i, uint32_t> probe_map; // Probe position in units of spacing -> index.

	_FORCE_INLINE_ uint32_t _cell_index(const Vector3i &p_cell) const { return (p_cell.z * grid_size.y + p_cell.y) * grid_size.x + p_cell.x; }
	_FORCE_INLINE_ Vector3i _cell_probe_origin(const Vector3i &p_cell) const { return p_cell * (3 << max_level); }

public:
	// Returns false if the volume would be too large; the error is printed.
	bool begin(const AABB &p_bounds, float p_min_spacing, int p_levels);
	void plot_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c);
	// Picks subdivisions and generates the probe positions, in the same space as the bounds.
	void finish_layout();

	const LocalVector<Vector3> &get_probe_positions() const { return probe_positions; }

	// p_sh holds 9 SH colors per probe, in the order of get_probe_positions(). The alpha of the first color is
	// the fraction of rays that hit a backface. Returns the data for LightmapGIData's probe volume.
	Dictionary build_data(const LocalVector<Color> &p_sh) const;

	// Unpacks data made by build_data(), for debug drawing. Positions are in the space of the bounds; each probe
	// gets 4 SH colors (L0, L1y, L1z, L1x) and the probe spacing around it. Each cell gives its bounds and its
	// bricks per axis.
	struct DebugCell {
		AABB aabb;
		int bricks_per_axis = 1;
	};
	static bool decode_data(const Dictionary &p_data, LocalVector<Vector3> &r_positions, LocalVector<Color> &r_sh, LocalVector<float> &r_spacing, LocalVector<DebugCell> &r_cells);
};
