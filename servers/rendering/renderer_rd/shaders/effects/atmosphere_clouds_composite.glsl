/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;
/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = gl_Position.xy * 0.5 + 0.5;
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

// Blends the clouds over the scene: color * transmittance + cloud light. Clouds are placed at the depth
// where they start, so the hardware depth test hides them behind closer geometry (MSAA aware).

layout(location = 0) in vec2 uv_interp;
/* clang-format on */

layout(set = 0, binding = 0) uniform sampler2D cloud_color;
layout(set = 0, binding = 1) uniform texture2D cloud_depth;

layout(push_constant, std430) uniform Params {
	mat4 projection;
	ivec2 cloud_size;
	float z_near; // In meters.
	float pad;
}
params;

layout(location = 0) out vec4 frag_color;

void main() {
	vec4 clouds = textureLod(cloud_color, uv_interp, 0.0);
	if (clouds.a > 0.999 && dot(clouds.rgb, vec3(1.0)) < 1e-6) {
		discard;
	}
	// Distances can't be blended with "no cloud", so take the nearest texel.
	ivec2 texel = clamp(ivec2(uv_interp * vec2(params.cloud_size)), ivec2(0), params.cloud_size - 1);
	float cloud_distance = texelFetch(cloud_depth, texel, 0).r;

	// Point at that distance along this pixel's view ray, projected back to depth.
	vec4 near_point = inverse(params.projection) * vec4(uv_interp * 2.0 - 1.0, 1.0, 1.0);
	vec3 view_dir = normalize(near_point.xyz / near_point.w);
	vec3 view_pos = view_dir * clamp(cloud_distance, params.z_near * 1.01, 1e7);
	vec4 clip = params.projection * vec4(view_pos, 1.0);
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);

	frag_color = clouds;
}
