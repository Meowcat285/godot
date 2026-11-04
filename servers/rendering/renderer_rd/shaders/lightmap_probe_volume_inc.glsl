// Per-pixel sampling of the LightmapGI adaptive probe volume, for dynamic objects.
// Needs lightmap_probe_volume, lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP and SAMPLER_LINEAR_CLAMP.
// See RendererRD::LightStorage::lightmap_set_probe_volume() for the texture layout.

// How far to push the sample point along the normal, in units of the local probe spacing.
// It keeps surfaces from sampling probes behind them.
#define LIGHTMAP_PROBE_VOLUME_NORMAL_BIAS 0.33

vec4 lightmap_probe_volume_fetch_cell(vec3 p_grid_pos) {
	ivec3 cell = ivec3(p_grid_pos);
	cell.z += int(lightmap_probe_volume.atlas_bricks.z * 12u);
	return texelFetch(sampler3D(lightmap_probe_volume_texture, SAMPLER_NEAREST_CLAMP), cell, 0);
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

	// Each cell holds bricks_per_axis^3 bricks of 4x4x4 probes, so probes are 1 / (3 * bricks_per_axis) apart.
	vec4 cell = lightmap_probe_volume_fetch_cell(grid_pos);
	grid_pos = clamp(grid_pos + grid_normal * (LIGHTMAP_PROBE_VOLUME_NORMAL_BIAS / (3.0 * cell.z)), vec3(0.0), grid_size - 0.001);
	cell = lightmap_probe_volume_fetch_cell(grid_pos);

	uint bricks_per_axis = uint(cell.z);
	vec3 local = fract(grid_pos) * cell.z;
	uvec3 brick_pos = min(uvec3(local), uvec3(bricks_per_axis - 1u));
	vec3 brick_fract = clamp(local - vec3(brick_pos), 0.0, 1.0);
	uint brick = uint(cell.x) + uint(cell.y) * 2048u + (brick_pos.z * bricks_per_axis + brick_pos.y) * bricks_per_axis + brick_pos.x;

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
