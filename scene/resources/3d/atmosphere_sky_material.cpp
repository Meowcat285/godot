/**************************************************************************/
/*  atmosphere_sky_material.cpp                                           */
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

#include "atmosphere_sky_material.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "servers/rendering/rendering_server.h"

namespace {

// CPU model of the atmosphere used to bake the lookup tables. Must match the shader below.
struct AtmosphereModel {
	float ground_radius;
	float top_radius;
	Vector3 rayleigh_scattering;
	float rayleigh_scale_height;
	Vector3 mie_scattering;
	Vector3 mie_extinction;
	float mie_scale_height;
	Vector3 ozone_absorption;
	float ozone_altitude;
	float ozone_width;
	Vector3 ground_albedo;

	void sample(float p_radius, Vector3 &r_scattering, Vector3 &r_extinction) const {
		const float h = MAX(0.0f, p_radius - ground_radius);
		const float rayleigh_density = Math::exp(-h / rayleigh_scale_height);
		const float mie_density = Math::exp(-h / mie_scale_height);
		const float ozone_density = MAX(0.0f, 1.0f - Math::abs(h - ozone_altitude) / (ozone_width * 0.5f));
		r_scattering = rayleigh_scattering * rayleigh_density + mie_scattering * mie_density;
		r_extinction = rayleigh_scattering * rayleigh_density + mie_extinction * mie_density + ozone_absorption * ozone_density;
	}

	// Distance from a point at radius p_r to the sphere of radius p_radius along a ray with zenith cosine p_mu,
	// or -1 if the ray misses it.
	static float ray_sphere(float p_r, float p_mu, float p_radius) {
		const float c = (p_r - p_radius) * (p_r + p_radius);
		const float b = p_r * p_mu;
		const float disc = b * b - c;
		if (disc < 0.0f) {
			return -1.0f;
		}
		const float s = Math::sqrt(disc);
		if (-b - s >= 0.0f) {
			return -b - s;
		}
		if (-b + s >= 0.0f) {
			return -b + s;
		}
		return -1.0f;
	}

	bool ray_hits_ground(float p_r, float p_mu) const {
		return p_mu < 0.0f && ray_sphere(p_r, p_mu, ground_radius) >= 0.0f;
	}
};

Vector3 color_to_vec3(const Color &p_color) {
	return Vector3(p_color.r, p_color.g, p_color.b);
}

} // namespace

/* Setters and getters */

#define ATMOSPHERE_PROPERTY(m_type, m_name, m_value) \
	void AtmosphereSkyMaterial::set_##m_name(m_type p_value) { \
		m_name = m_value; \
		_params_changed(); \
	} \
	decltype(AtmosphereSkyMaterial::m_name) AtmosphereSkyMaterial::get_##m_name() const { \
		return m_name; \
	}

ATMOSPHERE_PROPERTY(float, ground_radius, MAX(p_value, 1.0f))
ATMOSPHERE_PROPERTY(float, atmosphere_height, MAX(p_value, 0.01f))
ATMOSPHERE_PROPERTY(const Color &, ground_albedo, p_value)
ATMOSPHERE_PROPERTY(const Color &, rayleigh_scattering_color, p_value)
ATMOSPHERE_PROPERTY(float, rayleigh_scattering_scale, MAX(p_value, 0.0f))
ATMOSPHERE_PROPERTY(float, rayleigh_scale_height, MAX(p_value, 0.001f))
ATMOSPHERE_PROPERTY(const Color &, mie_scattering_color, p_value)
ATMOSPHERE_PROPERTY(float, mie_scattering_scale, MAX(p_value, 0.0f))
ATMOSPHERE_PROPERTY(const Color &, mie_absorption_color, p_value)
ATMOSPHERE_PROPERTY(float, mie_absorption_scale, MAX(p_value, 0.0f))
ATMOSPHERE_PROPERTY(float, mie_anisotropy, CLAMP(p_value, -0.999f, 0.999f))
ATMOSPHERE_PROPERTY(float, mie_scale_height, MAX(p_value, 0.001f))
ATMOSPHERE_PROPERTY(const Color &, ozone_absorption_color, p_value)
ATMOSPHERE_PROPERTY(float, ozone_absorption_scale, MAX(p_value, 0.0f))
ATMOSPHERE_PROPERTY(float, ozone_altitude, p_value)
ATMOSPHERE_PROPERTY(float, ozone_width, MAX(p_value, 0.001f))
ATMOSPHERE_PROPERTY(float, multi_scattering_factor, MAX(p_value, 0.0f))

#undef ATMOSPHERE_PROPERTY

// These don't affect the lookup tables.
void AtmosphereSkyMaterial::set_observer_altitude(float p_altitude) {
	observer_altitude = p_altitude;
	_update_params();
}

float AtmosphereSkyMaterial::get_observer_altitude() const {
	return observer_altitude;
}

void AtmosphereSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = MAX(p_multiplier, 0.0f);
	_update_params();
}

float AtmosphereSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

void AtmosphereSkyMaterial::set_use_camera_altitude(bool p_enable) {
	use_camera_altitude = p_enable;
	_update_shader_variant();
}

bool AtmosphereSkyMaterial::get_use_camera_altitude() const {
	return use_camera_altitude;
}

void AtmosphereSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader_variant();
}

bool AtmosphereSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void AtmosphereSkyMaterial::set_night_sky(const Ref<Texture2D> &p_night_sky) {
	night_sky = p_night_sky;
	RS::get_singleton()->material_set_param(_get_material(), "night_sky", night_sky.is_valid() ? Variant(night_sky->get_rid()) : Variant());
	_update_shader_variant();
}

Ref<Texture2D> AtmosphereSkyMaterial::get_night_sky() const {
	return night_sky;
}

/* Parameters and lookup tables */

void AtmosphereSkyMaterial::_params_changed() {
	_update_params();

	// Rebuild the lookup tables once per frame at most, no matter how many properties changed.
	luts_dirty = true;
	if (!lut_update_queued) {
		lut_update_queued = true;
		callable_mp(this, &AtmosphereSkyMaterial::_deferred_update_luts).call_deferred();
	}
}

void AtmosphereSkyMaterial::_update_params() {
	RS *rs = RS::get_singleton();
	const RID rid = _get_material();
	rs->material_set_param(rid, "ground_radius", ground_radius);
	rs->material_set_param(rid, "atmosphere_height", atmosphere_height);
	rs->material_set_param(rid, "ground_albedo", color_to_vec3(ground_albedo));
	rs->material_set_param(rid, "rayleigh_scattering", color_to_vec3(rayleigh_scattering_color) * rayleigh_scattering_scale);
	rs->material_set_param(rid, "rayleigh_scale_height", rayleigh_scale_height);
	rs->material_set_param(rid, "mie_scattering", color_to_vec3(mie_scattering_color) * mie_scattering_scale);
	rs->material_set_param(rid, "mie_extinction", color_to_vec3(mie_scattering_color) * mie_scattering_scale + color_to_vec3(mie_absorption_color) * mie_absorption_scale);
	rs->material_set_param(rid, "mie_anisotropy", mie_anisotropy);
	rs->material_set_param(rid, "mie_scale_height", mie_scale_height);
	rs->material_set_param(rid, "ozone_absorption", color_to_vec3(ozone_absorption_color) * ozone_absorption_scale);
	rs->material_set_param(rid, "ozone_altitude", ozone_altitude);
	rs->material_set_param(rid, "ozone_width", ozone_width);
	rs->material_set_param(rid, "observer_altitude", observer_altitude * 0.001f);
	rs->material_set_param(rid, "exposure", energy_multiplier);
}

void AtmosphereSkyMaterial::_deferred_update_luts() {
	lut_update_queued = false;
	_update_luts();
}

void AtmosphereSkyMaterial::_update_luts() const {
	if (!luts_dirty) {
		return;
	}
	luts_dirty = false;

	AtmosphereModel atmosphere;
	atmosphere.ground_radius = ground_radius;
	atmosphere.top_radius = ground_radius + atmosphere_height;
	atmosphere.rayleigh_scattering = color_to_vec3(rayleigh_scattering_color) * rayleigh_scattering_scale;
	atmosphere.rayleigh_scale_height = rayleigh_scale_height;
	atmosphere.mie_scattering = color_to_vec3(mie_scattering_color) * mie_scattering_scale;
	atmosphere.mie_extinction = atmosphere.mie_scattering + color_to_vec3(mie_absorption_color) * mie_absorption_scale;
	atmosphere.mie_scale_height = mie_scale_height;
	atmosphere.ozone_absorption = color_to_vec3(ozone_absorption_color) * ozone_absorption_scale;
	atmosphere.ozone_altitude = ozone_altitude;
	atmosphere.ozone_width = ozone_width;
	atmosphere.ground_albedo = color_to_vec3(ground_albedo);

	const float rg = atmosphere.ground_radius;
	const float rt = atmosphere.top_radius;
	const float horizon = MAX(Math::sqrt((rt - rg) * (rt + rg)), 1e-6f);

	// Transmittance from a point to the top of the atmosphere, parameterized as in Bruneton 2017.
	constexpr int TW = TRANSMITTANCE_LUT_WIDTH;
	constexpr int TH = TRANSMITTANCE_LUT_HEIGHT;
	constexpr int TRANSMITTANCE_STEPS = 40;
	LocalVector<Vector3> transmittance;
	transmittance.resize(TW * TH);
	for (int y = 0; y < TH; y++) {
		const float rho = horizon * float(y) / float(TH - 1);
		const float r = Math::sqrt(rho * rho + rg * rg);
		const float d_min = rt - r;
		const float d_max = rho + horizon;
		for (int x = 0; x < TW; x++) {
			const float d = d_min + (d_max - d_min) * float(x) / float(TW - 1);
			const float mu = d == 0.0f ? 1.0f : CLAMP((horizon * horizon - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
			const float dist = MAX(0.0f, AtmosphereModel::ray_sphere(r, mu, rt));
			const float dt = dist / TRANSMITTANCE_STEPS;
			Vector3 optical_depth;
			for (int i = 0; i < TRANSMITTANCE_STEPS; i++) {
				const float t = (i + 0.5f) * dt;
				const float sample_r = Math::sqrt(r * r + 2.0f * r * mu * t + t * t);
				Vector3 scattering, extinction;
				atmosphere.sample(sample_r, scattering, extinction);
				optical_depth += extinction * dt;
			}
			transmittance[y * TW + x] = Vector3(Math::exp(-optical_depth.x), Math::exp(-optical_depth.y), Math::exp(-optical_depth.z));
		}
	}

	// Bilinear lookup into the transmittance table, matching the shader's parameterization.
	auto sample_transmittance = [&](float p_r, float p_mu) -> Vector3 {
		const float rho = Math::sqrt(MAX(0.0f, (p_r - rg) * (p_r + rg)));
		const float d = MAX(0.0f, AtmosphereModel::ray_sphere(p_r, p_mu, rt));
		const float d_min = rt - p_r;
		const float d_max = rho + horizon;
		// Written so NaN from extreme settings maps to 0 instead of an invalid index.
		const float u_mu = (d - d_min) / MAX(d_max - d_min, 1e-6f);
		const float u_r = rho / horizon;
		const float x_mu = (u_mu > 0.0f ? MIN(u_mu, 1.0f) : 0.0f) * (TW - 1);
		const float x_r = (u_r > 0.0f ? MIN(u_r, 1.0f) : 0.0f) * (TH - 1);
		const int x0 = MIN(int(x_mu), TW - 2);
		const int y0 = MIN(int(x_r), TH - 2);
		const float fx = x_mu - x0;
		const float fy = x_r - y0;
		const Vector3 top = transmittance[y0 * TW + x0].lerp(transmittance[y0 * TW + x0 + 1], fx);
		const Vector3 bottom = transmittance[(y0 + 1) * TW + x0].lerp(transmittance[(y0 + 1) * TW + x0 + 1], fx);
		return top.lerp(bottom, fy);
	};

	// Multiple scattering contribution per unit illuminance (Hillaire 2020, section 5.5), assuming isotropic
	// phase for second and higher order scattering.
	constexpr int MS = MULTI_SCATTERING_LUT_SIZE;
	constexpr int MS_DIRECTIONS_SQRT = 8;
	constexpr int MS_STEPS = 20;
	const float isotropic_phase = 1.0f / (4.0f * Math::PI);
	LocalVector<Vector3> multi_scattering;
	multi_scattering.resize(MS * MS);
	for (int y = 0; y < MS; y++) {
		const float r = rg + MAX(0.001f, (rt - rg) * float(y) / float(MS - 1));
		for (int x = 0; x < MS; x++) {
			const float mu_light = float(x) / float(MS - 1) * 2.0f - 1.0f;
			const Vector3 light_dir = Vector3(Math::sqrt(MAX(0.0f, 1.0f - mu_light * mu_light)), mu_light, 0.0f);
			const Vector3 origin = Vector3(0.0f, r, 0.0f);

			Vector3 luminance_sum;
			Vector3 transfer_sum;
			// The light lies in the XY plane, so directions mirrored across it give the same result:
			// only integrate half of the azimuths and count them twice.
			for (int i = 0; i < MS_DIRECTIONS_SQRT / 2; i++) {
				for (int j = 0; j < MS_DIRECTIONS_SQRT; j++) {
					const float theta = Math::TAU * (i + 0.5f) / MS_DIRECTIONS_SQRT;
					const float cos_phi = 1.0f - 2.0f * (j + 0.5f) / MS_DIRECTIONS_SQRT;
					const float sin_phi = Math::sqrt(MAX(0.0f, 1.0f - cos_phi * cos_phi));
					const Vector3 dir = Vector3(Math::cos(theta) * sin_phi, cos_phi, Math::sin(theta) * sin_phi);

					const bool hits_ground = atmosphere.ray_hits_ground(r, dir.y);
					const float t_max = hits_ground ? AtmosphereModel::ray_sphere(r, dir.y, rg) : AtmosphereModel::ray_sphere(r, dir.y, rt);
					const float dt = MAX(0.0f, t_max) / MS_STEPS;

					Vector3 luminance;
					Vector3 transfer;
					Vector3 throughput = Vector3(1, 1, 1);
					for (int s = 0; s < MS_STEPS; s++) {
						const Vector3 p = origin + dir * ((s + 0.3f) * dt);
						const float sample_r = p.length();
						const float sample_mu_light = p.dot(light_dir) / sample_r;
						Vector3 scattering, extinction;
						atmosphere.sample(sample_r, scattering, extinction);
						const Vector3 step_transmittance = Vector3(Math::exp(-extinction.x * dt), Math::exp(-extinction.y * dt), Math::exp(-extinction.z * dt));
						const Vector3 light_transmittance = atmosphere.ray_hits_ground(sample_r, sample_mu_light) ? Vector3() : sample_transmittance(sample_r, sample_mu_light);

						// Analytical integration over the step of a constant source term.
						const Vector3 safe_extinction = Vector3(MAX(extinction.x, 1e-7f), MAX(extinction.y, 1e-7f), MAX(extinction.z, 1e-7f));
						const Vector3 step_integral = (Vector3(1, 1, 1) - step_transmittance) / safe_extinction;
						luminance += throughput * light_transmittance * scattering * isotropic_phase * step_integral;
						transfer += throughput * scattering * step_integral;
						throughput *= step_transmittance;
					}

					if (hits_ground) {
						const Vector3 ground_normal = (origin + dir * t_max).normalized();
						const float ground_mu_light = ground_normal.dot(light_dir);
						luminance += throughput * sample_transmittance(rg, ground_mu_light) * MAX(0.0f, ground_mu_light) * atmosphere.ground_albedo / Math::PI;
					}

					luminance_sum += luminance;
					transfer_sum += transfer;
				}
			}

			const float inv_count = 2.0f / (MS_DIRECTIONS_SQRT * MS_DIRECTIONS_SQRT);
			const Vector3 second_order = luminance_sum * inv_count;
			// Transfer is integrated over the sphere with an isotropic phase, so its mean is the fraction
			// of light that scatters again. The geometric series 1 / (1 - f) sums all higher orders.
			const Vector3 transfer_fraction = transfer_sum * inv_count;
			const Vector3 series = Vector3(1.0f / (1.0f - MIN(transfer_fraction.x, 0.99f)), 1.0f / (1.0f - MIN(transfer_fraction.y, 0.99f)), 1.0f / (1.0f - MIN(transfer_fraction.z, 0.99f)));
			multi_scattering[y * MS + x] = second_order * series * multi_scattering_factor;
		}
	}

	Ref<Image> transmittance_image = Image::create_empty(TW, TH, false, Image::FORMAT_RGBAH);
	for (int y = 0; y < TH; y++) {
		for (int x = 0; x < TW; x++) {
			const Vector3 &v = transmittance[y * TW + x];
			transmittance_image->set_pixel(x, y, Color(v.x, v.y, v.z));
		}
	}
	Ref<Image> multi_scattering_image = Image::create_empty(MS, MS, false, Image::FORMAT_RGBAH);
	for (int y = 0; y < MS; y++) {
		for (int x = 0; x < MS; x++) {
			const Vector3 &v = multi_scattering[y * MS + x];
			multi_scattering_image->set_pixel(x, y, Color(v.x, v.y, v.z));
		}
	}

	RS *rs = RS::get_singleton();
	if (transmittance_lut.is_null()) {
		transmittance_lut = ImageTexture::create_from_image(transmittance_image);
		multi_scattering_lut = ImageTexture::create_from_image(multi_scattering_image);
		rs->material_set_param(_get_material(), "transmittance_lut", transmittance_lut->get_rid());
		rs->material_set_param(_get_material(), "multi_scattering_lut", multi_scattering_lut->get_rid());
	} else {
		transmittance_lut->update(transmittance_image);
		multi_scattering_lut->update(multi_scattering_image);
	}
}

/* Shader */

Mutex AtmosphereSkyMaterial::shader_mutex;
RID AtmosphereSkyMaterial::shader_cache[SHADER_FLAG_MAX];

int AtmosphereSkyMaterial::_get_shader_flags() const {
	int flags = 0;
	if (use_debanding) {
		flags |= SHADER_FLAG_DEBANDING;
	}
	if (night_sky.is_valid()) {
		flags |= SHADER_FLAG_NIGHT_SKY;
	}
	if (use_camera_altitude) {
		flags |= SHADER_FLAG_CAMERA_ALTITUDE;
	}
	return flags;
}

void AtmosphereSkyMaterial::_update_shader_variant() {
	_update_shader(_get_shader_flags());
	// Only set if the shader was already requested by the renderer.
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[_get_shader_flags()]);
	}
}

void AtmosphereSkyMaterial::cleanup_shader() {
	for (int i = 0; i < SHADER_FLAG_MAX; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
			shader_cache[i] = RID();
		}
	}
}

void AtmosphereSkyMaterial::_update_shader(int p_flags) {
	MutexLock shader_lock(shader_mutex);
	if (shader_cache[p_flags].is_valid()) {
		return;
	}
	shader_cache[p_flags] = RS::get_singleton()->shader_create();

	// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
	RS::get_singleton()->shader_set_code(shader_cache[p_flags], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s AtmosphereSkyMaterial.

shader_type sky;
render_mode use_quarter_res_pass%s;

// Distances are in kilometers and coefficients in 1/km.
uniform float ground_radius = 6360.0;
uniform float atmosphere_height = 100.0;
uniform vec3 ground_albedo = vec3(0.3);
uniform vec3 rayleigh_scattering = vec3(0.005802, 0.013558, 0.0331);
uniform float rayleigh_scale_height = 8.0;
uniform vec3 mie_scattering = vec3(0.003996);
uniform vec3 mie_extinction = vec3(0.00444);
uniform float mie_anisotropy = 0.8;
uniform float mie_scale_height = 1.2;
uniform vec3 ozone_absorption = vec3(0.00065, 0.001881, 0.000085);
uniform float ozone_altitude = 25.0;
uniform float ozone_width = 30.0;
uniform float observer_altitude = 0.001;
// Light energy is in lux with physical light units, otherwise it is scaled by PI like scene lights.
uniform float illuminance_scale = 3.14159265358979;
uniform float exposure = 1.0;

uniform sampler2D transmittance_lut : filter_linear, repeat_disable, hint_default_black;
uniform sampler2D multi_scattering_lut : filter_linear, repeat_disable, hint_default_black;
uniform sampler2D night_sky : filter_linear, source_color, hint_default_black;

const int SCATTERING_STEPS = 32;
const vec2 TRANSMITTANCE_LUT_SIZE = vec2(256.0, 64.0);
const vec2 MULTI_SCATTERING_LUT_SIZE = vec2(32.0);

// Distance along a ray starting at radius r with zenith cosine mu to a sphere around the planet center,
// or -1.0 if it misses.
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

bool ray_hits_ground(float r, float mu) {
	return mu < 0.0 && ray_sphere(r, mu, ground_radius) >= 0.0;
}

vec2 lut_uv(vec2 x, vec2 size) {
	return 0.5 / size + clamp(x, 0.0, 1.0) * (1.0 - 1.0 / size);
}

// Transmittance from radius r to the top of the atmosphere along zenith cosine mu.
vec3 transmittance_to_top(float r, float mu) {
	float top_radius = ground_radius + atmosphere_height;
	float horizon = sqrt((top_radius - ground_radius) * (top_radius + ground_radius));
	float rho = sqrt(max(0.0, (r - ground_radius) * (r + ground_radius)));
	float d = max(0.0, ray_sphere(r, mu, top_radius));
	float d_min = top_radius - r;
	float d_max = rho + horizon;
	vec2 x = vec2((d - d_min) / max(d_max - d_min, 1e-6), rho / horizon);
	return texture(transmittance_lut, lut_uv(x, TRANSMITTANCE_LUT_SIZE)).rgb;
}

vec3 transmittance_to_light(float r, float mu) {
	return ray_hits_ground(r, mu) ? vec3(0.0) : transmittance_to_top(r, mu);
}

// Luminance arriving at a point from second and higher order scattering, per unit illuminance.
// Multiply by the local scattering coefficient to get the light scattered towards the observer.
vec3 multi_scattering(float r, float mu) {
	vec2 x = vec2(mu * 0.5 + 0.5, (r - ground_radius) / atmosphere_height);
	return texture(multi_scattering_lut, lut_uv(x, MULTI_SCATTERING_LUT_SIZE)).rgb;
}

float rayleigh_phase(float cos_theta) {
	return 3.0 / (16.0 * PI) * (1.0 + cos_theta * cos_theta);
}

float mie_phase(float cos_theta) {
	float g = mie_anisotropy;
	float k = 1.0 + g * g - 2.0 * g * cos_theta;
	return (1.0 - g * g) / (4.0 * PI * k * sqrt(k));
}

// Integrates light scattered towards the origin along dir over [t_start, t_end]. light_dirs and
// light_colors hold up to four directional lights in their columns. Writes the transmittance of
// the segment to `transmittance`. Volumetric clouds can split the view ray around the cloud layer
// and composite their own scattering between calls to this function.
vec3 integrate_scattering(vec3 origin, vec3 dir, float t_start, float t_end, mat4 light_dirs, mat4 light_colors, int light_count, out vec3 transmittance) {
	vec3 luminance = vec3(0.0);
	transmittance = vec3(1.0);
	float prev_t = t_start;
	for (int i = 0; i < SCATTERING_STEPS; i++) {
		// Quadratic step distribution to put more samples close to the observer.
		float x = float(i + 1) / float(SCATTERING_STEPS);
		float next_t = mix(t_start, t_end, x * x);
		float dt = next_t - prev_t;
		vec3 p = origin + dir * mix(prev_t, next_t, 0.3);
		prev_t = next_t;

		float r = length(p);
		vec3 up = p / r;
		float h = max(0.0, r - ground_radius);
		float rayleigh_density = exp(-h / rayleigh_scale_height);
		float mie_density = exp(-h / mie_scale_height);
		float ozone_density = max(0.0, 1.0 - abs(h - ozone_altitude) / (ozone_width * 0.5));
		vec3 rayleigh_s = rayleigh_scattering * rayleigh_density;
		vec3 mie_s = mie_scattering * mie_density;
		vec3 extinction = rayleigh_s + mie_extinction * mie_density + ozone_absorption * ozone_density;

		vec3 source = vec3(0.0);
		for (int l = 0; l < light_count; l++) {
			vec3 light_dir = light_dirs[l].xyz;
			float mu_light = dot(up, light_dir);
			float cos_theta = dot(dir, light_dir);
			vec3 single = transmittance_to_light(r, mu_light) * (rayleigh_s * rayleigh_phase(cos_theta) + mie_s * mie_phase(cos_theta));
			source += light_colors[l].rgb * (single + multi_scattering(r, mu_light) * (rayleigh_s + mie_s));
		}

		// Energy conserving integration of a constant source term over the step (Hillaire 2015).
		vec3 step_transmittance = exp(-extinction * dt);
		luminance += transmittance * (source - source * step_transmittance) / max(extinction, vec3(1e-7));
		transmittance *= step_transmittance;
	}
	return luminance;
}

void sky() {
	// Directions and angular sizes in light_dirs, illuminance in light_colors.
	mat4 light_dirs = mat4(0.0);
	mat4 light_colors = mat4(0.0);
	int light_count = 0;
	if (LIGHT0_ENABLED) {
		light_dirs[0] = vec4(LIGHT0_DIRECTION, LIGHT0_SIZE);
		light_colors[0] = vec4(LIGHT0_COLOR * LIGHT0_ENERGY * illuminance_scale, 0.0);
		light_count = 1;
	}
	if (LIGHT1_ENABLED) {
		light_dirs[1] = vec4(LIGHT1_DIRECTION, LIGHT1_SIZE);
		light_colors[1] = vec4(LIGHT1_COLOR * LIGHT1_ENERGY * illuminance_scale, 0.0);
		light_count = 2;
	}
	if (LIGHT2_ENABLED) {
		light_dirs[2] = vec4(LIGHT2_DIRECTION, LIGHT2_SIZE);
		light_colors[2] = vec4(LIGHT2_COLOR * LIGHT2_ENERGY * illuminance_scale, 0.0);
		light_count = 3;
	}
	if (LIGHT3_ENABLED) {
		light_dirs[3] = vec4(LIGHT3_DIRECTION, LIGHT3_SIZE);
		light_colors[3] = vec4(LIGHT3_COLOR * LIGHT3_ENERGY * illuminance_scale, 0.0);
		light_count = 4;
	}

	// The observer stands on top of the planet's north pole.
	float top_radius = ground_radius + atmosphere_height;
	float r = ground_radius + max(observer_altitude%s, 0.001);
	vec3 origin = vec3(0.0, r, 0.0);
	float mu = EYEDIR.y;

	// From space, start at the top of the atmosphere, or skip it entirely.
	bool in_atmosphere = true;
	// Distance to the far side of the atmosphere, computed before moving the origin to avoid
	// precision issues on grazing rays.
	float b = r * mu;
	float t_top_far = -b + sqrt(max(0.0, b * b - (r - top_radius) * (r + top_radius)));
	if (r > top_radius) {
		float t_top = ray_sphere(r, mu, top_radius);
		in_atmosphere = t_top >= 0.0;
		if (in_atmosphere) {
			origin += EYEDIR * t_top;
			t_top_far -= t_top;
			r = length(origin);
			mu = dot(origin / r, EYEDIR);
		}
	}
	float t_ground = in_atmosphere ? ray_sphere(r, mu, ground_radius) : -1.0;
	bool hits_ground = t_ground >= 0.0;

	if (AT_QUARTER_RES_PASS) {
		vec3 luminance = vec3(0.0);
		if (in_atmosphere) {
			float t_end = hits_ground ? t_ground : max(0.0, t_top_far);
			vec3 transmittance;
			luminance = integrate_scattering(origin, EYEDIR, 0.0, t_end, light_dirs, light_colors, light_count, transmittance);
			if (hits_ground) {
				// Lambertian ground lit by attenuated light.
				vec3 normal = normalize(origin + EYEDIR * t_ground);
				vec3 ground_light = vec3(0.0);
				for (int l = 0; l < light_count; l++) {
					float mu_light = dot(normal, light_dirs[l].xyz);
					ground_light += light_colors[l].rgb * transmittance_to_top(ground_radius, mu_light) * max(mu_light, 0.0);
				}
				luminance += transmittance * ground_light * ground_albedo / PI;
			}
		}
		COLOR = luminance * exposure;
	} else {
		vec3 color = QUARTER_RES_COLOR.rgb;
		vec3 view_transmittance = hits_ground ? vec3(0.0) : (in_atmosphere ? transmittance_to_top(r, mu) : vec3(1.0));
		if (!AT_CUBEMAP_PASS) {
			// Light disks at full resolution, with physically based radiance. They are left out of the
			// radiance map since the lights already provide specular reflections.
			for (int l = 0; l < light_count; l++) {
				// Lights without an angular size get the sun's 0.5 degree disk.
				float angular_size = light_dirs[l].w > 0.0 ? light_dirs[l].w : radians(0.5);
				float cos_radius = cos(angular_size * 0.5);
				if (dot(EYEDIR, light_dirs[l].xyz) > cos_radius) {
					// Solid angle of the disk, using 1 - cos(x) = 2 sin^2(x / 2) to stay precise for tiny disks.
					float half_sin = sin(angular_size * 0.25);
					color += light_colors[l].rgb / (4.0 * PI * half_sin * half_sin) * view_transmittance * exposure;
				}
			}
		}
		%s
		COLOR = color;
	}
}
)",
																		(p_flags & SHADER_FLAG_DEBANDING) ? ", use_debanding" : "", (p_flags & SHADER_FLAG_CAMERA_ALTITUDE) ? " + POSITION.y * 0.001" : "", (p_flags & SHADER_FLAG_NIGHT_SKY) ? "color += texture(night_sky, SKY_COORDS).rgb * view_transmittance * exposure;" : ""));
}

Shader::Mode AtmosphereSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

RID AtmosphereSkyMaterial::get_shader_rid() const {
	_update_shader(_get_shader_flags());
	return shader_cache[_get_shader_flags()];
}

RID AtmosphereSkyMaterial::get_rid() const {
	_update_shader(_get_shader_flags());
	// Bake the lookup tables right away the first time the material is used, so it never renders without them.
	_update_luts();
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[_get_shader_flags()]);
		shader_set = true;
	}
	return _get_material();
}

void AtmosphereSkyMaterial::_bind_methods() {
#define BIND_ATMOSPHERE_PROPERTY(m_name, m_arg) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, m_arg), &AtmosphereSkyMaterial::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &AtmosphereSkyMaterial::get_##m_name);

	BIND_ATMOSPHERE_PROPERTY(ground_radius, "radius");
	BIND_ATMOSPHERE_PROPERTY(atmosphere_height, "height");
	BIND_ATMOSPHERE_PROPERTY(ground_albedo, "albedo");
	BIND_ATMOSPHERE_PROPERTY(rayleigh_scattering_color, "color");
	BIND_ATMOSPHERE_PROPERTY(rayleigh_scattering_scale, "scale");
	BIND_ATMOSPHERE_PROPERTY(rayleigh_scale_height, "height");
	BIND_ATMOSPHERE_PROPERTY(mie_scattering_color, "color");
	BIND_ATMOSPHERE_PROPERTY(mie_scattering_scale, "scale");
	BIND_ATMOSPHERE_PROPERTY(mie_absorption_color, "color");
	BIND_ATMOSPHERE_PROPERTY(mie_absorption_scale, "scale");
	BIND_ATMOSPHERE_PROPERTY(mie_anisotropy, "anisotropy");
	BIND_ATMOSPHERE_PROPERTY(mie_scale_height, "height");
	BIND_ATMOSPHERE_PROPERTY(ozone_absorption_color, "color");
	BIND_ATMOSPHERE_PROPERTY(ozone_absorption_scale, "scale");
	BIND_ATMOSPHERE_PROPERTY(ozone_altitude, "altitude");
	BIND_ATMOSPHERE_PROPERTY(ozone_width, "width");
	BIND_ATMOSPHERE_PROPERTY(multi_scattering_factor, "factor");
	BIND_ATMOSPHERE_PROPERTY(observer_altitude, "altitude");
	BIND_ATMOSPHERE_PROPERTY(use_camera_altitude, "enable");
	BIND_ATMOSPHERE_PROPERTY(energy_multiplier, "multiplier");
	BIND_ATMOSPHERE_PROPERTY(use_debanding, "use_debanding");
	BIND_ATMOSPHERE_PROPERTY(night_sky, "night_sky");

#undef BIND_ATMOSPHERE_PROPERTY

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "multi_scattering_factor", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_multi_scattering_factor", "get_multi_scattering_factor");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "night_sky", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_night_sky", "get_night_sky");

	ADD_GROUP("Planet", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_radius", PROPERTY_HINT_RANGE, "1,100000,1,or_greater,suffix:km"), "set_ground_radius", "get_ground_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "atmosphere_height", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:km"), "set_atmosphere_height", "get_atmosphere_height");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_albedo", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_albedo", "get_ground_albedo");

	ADD_GROUP("Observer", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "observer_altitude", PROPERTY_HINT_RANGE, "0,100000,0.1,or_greater,suffix:m"), "set_observer_altitude", "get_observer_altitude");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_camera_altitude"), "set_use_camera_altitude", "get_use_camera_altitude");

	ADD_GROUP("Rayleigh", "rayleigh_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "rayleigh_scattering_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_rayleigh_scattering_color", "get_rayleigh_scattering_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rayleigh_scattering_scale", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater,suffix:1/km"), "set_rayleigh_scattering_scale", "get_rayleigh_scattering_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rayleigh_scale_height", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:km"), "set_rayleigh_scale_height", "get_rayleigh_scale_height");

	ADD_GROUP("Mie", "mie_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mie_scattering_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mie_scattering_color", "get_mie_scattering_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_scattering_scale", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater,suffix:1/km"), "set_mie_scattering_scale", "get_mie_scattering_scale");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mie_absorption_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mie_absorption_color", "get_mie_absorption_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_absorption_scale", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater,suffix:1/km"), "set_mie_absorption_scale", "get_mie_absorption_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_anisotropy", PROPERTY_HINT_RANGE, "-0.999,0.999,0.001"), "set_mie_anisotropy", "get_mie_anisotropy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_scale_height", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:km"), "set_mie_scale_height", "get_mie_scale_height");

	ADD_GROUP("Ozone", "ozone_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ozone_absorption_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ozone_absorption_color", "get_ozone_absorption_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ozone_absorption_scale", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater,suffix:1/km"), "set_ozone_absorption_scale", "get_ozone_absorption_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ozone_altitude", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:km"), "set_ozone_altitude", "get_ozone_altitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ozone_width", PROPERTY_HINT_RANGE, "0.01,100,0.1,or_greater,suffix:km"), "set_ozone_width", "get_ozone_width");
}

AtmosphereSkyMaterial::AtmosphereSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	const bool physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	RS::get_singleton()->material_set_param(_get_material(), "illuminance_scale", physical_light_units ? 1.0f : float(Math::PI));
	// The lookup tables are baked when the renderer first requests the material.
	_update_params();
}
