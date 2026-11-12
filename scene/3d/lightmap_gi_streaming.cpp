/**************************************************************************/
/*  lightmap_gi_streaming.cpp                                             */
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

#include "lightmap_gi_streaming.h"

#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

static const uint8_t LIGHTMAP_STREAM_MAGIC[4] = { 'G', 'D', 'L', 'S' };

// Rows of padded texels compressed at once (a multiple of the 4-texel BC6H block height).
static constexpr int STRIP_HEIGHT = 1024;

static int _align4(int p_value) {
	return (p_value + 3) & ~3;
}

/////////////////////////////////////////////////////////////////////////////
// LightmapStreamData

void LightmapStreamData::_bind_methods() {
}

Error LightmapStreamData::build_and_save(const String &p_path, const Vector<Ref<Image>> &p_layers, int p_layer_count, const Vector<BuildUser> &p_users, float p_downscale) {
	ERR_FAIL_COND_V(p_layer_count < 1 || p_layers.is_empty() || p_layers.size() % p_layer_count != 0, ERR_INVALID_PARAMETER);

	// Match the atlas save path, which shrinks supersampled lightmaps before saving them.
	Vector<Ref<Image>> layers;
	layers.resize(p_layers.size());
	for (int i = 0; i < p_layers.size(); i++) {
		ERR_FAIL_COND_V(p_layers[i].is_null(), ERR_INVALID_PARAMETER);
		Ref<Image> layer = p_layers[i];
		if (p_downscale > 1.0) {
			layer = layer->duplicate();
			layer->resize(layer->get_width() / p_downscale, layer->get_height() / p_downscale, Image::INTERPOLATE_TRILINEAR);
		}
		layers.write[i] = layer;
	}
	const int width = layers[0]->get_width();
	const int height = layers[0]->get_height();
	const int slice_count = layers.size() / p_layer_count;

	// Lay out every user's mip chain first, so the table can be written before the data.
	struct Crop {
		Rect2i rect;
		int slice = 0;
	};
	LocalVector<User> out_users;
	LocalVector<Crop> crops;
	out_users.resize(p_users.size());
	crops.resize(p_users.size());
	uint64_t table_size = 16;
	for (int i = 0; i < p_users.size(); i++) {
		const BuildUser &build_user = p_users[i];
		ERR_FAIL_INDEX_V(build_user.slice, slice_count, ERR_INVALID_PARAMETER);
		const Rect2 px_rect(build_user.uv_scale.position * Vector2(width, height), build_user.uv_scale.size * Vector2(width, height));
		const int x0 = CLAMP(int(Math::floor(px_rect.position.x)) - CROP_BORDER, 0, width - 1);
		const int y0 = CLAMP(int(Math::floor(px_rect.position.y)) - CROP_BORDER, 0, height - 1);
		const int x1 = CLAMP(int(Math::ceil(px_rect.get_end().x)) + CROP_BORDER, x0 + 1, width);
		const int y1 = CLAMP(int(Math::ceil(px_rect.get_end().y)) + CROP_BORDER, y0 + 1, height);
		crops[i].rect = Rect2i(x0, y0, x1 - x0, y1 - y0);
		crops[i].slice = build_user.slice;

		User &user = out_users[i];
		user.inner = Rect2(px_rect.position - Vector2(x0, y0), px_rect.size);
		user.world_aabb = build_user.world_aabb;
		const float texel_area = MAX(px_rect.size.x * px_rect.size.y, 1.0f);
		if (build_user.world_area > CMP_EPSILON) {
			user.texel_world_size = Math::sqrt(build_user.world_area / texel_area);
		} else {
			user.texel_world_size = MAX(build_user.world_aabb.get_longest_axis_size(), 0.001f) / MAX(px_rect.size.x, px_rect.size.y);
		}

		for (int m = 0;; m++) {
			Mip mip;
			mip.content_width = MAX(1, crops[i].rect.size.x >> m);
			mip.content_height = MAX(1, crops[i].rect.size.y >> m);
			mip.image_width = _align4(mip.content_width + MIP_PADDING * 2);
			mip.image_height = _align4(mip.content_height + MIP_PADDING * 2);
			mip.layer_size = mip.image_width * mip.image_height; // BC6H is one byte per texel.
			user.mips.push_back(mip);
			if (MAX(mip.content_width, mip.content_height) <= uint32_t(FLOOR_MIP_SIZE)) {
				break;
			}
		}
		table_size += 4 + 4 * 4 + 4 + 6 * 4 + user.mips.size() * (4 * 4 + 8 + 4);
	}

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_CREATE, vformat("Can't create lightmap stream file at '%s'.", p_path));

	// Data comes after the table.
	f->seek(table_size);
	for (uint32_t i = 0; i < out_users.size(); i++) {
		User &user = out_users[i];
		const Crop &crop = crops[i];

		LocalVector<Ref<Image>> contents;
		contents.resize(p_layer_count);
		for (int l = 0; l < p_layer_count; l++) {
			Ref<Image> content = layers[crop.slice * p_layer_count + l]->get_region(crop.rect);
			content->convert(Image::FORMAT_RGBF);
			// BC6H is stored unsigned; lightmap layers are non-negative apart from filtering noise.
			float *ptr = reinterpret_cast<float *>(content->ptrw());
			const int64_t count = int64_t(content->get_width()) * content->get_height() * 3;
			for (int64_t j = 0; j < count; j++) {
				ptr[j] = Math::is_finite(ptr[j]) ? MAX(ptr[j], 0.0f) : 0.0f;
			}
			contents[l] = content;
		}

		for (uint32_t m = 0; m < user.mips.size(); m++) {
			Mip &mip = user.mips[m];
			const int cw = mip.content_width;
			const int ch = mip.content_height;
			const int iw = mip.image_width;
			const int ih = mip.image_height;

			// Each layer is compressed in strips of whole block rows. BC6H blocks are stored row by row, so the strips
			// concatenate into the layer's data, and each image stays well within Image::MAX_PIXELS.
			mip.offset = f->get_position();
			for (int l = 0; l < p_layer_count; l++) {
				Ref<Image> &content = contents[l];
				if (content->get_width() != cw || content->get_height() != ch) {
					content->resize(cw, ch, Image::INTERPOLATE_BILINEAR);
				}
				const float *src = reinterpret_cast<const float *>(content->ptr());
				for (int strip_y = 0; strip_y < ih; strip_y += STRIP_HEIGHT) {
					const int sh = MIN(STRIP_HEIGHT, ih - strip_y); // ih is a multiple of 4, so sh is too.
					Vector<uint8_t> padded;
					padded.resize(int64_t(iw) * sh * 3 * sizeof(float));
					float *dst = reinterpret_cast<float *>(padded.ptrw());
					for (int y = 0; y < sh; y++) {
						const int sy = CLAMP(strip_y + y - MIP_PADDING, 0, ch - 1);
						for (int x = 0; x < iw; x++) {
							const int sx = CLAMP(x - MIP_PADDING, 0, cw - 1);
							const float *s = &src[(int64_t(sy) * cw + sx) * 3];
							float *d = &dst[(int64_t(y) * iw + x) * 3];
							d[0] = s[0];
							d[1] = s[1];
							d[2] = s[2];
						}
					}

					Ref<Image> image = Image::create_from_data(iw, sh, false, Image::FORMAT_RGBF, padded);
					image->convert(Image::FORMAT_RGBH);
					Error err = image->compress_from_channels(Image::COMPRESS_BPTC, Image::USED_CHANNELS_RGB);
					ERR_FAIL_COND_V_MSG(err != OK || image->get_format() != Image::FORMAT_BPTC_RGBFU, ERR_CANT_CREATE, "Couldn't compress a streamed lightmap rectangle to BC6H.");
					const Vector<uint8_t> compressed = image->get_data();
					ERR_FAIL_COND_V(compressed.size() != int64_t(iw) * sh, ERR_BUG);
					f->store_buffer(compressed);
				}
			}
		}
	}

	f->seek(0);
	f->store_buffer(LIGHTMAP_STREAM_MAGIC, 4);
	f->store_32(FORMAT_VERSION);
	f->store_32(p_layer_count);
	f->store_32(out_users.size());
	for (const User &user : out_users) {
		f->store_32(user.mips.size());
		f->store_float(user.inner.position.x);
		f->store_float(user.inner.position.y);
		f->store_float(user.inner.size.x);
		f->store_float(user.inner.size.y);
		f->store_float(user.texel_world_size);
		f->store_float(user.world_aabb.position.x);
		f->store_float(user.world_aabb.position.y);
		f->store_float(user.world_aabb.position.z);
		f->store_float(user.world_aabb.size.x);
		f->store_float(user.world_aabb.size.y);
		f->store_float(user.world_aabb.size.z);
		for (const Mip &mip : user.mips) {
			f->store_32(mip.content_width);
			f->store_32(mip.content_height);
			f->store_32(mip.image_width);
			f->store_32(mip.image_height);
			f->store_64(mip.offset);
			f->store_32(mip.layer_size);
		}
	}
	ERR_FAIL_COND_V(f->get_position() != table_size, ERR_BUG);
	return f->get_error() == ERR_FILE_EOF ? OK : f->get_error();
}

Error LightmapStreamData::load_header(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_OPEN, vformat("Can't open lightmap stream file '%s'.", p_path));

	uint8_t magic[4] = {};
	f->get_buffer(magic, 4);
	ERR_FAIL_COND_V_MSG(memcmp(magic, LIGHTMAP_STREAM_MAGIC, 4) != 0, ERR_FILE_UNRECOGNIZED, vformat("'%s' is not a lightmap stream file.", p_path));
	ERR_FAIL_COND_V_MSG(f->get_32() != FORMAT_VERSION, ERR_FILE_UNRECOGNIZED, vformat("'%s' uses an unsupported lightmap stream version. Bake lightmaps again.", p_path));

	const uint64_t file_length = f->get_length();
	layer_count = f->get_32();
	const uint32_t user_count = f->get_32();
	ERR_FAIL_COND_V(layer_count == 0 || layer_count > 64, ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(uint64_t(user_count) * 48 > file_length, ERR_FILE_CORRUPT);

	users.clear();
	users.resize(user_count);
	for (User &user : users) {
		const uint32_t mip_count = f->get_32();
		ERR_FAIL_COND_V(mip_count == 0 || mip_count > 32, ERR_FILE_CORRUPT);
		user.inner.position.x = f->get_float();
		user.inner.position.y = f->get_float();
		user.inner.size.x = f->get_float();
		user.inner.size.y = f->get_float();
		user.texel_world_size = MAX(f->get_float(), 0.000001f);
		user.world_aabb.position.x = f->get_float();
		user.world_aabb.position.y = f->get_float();
		user.world_aabb.position.z = f->get_float();
		user.world_aabb.size.x = f->get_float();
		user.world_aabb.size.y = f->get_float();
		user.world_aabb.size.z = f->get_float();
		user.mips.resize(mip_count);
		for (Mip &mip : user.mips) {
			mip.content_width = f->get_32();
			mip.content_height = f->get_32();
			mip.image_width = f->get_32();
			mip.image_height = f->get_32();
			mip.offset = f->get_64();
			mip.layer_size = f->get_32();
			ERR_FAIL_COND_V(mip.content_width == 0 || mip.content_height == 0 || mip.image_width % 4 != 0 || mip.image_height % 4 != 0, ERR_FILE_CORRUPT);
			ERR_FAIL_COND_V(mip.layer_size != mip.image_width * mip.image_height, ERR_FILE_CORRUPT);
			ERR_FAIL_COND_V(mip.offset + uint64_t(mip.layer_size) * layer_count > file_length, ERR_FILE_CORRUPT);
		}
	}
	ERR_FAIL_COND_V(f->eof_reached(), ERR_FILE_CORRUPT);
	return OK;
}

PackedByteArray LightmapStreamData::read_mip(uint32_t p_user, uint32_t p_mip) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_user, users.size(), PackedByteArray());
	ERR_FAIL_UNSIGNED_INDEX_V(p_mip, users[p_user].mips.size(), PackedByteArray());
	if (stale.is_set()) {
		return PackedByteArray();
	}
	Ref<FileAccess> f = FileAccess::open(get_path(), FileAccess::READ);
	ERR_FAIL_COND_V(f.is_null(), PackedByteArray());
	return read_mip(p_user, p_mip, f);
}

PackedByteArray LightmapStreamData::read_mip(uint32_t p_user, uint32_t p_mip, const Ref<FileAccess> &p_file) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_user, users.size(), PackedByteArray());
	ERR_FAIL_UNSIGNED_INDEX_V(p_mip, users[p_user].mips.size(), PackedByteArray());
	ERR_FAIL_COND_V(p_file.is_null(), PackedByteArray());
	if (stale.is_set()) {
		return PackedByteArray();
	}
	const Mip &mip = users[p_user].mips[p_mip];

	p_file->seek(mip.offset);
	PackedByteArray data;
	data.resize(int64_t(mip.layer_size) * layer_count);
	const uint64_t read = p_file->get_buffer(data.ptrw(), data.size());
	ERR_FAIL_COND_V(read != uint64_t(data.size()), PackedByteArray());
	return data;
}

/////////////////////////////////////////////////////////////////////////////
// ResourceFormatLoaderLightmapStreamData

Ref<Resource> ResourceFormatLoaderLightmapStreamData::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<LightmapStreamData> stream;
	stream.instantiate();
	const Error err = stream->load_header(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return stream;
}

void ResourceFormatLoaderLightmapStreamData::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("lmstream");
}

bool ResourceFormatLoaderLightmapStreamData::handles_type(const String &p_type) const {
	return p_type == "LightmapStreamData";
}

String ResourceFormatLoaderLightmapStreamData::get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "lmstream") {
		return "LightmapStreamData";
	}
	return "";
}

/////////////////////////////////////////////////////////////////////////////
// LightmapStreamer

Camera3D *(*LightmapStreamer::editor_camera_func)() = nullptr;

// Loads in flight at once.
static constexpr int MAX_JOBS = 16;
// A finer mip is dropped only once the camera is this much further than needed, so mips don't flip back and forth.
static constexpr float DOWNGRADE_HYSTERESIS = 1.5f;

void LightmapStreamer::_create_pool_rd(Ref<PoolHandle> p_pool, int p_size, int p_layers) {
#ifndef RD_ENABLED
	ERR_FAIL_MSG("Lightmap streaming requires the Forward+ or Mobile renderer.");
#else
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_MSG(rd, "Lightmap streaming requires the Forward+ or Mobile renderer.");

	const BitField<RD::TextureUsageBits> usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	p_pool->compressed = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_BC6H_UFLOAT_BLOCK, usage);

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.format = p_pool->compressed ? RD::DATA_FORMAT_BC6H_UFLOAT_BLOCK : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = p_size;
	tf.height = p_size;
	tf.array_layers = p_layers;
	tf.usage_bits = usage;

	// Start black, so filtering past an instance's padding never reads undefined memory.
	Vector<uint8_t> zero;
	zero.resize_initialized(int64_t(p_size) * p_size * (p_pool->compressed ? 1 : 8));
	Vector<Vector<uint8_t>> initial;
	initial.resize(p_layers);
	for (int i = 0; i < p_layers; i++) {
		initial.write[i] = zero;
	}

	p_pool->rd_texture = rd->texture_create(tf, RD::TextureView(), initial);
	ERR_FAIL_COND(p_pool->rd_texture.is_null());
	rd->set_resource_name(p_pool->rd_texture, "Lightmap Streaming Pool");
	p_pool->ready.set();
#endif // RD_ENABLED
}

void LightmapStreamer::_free_pool_rd(Ref<PoolHandle> p_pool) {
#ifdef RD_ENABLED
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd && p_pool->rd_texture.is_valid()) {
		rd->free_rid(p_pool->rd_texture);
	}
#endif // RD_ENABLED
	p_pool->rd_texture = RID();
}

void LightmapStreamer::_upload_rd(Ref<PoolHandle> p_pool, const PackedByteArray &p_data, int p_layers, Rect2i p_rect, int p_base_layer) {
#ifdef RD_ENABLED
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_pool->rd_texture.is_null());

	const int64_t layer_size = p_data.size() / p_layers;
	Vector<Vector<uint8_t>> layers;
	layers.resize(p_layers);
	for (int i = 0; i < p_layers; i++) {
		layers.write[i] = p_data.slice(i * layer_size, (i + 1) * layer_size);
	}

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.format = p_pool->compressed ? RD::DATA_FORMAT_BC6H_UFLOAT_BLOCK : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = p_rect.size.x;
	tf.height = p_rect.size.y;
	tf.array_layers = p_layers;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	RID staging = rd->texture_create(tf, RD::TextureView(), layers);
	ERR_FAIL_COND(staging.is_null());

	for (int i = 0; i < p_layers; i++) {
		rd->texture_copy(staging, p_pool->rd_texture, Vector3(), Vector3(p_rect.position.x, p_rect.position.y, 0), Vector3(p_rect.size.x, p_rect.size.y, 1), 0, 0, i, p_base_layer + i);
	}
	rd->free_rid(staging);
#endif // RD_ENABLED
}

LightmapStreamer::Placement LightmapStreamer::_allocate(int p_width, int p_height) {
	Placement placement;
	ERR_FAIL_COND_V(p_width > pool_size || p_height > pool_size, placement);
	const int shelf_height = MAX(int(Math::next_power_of_2(uint32_t(p_height))), 4);

	for (uint32_t s = 0; s < slices.size(); s++) {
		PoolSlice &slice = slices[s];
		int shelf_index = -1;
		int span_index = -1;
		for (uint32_t i = 0; i < slice.shelves.size() && shelf_index < 0; i++) {
			PoolSlice::Shelf &shelf = slice.shelves[i];
			// An empty shelf may be reused by a slightly smaller class.
			const bool fits_class = shelf.height == shelf_height || (shelf.used_width == 0 && shelf.height >= shelf_height && shelf.height <= shelf_height * 2);
			if (!fits_class) {
				continue;
			}
			for (uint32_t j = 0; j < shelf.free_spans.size(); j++) {
				if (shelf.free_spans[j].width >= p_width) {
					shelf_index = i;
					span_index = j;
					break;
				}
			}
		}

		if (shelf_index < 0 && slice.next_y + shelf_height <= pool_size) {
			PoolSlice::Shelf shelf;
			shelf.y = slice.next_y;
			shelf.height = shelf_height;
			shelf.free_spans.push_back({ 0, pool_size });
			slice.shelves.push_back(shelf);
			slice.next_y += shelf_height;
			shelf_index = slice.shelves.size() - 1;
			span_index = 0;
		}

		if (shelf_index >= 0) {
			PoolSlice::Shelf &shelf = slice.shelves[shelf_index];
			PoolSlice::Span &span = shelf.free_spans[span_index];
			placement.slice = s;
			placement.shelf = shelf_index;
			placement.rect = Rect2i(span.x, shelf.y, p_width, p_height);
			span.x += p_width;
			span.width -= p_width;
			if (span.width == 0) {
				shelf.free_spans.remove_at(span_index);
			}
			shelf.used_width += p_width;
			return placement;
		}
	}
	return placement;
}

void LightmapStreamer::_free(const Placement &p_placement) {
	ERR_FAIL_UNSIGNED_INDEX(uint32_t(p_placement.slice), slices.size());
	PoolSlice &slice = slices[p_placement.slice];
	ERR_FAIL_UNSIGNED_INDEX(uint32_t(p_placement.shelf), slice.shelves.size());
	PoolSlice::Shelf &shelf = slice.shelves[p_placement.shelf];

	shelf.used_width -= p_placement.rect.size.x;
	if (shelf.used_width <= 0) {
		shelf.used_width = 0;
		shelf.free_spans.clear();
		shelf.free_spans.push_back({ 0, pool_size });
		// Shelves are only ever added at the top, so the vector stays sorted by y. Give the space of empty
		// shelves at the top back, so it can be cut into shelves of another height.
		while (!slice.shelves.is_empty() && slice.shelves[slice.shelves.size() - 1].used_width == 0) {
			slice.next_y = slice.shelves[slice.shelves.size() - 1].y;
			slice.shelves.resize(slice.shelves.size() - 1);
		}
		return;
	}

	// Insert sorted by x and merge with neighbors.
	uint32_t index = 0;
	while (index < shelf.free_spans.size() && shelf.free_spans[index].x < p_placement.rect.position.x) {
		index++;
	}
	shelf.free_spans.insert(index, { p_placement.rect.position.x, p_placement.rect.size.x });
	if (index + 1 < shelf.free_spans.size() && shelf.free_spans[index].x + shelf.free_spans[index].width == shelf.free_spans[index + 1].x) {
		shelf.free_spans[index].width += shelf.free_spans[index + 1].width;
		shelf.free_spans.remove_at(index + 1);
	}
	if (index > 0 && shelf.free_spans[index - 1].x + shelf.free_spans[index - 1].width == shelf.free_spans[index].x) {
		shelf.free_spans[index - 1].width += shelf.free_spans[index].width;
		shelf.free_spans.remove_at(index);
	}
}

PackedByteArray LightmapStreamer::_prepare_layers(const PackedByteArray &p_data, uint32_t p_user, uint32_t p_mip) const {
	if (p_data.is_empty() || pool->compressed) {
		return p_data;
	}

	// No BC6H support: decompress to RGBA16F.
	const LightmapStreamData::Mip &mip = data->get_user(p_user).mips[p_mip];
	const uint32_t layer_count = data->get_layer_count();
	PackedByteArray out;
	for (uint32_t l = 0; l < layer_count; l++) {
		Ref<Image> image = Image::create_from_data(mip.image_width, mip.image_height, false, Image::FORMAT_BPTC_RGBFU, p_data.slice(l * mip.layer_size, (l + 1) * mip.layer_size));
		ERR_FAIL_COND_V(image.is_null() || image->decompress() != OK, PackedByteArray());
		image->convert(Image::FORMAT_RGBAH);
		out.append_array(image->get_data());
	}
	return out;
}

void LightmapStreamer::_load_job(Job *p_job) {
	p_job->data = _prepare_layers(data->read_mip(p_job->user, p_job->mip), p_job->user, p_job->mip);
}

RID LightmapStreamer::_get_instance_rid(const Instance &p_instance) const {
	Object *node = ObjectDB::get_instance(p_instance.node);
	if (!node) {
		return RID();
	}
	if (p_instance.sub_instance >= 0) {
		return node->call("get_bake_mesh_instance", p_instance.sub_instance);
	}
	VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
	return vi ? vi->get_instance() : RID();
}

void LightmapStreamer::_set_lightmap(const Instance &p_instance, int p_mip, const Placement &p_placement) {
	const LightmapStreamData::User &user = data->get_user(p_instance.user);
	const LightmapStreamData::Mip &mip0 = user.mips[0];
	const LightmapStreamData::Mip &mip = user.mips[p_mip];
	const Vector2 scale(float(mip.content_width) / mip0.content_width, float(mip.content_height) / mip0.content_height);
	const Vector2 origin = Vector2(p_placement.rect.position) + Vector2(LightmapStreamData::MIP_PADDING, LightmapStreamData::MIP_PADDING);
	const Rect2 uv_rect((origin + user.inner.position * scale) / pool_size, user.inner.size * scale / pool_size);
	const RID rid = _get_instance_rid(p_instance);
	if (rid.is_valid()) {
		RS::get_singleton()->instance_geometry_set_lightmap(rid, lightmap_instance, uv_rect, p_placement.slice);
	}
}

void LightmapStreamer::_apply(uint32_t p_instance, int p_mip, const Placement &p_placement, const PackedByteArray &p_data) {
	Instance &instance = instances[p_instance];
	const int layer_count = data->get_layer_count();
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&LightmapStreamer::_upload_rd).bind(pool, p_data, layer_count, p_placement.rect, p_placement.slice * layer_count));
	// Queued after the upload, so the instance never samples the rectangle before its data is there.
	_set_lightmap(instance, p_mip, p_placement);

	if (instance.placement.is_valid()) {
		_free(instance.placement);
	}
	instance.placement = p_placement;
	instance.current_mip = p_mip;
}

void LightmapStreamer::_use_floor(uint32_t p_instance) {
	Instance &instance = instances[p_instance];
	ERR_FAIL_COND(!instance.floor_placement.is_valid());
	const int floor_mip = data->get_user(instance.user).mips.size() - 1;
	_set_lightmap(instance, floor_mip, instance.floor_placement);
	if (instance.placement.is_valid()) {
		_free(instance.placement);
		instance.placement = Placement();
	}
	instance.current_mip = floor_mip;
}

int LightmapStreamer::_get_desired_mip(const Instance &p_instance, const Vector3 &p_camera_position, float p_pixel_scale, bool p_orthogonal, float p_distance_scale) const {
	const LightmapStreamData::User &user = data->get_user(p_instance.user);
	const AABB &aabb = user.world_aabb;
	const Vector3 closest = p_camera_position.clamp(aabb.position, aabb.get_end());
	const float distance = p_camera_position.distance_to(closest) * p_distance_scale;
	const float world_per_pixel = p_orthogonal ? p_pixel_scale : MAX(distance, 0.01f) * p_pixel_scale;
	const float texels_per_pixel = world_per_pixel / user.texel_world_size;
	const int mip = texels_per_pixel <= 1.0f ? 0 : int(Math::floor(Math::log2(texels_per_pixel)));
	return CLAMP(mip, p_instance.finest_mip, int(user.mips.size()) - 1);
}

void LightmapStreamer::_finish_jobs(bool p_wait) {
	for (uint32_t i = 0; i < jobs.size();) {
		Job *job = jobs[i];
		if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(job->task)) {
			i++;
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
		Instance &instance = instances[job->instance];
		instance.loading = false;
		if (p_wait || job->data.is_empty()) {
			_free(job->placement);
		} else {
			_apply(job->instance, job->mip, job->placement, job->data);
		}
		memdelete(job);
		jobs.remove_at_unordered(i);
	}
}

void LightmapStreamer::_load_floor_mips() {
	Ref<FileAccess> f = FileAccess::open(data->get_path(), FileAccess::READ);
	ERR_FAIL_COND_MSG(f.is_null(), vformat("Can't open lightmap stream file \"%s\".", data->get_path()));
	for (uint32_t i = 0; i < instances.size(); i++) {
		Instance &instance = instances[i];
		const LightmapStreamData::User &user = data->get_user(instance.user);
		const int mip = user.mips.size() - 1;
		const Placement placement = _allocate(user.mips[mip].image_width, user.mips[mip].image_height);
		ERR_CONTINUE_MSG(!placement.is_valid(), "Lightmap streaming pool is too small for even the lowest-resolution lightmaps. Increase the streaming pool size or slice count.");
		const PackedByteArray layers = _prepare_layers(data->read_mip(instance.user, mip, f), instance.user, mip);
		if (layers.is_empty()) {
			_free(placement);
			continue;
		}
		const int layer_count = data->get_layer_count();
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&LightmapStreamer::_upload_rd).bind(pool, layers, layer_count, placement.rect, placement.slice * layer_count));
		instance.floor_placement = placement;
		_use_floor(i);
	}
}

void LightmapStreamer::setup(const Ref<LightmapStreamData> &p_data, RID p_lightmap_instance, int p_pool_size, int p_pool_slices) {
	ERR_FAIL_COND(p_data.is_null());
	data = p_data;
	lightmap_instance = p_lightmap_instance;
	pool_size = p_pool_size;
	slices.resize(MAX(p_pool_slices, 1));
	pool.instantiate();
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&LightmapStreamer::_create_pool_rd).bind(pool, pool_size, int(slices.size() * data->get_layer_count())));
}

void LightmapStreamer::add_instance(ObjectID p_node, int32_t p_sub_instance, uint32_t p_user) {
	ERR_FAIL_COND(data.is_null());
	ERR_FAIL_UNSIGNED_INDEX(p_user, data->get_user_count());
	Instance instance;
	instance.node = p_node;
	instance.sub_instance = p_sub_instance;
	instance.user = p_user;
	const LightmapStreamData::User &user = data->get_user(p_user);
	instance.finest_mip = user.mips.size() - 1;
	for (uint32_t m = 0; m < user.mips.size(); m++) {
		if (user.mips[m].image_width <= uint32_t(pool_size) && user.mips[m].image_height <= uint32_t(pool_size)) {
			instance.finest_mip = m;
			break;
		}
	}
	instances.push_back(instance);
}

void LightmapStreamer::update(Camera3D *p_camera, const Transform3D &p_lightmap_transform) {
	if (pool.is_null() || !pool->ready.is_set()) {
		return;
	}
	if (pool_texture.is_null()) {
		pool_texture = RS::get_singleton()->texture_rd_create(pool->rd_texture, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}
	if (!floor_loaded) {
		floor_loaded = true;
		_load_floor_mips();
	}

	_finish_jobs(false);

	if (!p_camera || !p_camera->is_inside_tree() || !p_camera->get_viewport()) {
		return;
	}

	const Projection projection = p_camera->get_camera_projection();
	const float viewport_height = MAX(p_camera->get_viewport()->get_visible_rect().size.y, 1.0f);
	const bool orthogonal = projection.is_orthogonal();
	// World size of one pixel per unit of distance (perspective), or in absolute terms (orthogonal).
	float pixel_scale = 2.0f / (MAX(Math::abs(projection.columns[1][1]), 0.0001f) * viewport_height);
	// Bounds and texel sizes are baked in LightmapGI space.
	const Vector3 camera_position = p_lightmap_transform.affine_inverse().xform(p_camera->get_global_position());
	if (orthogonal) {
		const Vector3 scale = p_lightmap_transform.basis.get_scale_abs();
		pixel_scale /= MAX((scale.x + scale.y + scale.z) / 3.0f, 0.0001f);
	}

	struct Request {
		uint32_t instance = 0;
		int mip = 0;
		float distance = 0.0;
		bool downgrade = false;

		bool operator<(const Request &p_other) const {
			// Coarser mips first: they free pool space. Then the closest instances.
			if (downgrade != p_other.downgrade) {
				return downgrade;
			}
			return distance < p_other.distance;
		}
	};
	LocalVector<Request> requests;
	for (uint32_t i = 0; i < instances.size(); i++) {
		const Instance &instance = instances[i];
		if (instance.loading || instance.current_mip < 0) {
			continue;
		}
		const int finer = _get_desired_mip(instance, camera_position, pixel_scale, orthogonal, 1.0f);
		const int coarser = _get_desired_mip(instance, camera_position, pixel_scale, orthogonal, 1.0f / DOWNGRADE_HYSTERESIS);
		Request request;
		request.instance = i;
		if (finer < instance.current_mip) {
			request.mip = finer;
		} else if (coarser > instance.current_mip) {
			request.mip = coarser;
			request.downgrade = true;
		} else {
			continue;
		}
		const AABB &aabb = data->get_user(instance.user).world_aabb;
		request.distance = camera_position.distance_to(camera_position.clamp(aabb.position, aabb.get_end()));
		requests.push_back(request);
	}
	requests.sort();

	for (const Request &request : requests) {
		if (jobs.size() >= MAX_JOBS) {
			break;
		}
		Instance &instance = instances[request.instance];
		const LightmapStreamData::User &user = data->get_user(instance.user);
		if (request.mip == int(user.mips.size()) - 1) {
			// The floor mip is always resident.
			_use_floor(request.instance);
			continue;
		}
		// If the wanted mip doesn't fit, an upgrade settles for the finest mip that does.
		int mip_index = request.mip;
		const int last_mip = request.downgrade ? request.mip : instance.current_mip - 1;
		Placement placement;
		for (; mip_index <= last_mip; mip_index++) {
			placement = _allocate(user.mips[mip_index].image_width, user.mips[mip_index].image_height);
			if (placement.is_valid()) {
				break;
			}
		}
		if (!placement.is_valid() && !request.downgrade && instance.placement.is_valid()) {
			// The finer mip may fit in the space this instance already holds. Try with it freed, and put the
			// slice back as it was if that doesn't help.
			const int old_slice = instance.placement.slice;
			const PoolSlice saved = slices[old_slice];
			_free(instance.placement);
			for (mip_index = request.mip; mip_index < instance.current_mip; mip_index++) {
				placement = _allocate(user.mips[mip_index].image_width, user.mips[mip_index].image_height);
				if (placement.is_valid()) {
					break;
				}
			}
			if (placement.is_valid()) {
				// The new rectangle may overlap the old one, so show the floor mip until it's loaded.
				instance.placement = Placement();
				_use_floor(request.instance);
			} else {
				slices[old_slice] = saved;
			}
		}
		if (!placement.is_valid()) {
			if (request.downgrade) {
				// Drop to the floor mip to free this instance's space. It moves up again once there is room.
				_use_floor(request.instance);
			}
			// Otherwise try again once something further away has been dropped.
			continue;
		}
		Job *job = memnew(Job);
		job->instance = request.instance;
		job->user = instance.user;
		job->mip = mip_index;
		job->placement = placement;
		job->task = WorkerThreadPool::get_singleton()->add_template_task(this, &LightmapStreamer::_load_job, job, false, "Lightmap streaming");
		instance.loading = true;
		jobs.push_back(job);
	}
}

LightmapStreamer::~LightmapStreamer() {
	_finish_jobs(true);
	if (pool_texture.is_valid()) {
		RS::get_singleton()->free_rid(pool_texture);
	}
	if (pool.is_valid()) {
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&LightmapStreamer::_free_pool_rd).bind(pool));
	}
}
