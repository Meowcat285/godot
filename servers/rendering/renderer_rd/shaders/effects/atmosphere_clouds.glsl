#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

// Volumetric clouds for AtmosphereSkyMaterial. One file, several modes:
// - MODE_NOISE_SHAPE / MODE_NOISE_DETAIL / MODE_NOISE_WEATHER generate the tileable noise textures once.
// - MODE_MARCH raymarches the cloud layer at half resolution.
// - MODE_TEMPORAL reprojects and blends the march with last frame's result.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#if defined(MODE_NOISE_SHAPE) || defined(MODE_NOISE_DETAIL) || defined(MODE_NOISE_WEATHER)

#ifdef MODE_NOISE_WEATHER
layout(rgba8, set = 0, binding = 0) uniform restrict writeonly image2D dest_noise;
#else
layout(rgba8, set = 0, binding = 0) uniform restrict writeonly image3D dest_noise;
#endif

layout(push_constant, std430) uniform NoiseParams {
	int size;
	int pad0;
	int pad1;
	int pad2;
}
noise_params;

uvec3 pcg3d(uvec3 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

vec3 hash33(ivec3 p, int seed) {
	return vec3(pcg3d(uvec3(p + ivec3(seed * 101, seed * 211, seed * 307)))) * (1.0 / 4294967295.0);
}

ivec3 wrap_cell(ivec3 c, int period) {
	return ((c % period) + period) % period;
}

// Tileable Worley noise, 1 at feature points and 0 far from them. p is in [0, period).
float worley(vec3 p, int period, int seed) {
	ivec3 cell = ivec3(floor(p));
	vec3 f = p - vec3(cell);
	float d = 1.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 o = ivec3(x, y, z);
				vec3 pt = vec3(o) + hash33(wrap_cell(cell + o, period), seed);
				vec3 diff = pt - f;
				d = min(d, dot(diff, diff));
			}
		}
	}
	return 1.0 - clamp(sqrt(d), 0.0, 1.0);
}

// Tileable gradient noise in [-1, 1].
float gradient_noise(vec3 p, int period, int seed) {
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float n[8];
	for (int k = 0; k < 8; k++) {
		ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
		vec3 g = normalize(hash33(wrap_cell(i0 + o, period), seed) * 2.0 - 1.0 + 1e-6);
		n[k] = dot(g, f - vec3(o));
	}
	float x0 = mix(n[0], n[1], u.x);
	float x1 = mix(n[2], n[3], u.x);
	float x2 = mix(n[4], n[5], u.x);
	float x3 = mix(n[6], n[7], u.x);
	return mix(mix(x0, x1, u.y), mix(x2, x3, u.y), u.z) * 1.5;
}

float worley_fbm(vec3 uvw, int period, int seed) {
	return worley(uvw * float(period), period, seed) * 0.625 + worley(uvw * float(period * 2), period * 2, seed) * 0.25 + worley(uvw * float(period * 4), period * 4, seed) * 0.125;
}

float gradient_fbm(vec3 uvw, int period, int octaves, int seed) {
	float sum = 0.0;
	float amp = 0.5;
	for (int i = 0; i < octaves; i++) {
		sum += gradient_noise(uvw * float(period), period, seed) * amp;
		period *= 2;
		amp *= 0.5;
	}
	return sum;
}

float remap(float v, float l0, float h0, float l1, float h1) {
	return l1 + (v - l0) * (h1 - l1) / max(h0 - l0, 1e-5);
}

void main() {
	ivec3 pos = ivec3(gl_GlobalInvocationID);
	int size = noise_params.size;
#ifdef MODE_NOISE_WEATHER
	if (any(greaterThanEqual(pos.xy, ivec2(size)))) {
		return;
	}
	// Weather map: R = coverage, G = cloud type. 2D slices of 3D noise keep it tileable.
	vec3 uvw = vec3((vec2(pos.xy) + 0.5) / float(size), 0.0);
	float coverage = clamp(gradient_fbm(uvw, 4, 5, 1) * 0.5 + 0.5, 0.0, 1.0);
	coverage = clamp(remap(coverage, 0.25, 0.75, 0.0, 1.0), 0.0, 1.0);
	coverage = mix(coverage, worley(uvw * 6.0, 6, 3), 0.25);
	float type = clamp(gradient_fbm(uvw + vec3(0.0, 0.0, 0.5), 2, 3, 2) * 0.5 + 0.5, 0.0, 1.0);
	imageStore(dest_noise, pos.xy, vec4(coverage, type, 0.0, 1.0));
#else
	pos.z = int(gl_GlobalInvocationID.z);
	if (any(greaterThanEqual(pos, ivec3(size)))) {
		return;
	}
	vec3 uvw = (vec3(pos) + 0.5) / float(size);
#ifdef MODE_NOISE_SHAPE
	// R: Perlin-Worley base shape, GBA: Worley FBM at increasing frequencies.
	float perlin = clamp(gradient_fbm(uvw, 4, 4, 5) * 0.5 + 0.5, 0.0, 1.0);
	float w0 = worley_fbm(uvw, 4, 7);
	float perlin_worley = clamp(remap(perlin, w0 - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
	imageStore(dest_noise, pos, vec4(perlin_worley, w0, worley_fbm(uvw, 8, 9), worley_fbm(uvw, 16, 11)));
#else
	// Detail erosion noise: Worley FBM at three frequencies.
	imageStore(dest_noise, pos, vec4(worley_fbm(uvw, 2, 13), worley_fbm(uvw, 4, 15), worley_fbm(uvw, 8, 17), 1.0));
#endif
#endif
}

#else // MODE_MARCH or MODE_TEMPORAL

#define PI 3.14159265358979

// Keep in sync with AtmosphereClouds::Params.
layout(set = 0, binding = 0, std140) uniform Params {
	mat4 inv_projection[2];
	mat4 projection[2];
	mat4 inv_view;
	mat4 prev_view_projection[2];

	vec4 camera; // xz: world position in km, y: planet frame radius in km, w: frame index.
	vec4 wind_offset; // xy: weather offset in km, zw: unused.
	vec4 sun_direction; // xyz: towards the light, w: 1 if enabled.
	vec4 sun_illuminance; // rgb, w: unused.
	vec4 layer; // x: ground radius, y: atmosphere top radius, z: cloud bottom radius, w: cloud top radius.
	vec4 shape; // x: coverage, y: cloud type, z: extinction per km, w: shape scale in km.
	vec4 detail; // x: detail scale in km, y: detail strength, z: weather scale in km, w: max distance in km.
	vec4 albedo; // rgb, w: output multiplier.
	vec4 rayleigh; // rgb: scattering, w: scale height.
	vec4 mie_scattering; // rgb, w: scale height.
	vec4 mie_extinction; // rgb, w: anisotropy.
	vec4 ozone; // rgb: absorption, w: altitude.
	vec4 misc; // x: ozone width, y: luminance multiplier of the radiance map, z: max sky lod, w: temporal blend.
	vec4 sky_border; // xy: radiance border size, z: depth available, w: weather map override.
	ivec4 sizes; // xy: march size, zw: full size.
	ivec4 steps; // x: view steps, y: light steps, z: view index count, w: history valid.
}
params;

layout(set = 0, binding = 1) uniform sampler linear_repeat;
layout(set = 0, binding = 2) uniform sampler linear_clamp;
layout(set = 0, binding = 3) uniform sampler linear_mipmap_clamp;

layout(push_constant, std430) uniform PushConstant {
	int view;
	int pad0;
	int pad1;
	int pad2;
}
pc;

float remap(float v, float l0, float h0, float l1, float h1) {
	return l1 + (v - l0) * (h1 - l1) / max(h0 - l0, 1e-5);
}

#ifdef MODE_MARCH

#include "../oct_inc.glsl"

layout(set = 1, binding = 0) uniform texture3D shape_noise;
layout(set = 1, binding = 1) uniform texture3D detail_noise;
layout(set = 1, binding = 2) uniform texture2D weather_map;
layout(set = 1, binding = 3) uniform texture2D transmittance_lut;
layout(set = 1, binding = 4) uniform texture2D multi_scattering_lut;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 1, binding = 5) uniform texture2DArray radiance;
#else
layout(set = 1, binding = 5) uniform texture2D radiance;
#endif
layout(set = 1, binding = 6) uniform texture2D depth_buffer;

layout(rgba16f, set = 2, binding = 0) uniform restrict writeonly image2D dest_color;
layout(r32f, set = 2, binding = 1) uniform restrict writeonly image2D dest_depth;

/* Atmosphere, matching AtmosphereSkyMaterial's shader. Distances in km. */

float ray_sphere(float r, float mu, float radius) {
	float c = (r - radius) * (r + radius);
	float b = r * mu;
	float disc = b * b - c;
	if (disc < 0.0) {
		return -1.0;
	}
	float s = sqrt(disc);
	if (-b - s >= 0.0) {
		return -b - s;
	}
	if (-b + s >= 0.0) {
		return -b + s;
	}
	return -1.0;
}

vec2 lut_uv(vec2 x, vec2 size) {
	return (x * (size - 1.0) + 0.5) / size;
}

vec3 transmittance_to_top(float r, float mu) {
	float ground_radius = params.layer.x;
	float top_radius = params.layer.y;
	float horizon = sqrt(max(1e-6, (top_radius - ground_radius) * (top_radius + ground_radius)));
	float rho = sqrt(max(0.0, (r - ground_radius) * (r + ground_radius)));
	float d = max(0.0, ray_sphere(r, mu, top_radius));
	float d_min = top_radius - r;
	float d_max = rho + horizon;
	vec2 x = clamp(vec2((d - d_min) / max(d_max - d_min, 1e-6), rho / horizon), 0.0, 1.0);
	return textureLod(sampler2D(transmittance_lut, linear_clamp), lut_uv(x, vec2(256.0, 64.0)), 0.0).rgb;
}

vec3 transmittance_to_light(float r, float mu) {
	bool hits_ground = mu < 0.0 && ray_sphere(r, mu, params.layer.x) >= 0.0;
	return hits_ground ? vec3(0.0) : transmittance_to_top(r, mu);
}

vec3 multi_scattering(float r, float mu) {
	vec2 x = clamp(vec2(mu * 0.5 + 0.5, (r - params.layer.x) / (params.layer.y - params.layer.x)), 0.0, 1.0);
	return textureLod(sampler2D(multi_scattering_lut, linear_clamp), lut_uv(x, vec2(32.0)), 0.0).rgb;
}

float henyey_greenstein(float cos_theta, float g) {
	float k = 1.0 + g * g - 2.0 * g * cos_theta;
	return (1.0 - g * g) / (4.0 * PI * k * sqrt(k));
}

// In-scattering and transmittance between the camera and a point at distance t_end, with few steps.
vec3 atmosphere_segment(vec3 origin, vec3 dir, float t_end, out vec3 transmittance) {
	const int STEPS = 8;
	vec3 luminance = vec3(0.0);
	transmittance = vec3(1.0);
	vec3 light_dir = params.sun_direction.xyz;
	float cos_theta = dot(dir, light_dir);
	float rayleigh_phase = 3.0 / (16.0 * PI) * (1.0 + cos_theta * cos_theta);
	float mie_phase = henyey_greenstein(cos_theta, params.mie_extinction.w);
	float prev_t = 0.0;
	for (int i = 0; i < STEPS; i++) {
		float x = float(i + 1) / float(STEPS);
		float next_t = t_end * x * x;
		float dt = next_t - prev_t;
		vec3 p = origin + dir * mix(prev_t, next_t, 0.3);
		prev_t = next_t;

		float r = length(p);
		float h = max(0.0, r - params.layer.x);
		float rayleigh_density = exp(-h / params.rayleigh.w);
		float mie_density = exp(-h / params.mie_scattering.w);
		float ozone_density = max(0.0, 1.0 - abs(h - params.ozone.w) / (params.misc.x * 0.5));
		vec3 rayleigh_s = params.rayleigh.rgb * rayleigh_density;
		vec3 mie_s = params.mie_scattering.rgb * mie_density;
		vec3 extinction = rayleigh_s + params.mie_extinction.rgb * mie_density + params.ozone.rgb * ozone_density;

		float mu_light = dot(p / r, light_dir);
		vec3 source = params.sun_illuminance.rgb * (transmittance_to_light(r, mu_light) * (rayleigh_s * rayleigh_phase + mie_s * mie_phase) + multi_scattering(r, mu_light) * (rayleigh_s + mie_s));

		vec3 step_transmittance = exp(-extinction * dt);
		luminance += transmittance * (source - source * step_transmittance) / max(extinction, vec3(1e-7));
		transmittance *= step_transmittance;
	}
	return luminance;
}

/* Clouds */

vec3 sample_radiance(vec3 dir) {
	vec2 uv = vec3_to_oct_with_border(dir, params.sky_border.xy);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
	return textureLod(sampler2DArray(radiance, linear_mipmap_clamp), vec3(uv, params.misc.z), 0.0).rgb;
#else
	return textureLod(sampler2D(radiance, linear_mipmap_clamp), uv, params.misc.z).rgb;
#endif
}

// Weather at a position in the cloud layer: x = coverage, y = cloud type.
vec2 sample_weather(vec2 xz) {
	vec2 w = textureLod(sampler2D(weather_map, linear_repeat), (xz - params.wind_offset.xy) / params.detail.z, 0.0).rg;
	if (params.sky_border.w > 0.5) {
		// User weather map: use it as is.
		return w;
	}
	// Built-in map: coverage slider shifts the threshold, cloud type slider biases the type.
	float coverage = clamp(remap(w.x, 1.0 - params.shape.x, 1.0, 0.0, 1.0), 0.0, 1.0) * smoothstep(0.0, 0.1, params.shape.x);
	float type = clamp(mix(params.shape.y, w.y, 0.35), 0.0, 1.0);
	return vec2(coverage, type);
}

// Rounds the bottom and top of the layer depending on cloud type (0 = stratus, 1 = cumulus).
float height_gradient(float h, float type) {
	float stratus = clamp(remap(h, 0.0, 0.07, 0.0, 1.0), 0.0, 1.0) * clamp(remap(h, 0.15, 0.3, 1.0, 0.0), 0.0, 1.0);
	float stratocumulus = clamp(remap(h, 0.0, 0.15, 0.0, 1.0), 0.0, 1.0) * clamp(remap(h, 0.3, 0.6, 1.0, 0.0), 0.0, 1.0);
	float cumulus = clamp(remap(h, 0.0, 0.1, 0.0, 1.0), 0.0, 1.0) * clamp(remap(h, 0.65, 0.95, 1.0, 0.0), 0.0, 1.0);
	return type < 0.5 ? mix(stratus, stratocumulus, type * 2.0) : mix(stratocumulus, cumulus, type * 2.0 - 1.0);
}

// Cloud extinction per km at a point. p is in the planet frame (km), xz is the matching world position (km).
float cloud_density(vec3 p, vec2 xz, bool p_detail) {
	float r = length(p);
	float h = (r - params.layer.z) / (params.layer.w - params.layer.z);
	if (h < 0.0 || h > 1.0) {
		return 0.0;
	}
	vec2 weather = sample_weather(xz);
	if (weather.x <= 0.0) {
		return 0.0;
	}

	vec3 uvw = vec3(xz.x - params.wind_offset.x, r - params.layer.x, xz.y - params.wind_offset.y) / params.shape.w;
	vec4 noise = textureLod(sampler3D(shape_noise, linear_repeat), uvw, 0.0);
	float fbm = noise.g * 0.625 + noise.b * 0.25 + noise.a * 0.125;
	float base = clamp(remap(noise.r, fbm - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
	base *= height_gradient(h, weather.y);
	base = clamp(remap(base, 1.0 - weather.x, 1.0, 0.0, 1.0), 0.0, 1.0) * weather.x;

	if (p_detail && base > 0.0) {
		vec3 d = textureLod(sampler3D(detail_noise, linear_repeat), uvw * params.shape.w / params.detail.x, 0.0).rgb;
		float detail_fbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
		// Wispy at the bottom, billowy at the top.
		float detail_mod = mix(detail_fbm, 1.0 - detail_fbm, clamp(h * 4.0, 0.0, 1.0));
		base = clamp(remap(base, detail_mod * params.detail.y, 1.0, 0.0, 1.0), 0.0, 1.0);
	}
	return base * params.shape.z;
}

// Distance along the ray to the first cloud layer segment, [t_min, t_max], in km. Returns false on a miss.
bool cloud_segment(vec3 origin, vec3 dir, out float t_min, out float t_max) {
	float r = length(origin);
	float mu = dot(origin / r, dir);
	float bottom = params.layer.z;
	float top = params.layer.w;
	float t_bottom = ray_sphere(r, mu, bottom);
	float t_top = ray_sphere(r, mu, top);
	if (r < bottom) {
		// Below: from the bottom of the layer to its top.
		bool hits_ground = mu < 0.0 && ray_sphere(r, mu, params.layer.x) >= 0.0;
		if (hits_ground || t_bottom < 0.0) {
			return false;
		}
		t_min = t_bottom;
		t_max = t_top;
	} else if (r > top) {
		// Above: from the top of the layer to the bottom, or to the far side of the top.
		if (t_top < 0.0) {
			return false;
		}
		t_min = t_top;
		vec3 entry = origin + dir * t_top;
		float entry_r = length(entry);
		float entry_mu = dot(entry / entry_r, dir);
		float t_inner = ray_sphere(entry_r, entry_mu, bottom);
		if (t_inner >= 0.0) {
			t_max = t_top + t_inner;
		} else {
			float b = entry_r * entry_mu;
			t_max = t_top + max(0.0, -b + sqrt(max(0.0, b * b - (entry_r - top) * (entry_r + top))));
		}
	} else {
		// Inside: to whichever boundary comes first.
		t_min = 0.0;
		t_max = (mu < 0.0 && t_bottom >= 0.0) ? t_bottom : t_top;
	}
	return t_max > t_min;
}

// Interleaved gradient noise, varied per frame.
float frame_noise(vec2 pixel) {
	pixel += float(int(params.camera.w) % 64) * vec2(5.588238);
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.sizes.xy))) {
		return;
	}
	int view = pc.view;

	vec2 uv = (vec2(pos) + 0.5) / vec2(params.sizes.xy);
	vec4 view_pos = params.inv_projection[view] * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec3 view_dir = normalize(view_pos.xyz / view_pos.w);
	vec3 dir = normalize(mat3(params.inv_view) * view_dir);

	// Distance to the scene in km: the farthest of the full resolution pixels this one covers.
	float scene_distance = 1e9;
	if (params.sky_border.z > 0.5) {
		ivec2 full = min(pos * 2, params.sizes.zw - 2);
		float depth = min(min(texelFetch(depth_buffer, full, 0).r, texelFetch(depth_buffer, full + ivec2(1, 0), 0).r),
				min(texelFetch(depth_buffer, full + ivec2(0, 1), 0).r, texelFetch(depth_buffer, full + ivec2(1, 1), 0).r));
		if (depth > 0.0) {
			vec4 p = params.inv_projection[view] * vec4(uv * 2.0 - 1.0, depth, 1.0);
			scene_distance = length(p.xyz / p.w) * 0.001;
		}
	}

	vec3 origin = vec3(0.0, params.camera.y, 0.0);
	float t_min, t_max;
	vec4 result = vec4(0.0, 0.0, 0.0, 1.0);
	float entry_distance = 1e9; // In meters, as the composite pass expects.
	if (cloud_segment(origin, dir, t_min, t_max)) {
		t_max = min(t_max, min(t_min + params.detail.w, scene_distance));
		if (t_max > t_min) {
			int steps = params.steps.x;
			float dt = (t_max - t_min) / float(steps);
			float t = t_min + dt * frame_noise(vec2(pos));

			vec3 light_dir = params.sun_direction.xyz;
			float cos_theta = dot(dir, light_dir);
			// Dual-lobe phase: strong forward scattering (silver lining) plus some back scattering.
			float phase = mix(henyey_greenstein(cos_theta, 0.8), henyey_greenstein(cos_theta, -0.3), 0.3);
			vec3 ambient_top = sample_radiance(vec3(0.0, 1.0, 0.0)) * params.misc.y;
			vec3 ambient_bottom = sample_radiance(vec3(0.0, -1.0, 0.0)) * params.misc.y;

			vec3 luminance = vec3(0.0);
			float transmittance = 1.0;
			float weighted_t = 0.0;
			float weight_sum = 0.0;
			for (int i = 0; i < steps; i++) {
				vec3 p = origin + dir * t;
				vec2 xz = params.camera.xz + p.xz;
				float density = cloud_density(p, xz, true);
				if (density > 0.0) {
					float r = length(p);
					vec3 up = p / r;
					if (entry_distance > 1e8) {
						entry_distance = max(0.0, t - dt) * 1000.0;
					}

					// Optical depth towards the sun, with growing steps.
					float light_od = 0.0;
					if (params.sun_direction.w > 0.5) {
						int light_steps = params.steps.y;
						float prev = 0.0;
						for (int j = 0; j < light_steps; j++) {
							float x = float(j + 1) / float(light_steps);
							float d = x * x * (params.layer.w - params.layer.z) * 0.4;
							vec3 lp = p + light_dir * mix(prev, d, 0.5);
							light_od += cloud_density(lp, params.camera.xz + lp.xz, false) * (d - prev);
							prev = d;
						}
					}

					// Multiple scattering approximation (Wrenninge 2015): octaves with weaker extinction and phase.
					float sun_scatter = 0.0;
					float a = 1.0;
					float b = 1.0;
					float c = 1.0;
					for (int o = 0; o < 4; o++) {
						float octave_phase = mix(henyey_greenstein(cos_theta, 0.8 * c), henyey_greenstein(cos_theta, -0.3 * c), 0.3);
						sun_scatter += a * octave_phase * exp(-light_od * b);
						a *= 0.6;
						b *= 0.3;
						c *= 0.5;
					}
					// Beer-powder: darkens the sunlit side of thin edges a little for depth.
					float powder = 1.0 - exp(-density * 2.0 * dt);
					sun_scatter *= mix(1.0, powder * 2.0, 0.25);

					float mu_light = dot(up, light_dir);
					vec3 sun = params.sun_illuminance.rgb * transmittance_to_light(r, mu_light) * sun_scatter;
					float height = clamp((r - params.layer.z) / (params.layer.w - params.layer.z), 0.0, 1.0);
					vec3 ambient = mix(ambient_bottom, ambient_top, height);

					vec3 source = (sun + ambient) * params.albedo.rgb;
					float step_transmittance = exp(-density * dt);
					// Energy conserving integration of the source over the step (scattering == extinction).
					luminance += transmittance * source * (1.0 - step_transmittance);
					weighted_t += t * transmittance * (1.0 - step_transmittance);
					weight_sum += transmittance * (1.0 - step_transmittance);
					transmittance *= step_transmittance;
					if (transmittance < 0.01) {
						break;
					}
				}
				t += dt;
			}

			if (weight_sum > 0.0) {
				// Haze between the camera and the cloud.
				float mean_t = weighted_t / weight_sum;
				vec3 haze_transmittance;
				vec3 haze = atmosphere_segment(origin, dir, mean_t, haze_transmittance);
				luminance = luminance * haze_transmittance + haze * (1.0 - transmittance);
			}
			result = vec4(luminance * params.albedo.w, transmittance);
		}
	}

	imageStore(dest_color, pos, result);
	imageStore(dest_depth, pos, vec4(entry_distance));
}

#endif // MODE_MARCH

#ifdef MODE_TEMPORAL

layout(set = 1, binding = 0) uniform texture2D march_color;
layout(set = 1, binding = 1) uniform texture2D march_depth;
layout(set = 1, binding = 2) uniform texture2D history_color;
layout(set = 1, binding = 3) uniform texture2D history_depth;

layout(rgba16f, set = 2, binding = 0) uniform restrict writeonly image2D dest_color;
layout(r32f, set = 2, binding = 1) uniform restrict writeonly image2D dest_depth;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.sizes.xy))) {
		return;
	}
	int view = pc.view;

	vec4 current = texelFetch(march_color, pos, 0);
	float current_depth = texelFetch(march_depth, pos, 0).r;

	// Neighbourhood bounds of the current march, to reject stale history.
	vec4 lo = current;
	vec4 hi = current;
	float min_depth = current_depth;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 p = clamp(pos + ivec2(x, y), ivec2(0), params.sizes.xy - 1);
			vec4 c = texelFetch(march_color, p, 0);
			lo = min(lo, c);
			hi = max(hi, c);
			min_depth = min(min_depth, texelFetch(march_depth, p, 0).r);
		}
	}

	vec4 result = current;
	float result_depth = min_depth;
	if (params.steps.w != 0) {
		// Reproject with the cloud distance, or a far distance for clear sky.
		vec2 uv = (vec2(pos) + 0.5) / vec2(params.sizes.xy);
		vec4 view_pos = params.inv_projection[view] * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
		vec3 dir = normalize(mat3(params.inv_view) * normalize(view_pos.xyz / view_pos.w));
		float reproject_distance = min_depth < 1e8 ? min_depth : 50000.0;
		vec3 world = params.inv_view[3].xyz + dir * reproject_distance;
		vec4 prev_clip = params.prev_view_projection[view] * vec4(world, 1.0);
		vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
		if (prev_clip.w > 0.0 && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			vec4 history = textureLod(sampler2D(history_color, linear_clamp), prev_uv, 0.0);
			// Depth isn't filterable on all hardware, and blending distances with "no cloud" makes no sense anyway.
			float history_depth_value = texelFetch(history_depth, clamp(ivec2(prev_uv * vec2(params.sizes.xy)), ivec2(0), params.sizes.xy - 1), 0).r;
			history = clamp(history, lo, hi);
			result = mix(current, history, params.misc.w);
			result_depth = min(min_depth, history_depth_value);
		}
	}

	imageStore(dest_color, pos, result);
	imageStore(dest_depth, pos, vec4(result_depth));
}

#endif // MODE_TEMPORAL

#endif
