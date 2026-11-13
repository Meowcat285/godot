/**************************************************************************/
/*  atmosphere_clouds.cpp                                                 */
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

#include "atmosphere_clouds.h"

#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

using namespace RendererRD;

static const char *CLOUD_BUFFERS_NAME = "atmosphere_clouds";

static constexpr int SHAPE_NOISE_SIZE = 128;
static constexpr int DETAIL_NOISE_SIZE = 32;
static constexpr int WEATHER_NOISE_SIZE = 512;

AtmosphereClouds *AtmosphereClouds::singleton = nullptr;

/* Cloud buffers */

void AtmosphereClouds::CloudBuffers::configure(RenderSceneBuffersRD *p_render_buffers) {
	// Sizes may have changed: textures are recreated on the next render.
	free_data();
}

void AtmosphereClouds::CloudBuffers::free_data() {
	RD *rd = RD::get_singleton();
	for (int v = 0; v < 2; v++) {
		RID *rids[] = { &march_color[v], &march_depth[v], &history_color[v][0], &history_color[v][1], &history_depth[v][0], &history_depth[v][1] };
		for (RID *rid : rids) {
			if (rid->is_valid()) {
				rd->free_rid(*rid);
				*rid = RID();
			}
		}
	}
	size = Size2i();
	view_count = 0;
	history_valid = false;
}

/* Setup */

AtmosphereClouds::AtmosphereClouds(bool p_use_octmap_array) {
	use_octmap_array = p_use_octmap_array;

	const String radiance_define = use_octmap_array ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "";
	Vector<String> modes;
	modes.push_back("\n#define MODE_NOISE_SHAPE\n");
	modes.push_back("\n#define MODE_NOISE_DETAIL\n");
	modes.push_back("\n#define MODE_NOISE_WEATHER\n");
	modes.push_back("\n#define MODE_MARCH\n" + radiance_define);
	modes.push_back("\n#define MODE_TEMPORAL\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	Vector<String> composite_modes;
	composite_modes.push_back("");
	composite_shader.initialize(composite_modes);
	composite_version = composite_shader.version_create();

	RD::PipelineDepthStencilState depth_state;
	depth_state.enable_depth_test = true;
	depth_state.enable_depth_write = false;
	// Reverse Z: pass where the cloud starts in front of the scene. Clouds beyond the far plane are clamped to depth 0,
	// so "or equal" lets them cover the sky (also at depth 0) but not geometry.
	depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;

	RD::PipelineColorBlendState blend_state;
	RD::PipelineColorBlendState::Attachment attachment;
	attachment.enable_blend = true;
	// color = cloud light + scene color * cloud transmittance. Destination alpha is kept.
	attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_SRC_ALPHA;
	attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ZERO;
	attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	blend_state.attachments.push_back(attachment);

	composite_pipeline.setup(composite_shader.version_get_shader(composite_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_state, blend_state, 0);

	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

AtmosphereClouds::~AtmosphereClouds() {
	RD *rd = RD::get_singleton();
	RID *rids[] = { &shape_noise, &detail_noise, &weather_noise, &params_buffer };
	for (RID *rid : rids) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
		}
	}
	for (RID &pipeline : pipelines) {
		if (pipeline.is_valid()) {
			rd->free_rid(pipeline);
		}
	}
	composite_pipeline.clear();
	composite_shader.version_free(composite_version);
	shader.version_free(shader_version);
}

void AtmosphereClouds::free_singleton() {
	if (singleton) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void AtmosphereClouds::_generate_noise() {
	RD *rd = RD::get_singleton();

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.width = tf.height = tf.depth = SHAPE_NOISE_SIZE;
	shape_noise = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(shape_noise, "Atmosphere Clouds Shape Noise");

	tf.width = tf.height = tf.depth = DETAIL_NOISE_SIZE;
	detail_noise = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(detail_noise, "Atmosphere Clouds Detail Noise");

	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.width = tf.height = WEATHER_NOISE_SIZE;
	tf.depth = 1;
	weather_noise = rd->texture_create(tf, RD::TextureView());
	rd->set_resource_name(weather_noise, "Atmosphere Clouds Weather Noise");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	struct Job {
		Mode mode;
		RID texture;
		int size;
		int depth;
	};
	const Job jobs[] = {
		{ MODE_NOISE_SHAPE, shape_noise, SHAPE_NOISE_SIZE, SHAPE_NOISE_SIZE },
		{ MODE_NOISE_DETAIL, detail_noise, DETAIL_NOISE_SIZE, DETAIL_NOISE_SIZE },
		{ MODE_NOISE_WEATHER, weather_noise, WEATHER_NOISE_SIZE, 1 },
	};

	RD::get_singleton()->draw_command_begin_label("Generate Cloud Noise");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (const Job &job : jobs) {
		RID job_shader = shader.version_get_shader(shader_version, job.mode);
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, job.texture);
		ComputePushConstant push_constant = {};
		push_constant.view = job.size;
		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[job.mode]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(job_shader, 0, u_dest), 0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ComputePushConstant));
		rd->compute_list_dispatch_threads(compute_list, job.size, job.size, job.depth);
	}
	rd->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
}

/* Hooks */

RID AtmosphereClouds::_get_active_material(const RenderDataRD *p_render_data, const SkyRD &p_sky) {
	if (p_render_data->reflection_probe.is_valid() || p_render_data->transparent_bg || p_render_data->render_buffers.is_null()) {
		return RID();
	}
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	RID environment = p_render_data->environment;
	if (!scene_render->is_environment(environment) || scene_render->environment_get_background(environment) != RSE::ENV_BG_SKY) {
		return RID();
	}
	RID sky = scene_render->environment_get_sky(environment);
	if (sky.is_null()) {
		return RID();
	}
	RID material = p_sky.sky_get_material(sky);
	if (material.is_null()) {
		return RID();
	}
	// Set by AtmosphereSkyMaterial, or by a ShaderMaterial converted from it.
	if (!bool(MaterialStorage::get_singleton()->material_get_param(material, "clouds_enabled"))) {
		return RID();
	}
	return material;
}

bool AtmosphereClouds::uses_callback(const RenderDataRD *p_render_data, const SkyRD &p_sky, RSE::CompositorEffectCallbackType p_callback_type) {
	if (p_callback_type != RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT && p_callback_type != RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_ANY) {
		return false;
	}
	return _get_active_material(p_render_data, p_sky).is_valid();
}

bool AtmosphereClouds::has_flag(const RenderDataRD *p_render_data, const SkyRD &p_sky, RSE::CompositorEffectFlags p_flag, RSE::CompositorEffectCallbackType p_callback_type) {
	// Only answered for the exact callback: Forward+ asks per callback, while the Mobile renderer asks with
	// CALLBACK_TYPE_ANY and only warns since it can't resolve depth for effects. The march then skips the depth clip.
	if (p_flag != RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH || p_callback_type != RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT) {
		return false;
	}
	return _get_active_material(p_render_data, p_sky).is_valid();
}

void AtmosphereClouds::process(const RenderDataRD *p_render_data, SkyRD &p_sky, RSE::CompositorEffectCallbackType p_callback_type) {
	if (p_callback_type != RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT) {
		return;
	}
	RID material = _get_active_material(p_render_data, p_sky);
	if (material.is_null()) {
		return;
	}
	if (!singleton) {
		singleton = memnew(AtmosphereClouds(RendererSceneRenderRD::get_singleton()->is_using_radiance_octmap_array()));
	}
	singleton->_render(p_render_data, p_sky, material);
}

/* Rendering */

void AtmosphereClouds::_render(const RenderDataRD *p_render_data, SkyRD &p_sky, RID p_material) {
	RD *rd = RD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const Size2i full_size = rb->get_internal_size();
	const Size2i size = Size2i(MAX(1, (full_size.x + 1) / 2), MAX(1, (full_size.y + 1) / 2));
	const uint32_t view_count = MIN(rb->get_view_count(), 2u);
	const bool msaa = rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
	// The Mobile renderer can't resolve MSAA depth for effects; the composite's depth test still works there.
	const bool depth_available = !msaa || rb->get_can_be_storage();

	if (shape_noise.is_null()) {
		_generate_noise();
	}

	auto param = [&](const char *p_name) -> Variant {
		return material_storage->material_get_param(p_material, p_name);
	};
	auto param_texture = [&](const char *p_name, TextureStorage::DefaultRDTexture p_default) -> RID {
		RID texture = param(p_name);
		RID rd_texture = texture.is_valid() ? texture_storage->texture_get_rd_texture(texture) : RID();
		return rd_texture.is_valid() ? rd_texture : texture_storage->texture_rd_get_default(p_default);
	};

	/* Buffers */

	Ref<CloudBuffers> buffers;
	if (rb->has_custom_data(CLOUD_BUFFERS_NAME)) {
		buffers = rb->get_custom_data(CLOUD_BUFFERS_NAME);
	} else {
		buffers.instantiate();
		rb->set_custom_data(CLOUD_BUFFERS_NAME, buffers);
	}
	if (buffers->size != size || buffers->view_count != view_count) {
		buffers->free_data();
		buffers->size = size;
		buffers->view_count = view_count;

		RD::TextureFormat tf;
		tf.width = size.x;
		tf.height = size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		for (uint32_t v = 0; v < view_count; v++) {
			tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			buffers->march_color[v] = rd->texture_create(tf, RD::TextureView());
			buffers->history_color[v][0] = rd->texture_create(tf, RD::TextureView());
			buffers->history_color[v][1] = rd->texture_create(tf, RD::TextureView());
			tf.format = RD::DATA_FORMAT_R32_SFLOAT;
			buffers->march_depth[v] = rd->texture_create(tf, RD::TextureView());
			buffers->history_depth[v][0] = rd->texture_create(tf, RD::TextureView());
			buffers->history_depth[v][1] = rd->texture_create(tf, RD::TextureView());
		}
	}

	/* Parameters */

	Params params = {};
	Projection correction;
	correction.set_depth_correction(scene_data->flip_y);
	correction.add_jitter_offset(scene_data->taa_jitter);
	const Transform3D cam_transform = scene_data->cam_transform;
	Projection view_projections[2];
	for (uint32_t v = 0; v < view_count; v++) {
		Projection projection = correction * (view_count > 1 ? scene_data->view_projection[v] : scene_data->cam_projection);
		MaterialStorage::store_camera(projection, params.projection[v]);
		MaterialStorage::store_camera(projection.inverse(), params.inv_projection[v]);
		view_projections[v] = projection * Projection(cam_transform.affine_inverse());
		MaterialStorage::store_camera(buffers->history_valid ? buffers->prev_view_projection[v] : view_projections[v], params.prev_view_projection[v]);
	}
	MaterialStorage::store_transform(cam_transform, params.inv_view);

	const float ground_radius = param("ground_radius");
	const float observer_altitude = param("observer_altitude"); // Already in km.
	const float altitude = observer_altitude + (bool(param("use_camera_altitude")) ? float(cam_transform.origin.y) * 0.001f : 0.0f);
	params.camera[0] = cam_transform.origin.x * 0.001f;
	params.camera[1] = ground_radius + MAX(altitude, 0.001f);
	params.camera[2] = cam_transform.origin.z * 0.001f;
	params.camera[3] = float(buffers->frame % 1024);

	const Vector2 wind = param("clouds_wind_velocity"); // In km/s.
	// Scene time already wraps (rendering/limits/time/time_rollover_secs), which keeps this precise.
	params.wind_offset[0] = wind.x * float(scene_data->time);
	params.wind_offset[1] = wind.y * float(scene_data->time);

	// The first directional light the sky sees lights the clouds.
	const float illuminance_scale = param("illuminance_scale");
	for (uint32_t i = 0; i < p_sky.sky_scene_state.ubo.directional_light_count; i++) {
		const SkyRD::SkyDirectionalLightData &light = p_sky.sky_scene_state.directional_lights[i];
		if (!light.enabled) {
			continue;
		}
		for (int c = 0; c < 3; c++) {
			params.sun_direction[c] = light.direction[c];
			params.sun_illuminance[c] = light.color[c] * light.energy * illuminance_scale;
		}
		params.sun_direction[3] = 1.0f;
		break;
	}

	const float bottom_altitude = param("clouds_bottom_altitude");
	const float thickness = param("clouds_thickness");
	params.layer[0] = ground_radius;
	params.layer[1] = ground_radius + float(param("atmosphere_height"));
	params.layer[2] = ground_radius + bottom_altitude;
	params.layer[3] = ground_radius + bottom_altitude + MAX(thickness, 0.01f);

	params.shape[0] = param("clouds_coverage");
	params.shape[1] = param("clouds_type");
	params.shape[2] = param("clouds_density");
	params.shape[3] = MAX(float(param("clouds_shape_scale")), 0.01f);

	params.detail[0] = MAX(float(param("clouds_detail_scale")), 0.001f);
	params.detail[1] = param("clouds_detail_strength");
	params.detail[2] = MAX(float(param("clouds_weather_scale")), 0.01f);
	params.detail[3] = 80.0f; // Max march distance in km; beyond it clouds are a thin band at the horizon.

	float brightness = scene_render->environment_get_bg_energy_multiplier(p_render_data->environment) * scene_render->environment_get_bg_intensity(p_render_data->environment);
	if (p_render_data->camera_attributes.is_valid()) {
		brightness *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}
	const float luminance_multiplier = rb->get_luminance_multiplier();
	const Vector3 albedo = param("clouds_albedo");
	params.albedo[0] = albedo.x;
	params.albedo[1] = albedo.y;
	params.albedo[2] = albedo.z;
	// Same scaling as the sky shader's output (exposure uniform, background energy, buffer luminance range).
	params.albedo[3] = float(param("exposure")) * brightness / luminance_multiplier;

	const Vector3 rayleigh = param("rayleigh_scattering");
	const Vector3 mie_scattering = param("mie_scattering");
	const Vector3 mie_extinction = param("mie_extinction");
	const Vector3 ozone = param("ozone_absorption");
	params.rayleigh[0] = rayleigh.x;
	params.rayleigh[1] = rayleigh.y;
	params.rayleigh[2] = rayleigh.z;
	params.rayleigh[3] = MAX(float(param("rayleigh_scale_height")), 0.001f);
	params.mie_scattering[0] = mie_scattering.x;
	params.mie_scattering[1] = mie_scattering.y;
	params.mie_scattering[2] = mie_scattering.z;
	params.mie_scattering[3] = MAX(float(param("mie_scale_height")), 0.001f);
	params.mie_extinction[0] = mie_extinction.x;
	params.mie_extinction[1] = mie_extinction.y;
	params.mie_extinction[2] = mie_extinction.z;
	params.mie_extinction[3] = param("mie_anisotropy");
	params.ozone[0] = ozone.x;
	params.ozone[1] = ozone.y;
	params.ozone[2] = ozone.z;
	params.ozone[3] = param("ozone_altitude");

	RID sky = scene_render->environment_get_sky(p_render_data->environment);
	const float border = p_sky.sky_get_uv_border_size(sky);
	params.misc[0] = MAX(float(param("ozone_width")), 0.001f);
	// The radiance map is stored divided by the buffers' luminance multiplier, like the sky.
	params.misc[1] = luminance_multiplier;
	params.misc[2] = MAX(scene_render->get_roughness_layers() - 1, 0);
	params.misc[3] = 0.9f; // History weight.
	params.sky_border[0] = border;
	params.sky_border[1] = 1.0f - border * 2.0f;
	params.sky_border[2] = depth_available ? 1.0f : 0.0f;

	RID weather_texture = param("clouds_weather_map");
	RID weather_rd = weather_texture.is_valid() ? texture_storage->texture_get_rd_texture(weather_texture) : RID();
	params.sky_border[3] = weather_rd.is_valid() ? 1.0f : 0.0f;
	if (weather_rd.is_null()) {
		weather_rd = weather_noise;
	}

	params.sizes[0] = size.x;
	params.sizes[1] = size.y;
	params.sizes[2] = full_size.x;
	params.sizes[3] = full_size.y;

	const int quality = param("clouds_quality");
	static const int VIEW_STEPS[3] = { 32, 64, 128 };
	static const int LIGHT_STEPS[3] = { 4, 6, 8 };
	params.steps[0] = VIEW_STEPS[CLAMP(quality, 0, 2)];
	params.steps[1] = LIGHT_STEPS[CLAMP(quality, 0, 2)];
	params.steps[2] = view_count;
	params.steps[3] = buffers->history_valid ? 1 : 0;

	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);

	/* Uniforms */

	RID linear_repeat = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	RID linear_clamp = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_mipmap_clamp = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer);
	RD::Uniform u_linear_repeat(RD::UNIFORM_TYPE_SAMPLER, 1, linear_repeat);
	RD::Uniform u_linear_clamp(RD::UNIFORM_TYPE_SAMPLER, 2, linear_clamp);
	RD::Uniform u_linear_mipmap_clamp(RD::UNIFORM_TYPE_SAMPLER, 3, linear_mipmap_clamp);

	RID radiance = sky.is_valid() ? p_sky.sky_get_radiance_texture_rd(sky) : RID();
	if (radiance.is_null()) {
		radiance = texture_storage->texture_rd_get_default(use_octmap_array ? TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RD::Uniform u_shape(RD::UNIFORM_TYPE_TEXTURE, 0, shape_noise);
	RD::Uniform u_detail(RD::UNIFORM_TYPE_TEXTURE, 1, detail_noise);
	RD::Uniform u_weather(RD::UNIFORM_TYPE_TEXTURE, 2, weather_rd);
	RD::Uniform u_transmittance(RD::UNIFORM_TYPE_TEXTURE, 3, param_texture("transmittance_lut", TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
	RD::Uniform u_multi_scattering(RD::UNIFORM_TYPE_TEXTURE, 4, param_texture("multi_scattering_lut", TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
	RD::Uniform u_radiance(RD::UNIFORM_TYPE_TEXTURE, 5, radiance);

	const uint32_t read_index = buffers->frame & 1;
	const uint32_t write_index = read_index ^ 1;

	RD::get_singleton()->draw_command_begin_label("Atmosphere Clouds");

	RID march_shader = shader.version_get_shader(shader_version, MODE_MARCH);
	RID temporal_shader = shader.version_get_shader(shader_version, MODE_TEMPORAL);

	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (uint32_t v = 0; v < view_count; v++) {
		ComputePushConstant push_constant = {};
		push_constant.view = v;

		RID depth = depth_available ? rb->get_depth_texture(v) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_TEXTURE, 6, depth);
		RD::Uniform u_march_color_out(RD::UNIFORM_TYPE_IMAGE, 0, buffers->march_color[v]);
		RD::Uniform u_march_depth_out(RD::UNIFORM_TYPE_IMAGE, 1, buffers->march_depth[v]);

		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_MARCH]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(march_shader, 0, u_params, u_linear_repeat, u_linear_clamp, u_linear_mipmap_clamp), 0);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(march_shader, 1, u_shape, u_detail, u_weather, u_transmittance, u_multi_scattering, u_radiance, u_depth), 1);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(march_shader, 2, u_march_color_out, u_march_depth_out), 2);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ComputePushConstant));
		rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	}
	rd->compute_list_add_barrier(compute_list);
	for (uint32_t v = 0; v < view_count; v++) {
		ComputePushConstant push_constant = {};
		push_constant.view = v;

		RD::Uniform u_march_color(RD::UNIFORM_TYPE_TEXTURE, 0, buffers->march_color[v]);
		RD::Uniform u_march_depth(RD::UNIFORM_TYPE_TEXTURE, 1, buffers->march_depth[v]);
		RD::Uniform u_history_color(RD::UNIFORM_TYPE_TEXTURE, 2, buffers->history_color[v][read_index]);
		RD::Uniform u_history_depth(RD::UNIFORM_TYPE_TEXTURE, 3, buffers->history_depth[v][read_index]);
		RD::Uniform u_color_out(RD::UNIFORM_TYPE_IMAGE, 0, buffers->history_color[v][write_index]);
		RD::Uniform u_depth_out(RD::UNIFORM_TYPE_IMAGE, 1, buffers->history_depth[v][write_index]);

		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(temporal_shader, 0, u_params, u_linear_repeat, u_linear_clamp, u_linear_mipmap_clamp), 0);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(temporal_shader, 1, u_march_color, u_march_depth, u_history_color, u_history_depth), 1);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(temporal_shader, 2, u_color_out, u_depth_out), 2);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ComputePushConstant));
		rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	}
	rd->compute_list_end();

	/* Composite */

	RID composite = composite_shader.version_get_shader(composite_version, 0);
	for (uint32_t v = 0; v < view_count; v++) {
		RID framebuffer;
		if (msaa) {
			// Also resolve into the internal texture: the Mobile renderer only resolves again if it has transparent objects to draw.
			RD::FramebufferPass pass;
			pass.color_attachments.push_back(0);
			pass.depth_attachment = 1;
			pass.resolve_attachments.push_back(2);
			framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multipass({ rb->get_color_msaa(v), rb->get_depth_msaa(v), rb->get_internal_texture(v) }, { pass });
		} else {
			framebuffer = FramebufferCacheRD::get_singleton()->get_cache(rb->get_internal_texture(v), rb->get_depth_texture(v));
		}

		CompositePushConstant push_constant = {};
		Projection projection = correction * (view_count > 1 ? scene_data->view_projection[v] : scene_data->cam_projection);
		MaterialStorage::store_camera(projection, push_constant.projection);
		push_constant.cloud_size[0] = size.x;
		push_constant.cloud_size[1] = size.y;
		push_constant.z_near = scene_data->z_near;

		RD::Uniform u_cloud_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_clamp, buffers->history_color[v][write_index] }));
		RD::Uniform u_cloud_depth(RD::UNIFORM_TYPE_TEXTURE, 1, buffers->history_depth[v][write_index]);

		RD::DrawListID draw_list = rd->draw_list_begin(framebuffer, RD::DRAW_DEFAULT_ALL);
		rd->draw_list_bind_render_pipeline(draw_list, composite_pipeline.get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(framebuffer)));
		rd->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(composite, 0, u_cloud_color, u_cloud_depth), 0);
		rd->draw_list_set_push_constant(draw_list, &push_constant, sizeof(CompositePushConstant));
		rd->draw_list_draw(draw_list, false, 1u, 3u);
		rd->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();

	for (uint32_t v = 0; v < view_count; v++) {
		buffers->prev_view_projection[v] = view_projections[v];
	}
	buffers->history_valid = true;
	buffers->frame++;
}
