/**************************************************************************/
/*  atmosphere_clouds.h                                                   */
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

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/atmosphere_clouds.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/atmosphere_clouds_composite.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_server_enums.h"

class RenderDataRD;

namespace RendererRD {

class SkyRD;

// Volumetric clouds for AtmosphereSkyMaterial. The renderer runs them like a built-in compositor effect
// (before the transparent pass) whenever the environment's sky material has `clouds_enabled` set, so
// the settings come from the sky material's parameters and no Compositor resource is needed.
class AtmosphereClouds {
public:
	class CloudBuffers : public RenderBufferCustomDataRD {
		GDCLASS(CloudBuffers, RenderBufferCustomDataRD);

	public:
		Size2i size;
		uint32_t view_count = 0;
		// Per view: the current march, and two history buffers used in turn.
		RID march_color[2];
		RID march_depth[2];
		RID history_color[2][2];
		RID history_depth[2][2];
		uint32_t frame = 0;
		bool history_valid = false;
		Projection prev_view_projection[2];

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;
	};

private:
	enum Mode {
		MODE_NOISE_SHAPE,
		MODE_NOISE_DETAIL,
		MODE_NOISE_WEATHER,
		MODE_MARCH,
		MODE_TEMPORAL,
		MODE_MAX,
	};

	// Keep in sync with the Params block in atmosphere_clouds.glsl (std140, vec4 aligned).
	struct Params {
		float inv_projection[2][16];
		float projection[2][16];
		float inv_view[16];
		float prev_view_projection[2][16];
		float camera[4];
		float wind_offset[4];
		float sun_direction[4];
		float sun_illuminance[4];
		float layer[4];
		float shape[4];
		float detail[4];
		float albedo[4];
		float rayleigh[4];
		float mie_scattering[4];
		float mie_extinction[4];
		float ozone[4];
		float misc[4];
		float sky_border[4];
		int32_t sizes[4];
		int32_t steps[4];
	};

	struct ComputePushConstant {
		int32_t view;
		int32_t pad[3];
	};

	struct CompositePushConstant {
		float projection[16];
		int32_t cloud_size[2];
		float z_near;
		float pad;
	};

	static AtmosphereClouds *singleton;

	bool use_octmap_array = false;
	AtmosphereCloudsShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	AtmosphereCloudsCompositeShaderRD composite_shader;
	RID composite_version;
	PipelineCacheRD composite_pipeline;

	RID shape_noise;
	RID detail_noise;
	RID weather_noise;
	RID params_buffer;

	void _generate_noise();
	void _render(const RenderDataRD *p_render_data, SkyRD &p_sky, RID p_material);

	static RID _get_active_material(const RenderDataRD *p_render_data, const SkyRD &p_sky);

public:
	AtmosphereClouds(bool p_use_octmap_array);
	~AtmosphereClouds();

	// Hooks for RendererSceneRenderRD's compositor effect handling.
	static bool uses_callback(const RenderDataRD *p_render_data, const SkyRD &p_sky, RSE::CompositorEffectCallbackType p_callback_type);
	static bool has_flag(const RenderDataRD *p_render_data, const SkyRD &p_sky, RSE::CompositorEffectFlags p_flag, RSE::CompositorEffectCallbackType p_callback_type);
	static void process(const RenderDataRD *p_render_data, SkyRD &p_sky, RSE::CompositorEffectCallbackType p_callback_type);
	static void free_singleton();
};

} // namespace RendererRD
