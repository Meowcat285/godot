/**************************************************************************/
/*  atmosphere_sky_material.h                                             */
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

#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

// Physically based sky based on "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
// (Sébastien Hillaire, 2020). Transmittance and multiple scattering lookup tables are baked on the CPU
// whenever the atmosphere changes, and single scattering is raymarched in the sky shader's quarter
// resolution pass.
class AtmosphereSkyMaterial : public Material {
	GDCLASS(AtmosphereSkyMaterial, Material);

public:
	static constexpr int TRANSMITTANCE_LUT_WIDTH = 256;
	static constexpr int TRANSMITTANCE_LUT_HEIGHT = 64;
	static constexpr int MULTI_SCATTERING_LUT_SIZE = 32;

private:
	enum ShaderFlags {
		SHADER_FLAG_DEBANDING = 1,
		SHADER_FLAG_NIGHT_SKY = 2,
		SHADER_FLAG_CAMERA_ALTITUDE = 4,
		SHADER_FLAG_MAX = 8,
	};

	static Mutex shader_mutex;
	static RID shader_cache[SHADER_FLAG_MAX];
	static void _update_shader(int p_flags);
	int _get_shader_flags() const;
	mutable bool shader_set = false;

	// Distances are in kilometers, coefficients in 1/km.
	float ground_radius = 6360.0f;
	float atmosphere_height = 100.0f;
	Color ground_albedo = Color(0.3, 0.3, 0.3);

	Color rayleigh_scattering_color = Color(0.175287, 0.409607, 1.0);
	float rayleigh_scattering_scale = 0.0331f;
	float rayleigh_scale_height = 8.0f;

	Color mie_scattering_color = Color(1, 1, 1);
	float mie_scattering_scale = 0.003996f;
	Color mie_absorption_color = Color(1, 1, 1);
	float mie_absorption_scale = 0.000444f;
	float mie_anisotropy = 0.8f;
	float mie_scale_height = 1.2f;

	Color ozone_absorption_color = Color(0.345561, 1.0, 0.045189);
	float ozone_absorption_scale = 0.001881f;
	float ozone_altitude = 25.0f;
	float ozone_width = 30.0f;

	float multi_scattering_factor = 1.0f;

	float observer_altitude = 1.0f; // Meters.
	bool use_camera_altitude = false;

	float energy_multiplier = 1.0f;
	bool use_debanding = true;
	Ref<Texture2D> night_sky;

	mutable Ref<ImageTexture> transmittance_lut;
	mutable Ref<ImageTexture> multi_scattering_lut;
	mutable bool luts_dirty = true;
	bool lut_update_queued = false;

	void _params_changed();
	void _update_params();
	void _deferred_update_luts();
	void _update_luts() const;
	void _update_shader_variant();

protected:
	static void _bind_methods();

public:
	void set_ground_radius(float p_radius);
	float get_ground_radius() const;
	void set_atmosphere_height(float p_height);
	float get_atmosphere_height() const;
	void set_ground_albedo(const Color &p_albedo);
	Color get_ground_albedo() const;

	void set_rayleigh_scattering_color(const Color &p_color);
	Color get_rayleigh_scattering_color() const;
	void set_rayleigh_scattering_scale(float p_scale);
	float get_rayleigh_scattering_scale() const;
	void set_rayleigh_scale_height(float p_height);
	float get_rayleigh_scale_height() const;

	void set_mie_scattering_color(const Color &p_color);
	Color get_mie_scattering_color() const;
	void set_mie_scattering_scale(float p_scale);
	float get_mie_scattering_scale() const;
	void set_mie_absorption_color(const Color &p_color);
	Color get_mie_absorption_color() const;
	void set_mie_absorption_scale(float p_scale);
	float get_mie_absorption_scale() const;
	void set_mie_anisotropy(float p_anisotropy);
	float get_mie_anisotropy() const;
	void set_mie_scale_height(float p_height);
	float get_mie_scale_height() const;

	void set_ozone_absorption_color(const Color &p_color);
	Color get_ozone_absorption_color() const;
	void set_ozone_absorption_scale(float p_scale);
	float get_ozone_absorption_scale() const;
	void set_ozone_altitude(float p_altitude);
	float get_ozone_altitude() const;
	void set_ozone_width(float p_width);
	float get_ozone_width() const;

	void set_multi_scattering_factor(float p_factor);
	float get_multi_scattering_factor() const;

	void set_observer_altitude(float p_altitude);
	float get_observer_altitude() const;
	void set_use_camera_altitude(bool p_enable);
	bool get_use_camera_altitude() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;
	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;
	void set_night_sky(const Ref<Texture2D> &p_night_sky);
	Ref<Texture2D> get_night_sky() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	AtmosphereSkyMaterial();
};
