// Per-pixel sampling of the LightmapGI adaptive probe volume, for dynamic objects.
// Needs lightmap_probe_volume, lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP and SAMPLER_LINEAR_CLAMP.
// See RendererRD::LightStorage::lightmap_set_probe_volume() for the texture layout.

// How far to push the sample point along the normal, in units of the local probe spacing.
// It keeps surfaces from sampling probes behind them.
#define LIGHTMAP_PROBE_VOLUME_NORMAL_BIAS 0.33

// Finds the brick holding a grid space point. Returns the brick density in bricks per cell axis.
float lightmap_probe_volume_find_brick(vec3 p_grid_pos, out uint r_brick, out vec3 r_brick_fract) {
	ivec3 cell_pos = ivec3(p_grid_pos);
	uint indirection_base = lightmap_probe_volume.atlas_bricks.z * 12u;
	vec4 cell = texelFetch(sampler3D(lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP), ivec3(cell_pos.xy, cell_pos.z + int(indirection_base)), 0);
	uint index = uint(cell.x) + uint(cell.y) * 2048u;
	if (cell.z <= 1.0) {
		r_brick = index;
		r_brick_fract = clamp(fract(p_grid_pos), 0.0, 1.0);
		return 1.0;
	}

	// A split cell: look up the slot's brick in the cell's slot table.
	uint n = uint(cell.z);
	vec3 local = fract(p_grid_pos) * cell.z;
	uvec3 slot = min(uvec3(local), uvec3(n - 1u));
	uint t = index + (slot.z * n + slot.y) * n + slot.x;
	uvec3 texture_size = uvec3(textureSize(sampler3D(lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP), 0));
	ivec3 slot_texel = ivec3(t % texture_size.x, (t / texture_size.x) % texture_size.y, indirection_base + lightmap_probe_volume.grid_size.z + t / (texture_size.x * texture_size.y));
	vec4 entry = texelFetch(sampler3D(lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP), slot_texel, 0);
	uint size = max(uint(entry.z), 1u);
	r_brick = uint(entry.x) + uint(entry.y) * 2048u;
	r_brick_fract = clamp((local - vec3((slot / size) * size)) / float(size), 0.0, 1.0);
	return cell.z / float(size);
}

// Returns false if the point is outside the volume. r_irradiance is the L1 SH irradiance, before exposure.
bool lightmap_probe_volume_sample(vec3 p_view_vertex, vec3 p_view_normal, out vec3 r_irradiance) {
	r_irradiance = vec3(0.0);
	if (lightmap_probe_volume.grid_size.w == 0u) {
		return false;
	}

	const vec3 grid_size = vec3(lightmap_probe_volume.grid_size.xyz);
	vec4 view_vertex = vec4(p_view_vertex, 1.0);
	vec3 grid_pos = vec3(dot(lightmap_probe_volume.view_to_grid[0], view_vertex), dot(lightmap_probe_volume.view_to_grid[1], view_vertex), dot(lightmap_probe_volume.view_to_grid[2], view_vertex));
	if (any(lessThan(grid_pos, vec3(0.0))) || any(greaterThanEqual(grid_pos, grid_size))) {
		return false;
	}
	// The surface normal as a grid space direction, for the bias.
	vec3 grid_normal = normalize(vec3(dot(lightmap_probe_volume.view_to_grid[0].xyz, p_view_normal), dot(lightmap_probe_volume.view_to_grid[1].xyz, p_view_normal), dot(lightmap_probe_volume.view_to_grid[2].xyz, p_view_normal)));
	// The normal in the lightmap's orientation, which the SH is stored in.
	vec3 sh_normal = normalize(vec3(dot(lightmap_probe_volume.normal_to_grid[0].xyz, p_view_normal), dot(lightmap_probe_volume.normal_to_grid[1].xyz, p_view_normal), dot(lightmap_probe_volume.normal_to_grid[2].xyz, p_view_normal)));

	// Bricks of 4x4x4 probes are 1 / (3 * density) cells across.
	uint brick;
	vec3 brick_fract;
	float density = lightmap_probe_volume_find_brick(grid_pos, brick, brick_fract);
	grid_pos = clamp(grid_pos + grid_normal * (LIGHTMAP_PROBE_VOLUME_NORMAL_BIAS / (3.0 * density)), vec3(0.0), grid_size - 0.001);
	lightmap_probe_volume_find_brick(grid_pos, brick, brick_fract);

	uvec3 atlas_bricks = lightmap_probe_volume.atlas_bricks.xyz;
	uvec3 atlas_pos = uvec3(brick % atlas_bricks.x, (brick / atlas_bricks.x) % atlas_bricks.y, brick / (atlas_bricks.x * atlas_bricks.y)) * 4u;
	vec3 texture_size = vec3(textureSize(sampler3D(lightmap_probe_volume_texture, SAMPLER_LINEAR_CLAMP), 0));
	// Probes sit on texel centers, so trilinear filtering inside a brick interpolates between them.
	vec3 uvw = (vec3(atlas_pos) + 0.5 + brick_fract * 3.0) / texture_size;
	float slab = float(atlas_bricks.z * 4u) / texture_size.z;

	vec4 t0 = textureLod(sampler3D(lightmap_probe_volume_texture, SAMPLER_LINEAR_CLAMP), uvw, 0.0);
	vec4 t1 = textureLod(sampler3D(lightmap_probe_volume_texture, SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, slab), 0.0);
	vec4 t2 = textureLod(sampler3D(lightmap_probe_volume_texture, SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, slab * 2.0), 0.0);

	vec3 l0 = t0.rgb;
	vec3 l1y = vec3(t0.a, t1.rg);
	vec3 l1z = vec3(t1.ba, t2.r);
	vec3 l1x = t2.gba;

	// Same convolution constants as the per-object SH capture.
	r_irradiance = 0.886227 * l0 + 1.023327 * (l1y * sh_normal.y + l1z * sh_normal.z + l1x * sh_normal.x);
	r_irradiance = max(r_irradiance, vec3(0.0));
	return true;
}
