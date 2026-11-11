/**************************************************************************/
/*  lightmap_gi_streaming.h                                               */
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

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/math/aabb.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/local_vector.h"

class Camera3D;

// Instance-rect lightmap streaming.
//
// At bake time, each lightmapped instance's rectangle is cut out of the atlas and stored with its own mip chain
// (BC6H, all layers of a mip stored together) in a `.lmstream` file. At runtime, LightmapStreamer keeps a fixed-size
// pool texture array, picks a mip per instance from its distance to the camera, loads that mip from disk on a worker
// thread, copies it into a free rectangle of the pool, and points the instance at it with
// RenderingServer::instance_geometry_set_lightmap(). The scene shaders are unchanged: they see an ordinary lightmap.

class LightmapStreamData : public Resource {
	GDCLASS(LightmapStreamData, Resource);

public:
	static constexpr uint32_t FORMAT_VERSION = 1;
	// Texels of real atlas data kept around each instance's rectangle at mip 0.
	static constexpr int CROP_BORDER = 2;
	// Texels of edge-replicated padding added around the content of every mip, so bicubic filtering at the
	// rectangle's edge never reads a neighbor in the pool.
	static constexpr int MIP_PADDING = 2;
	// The coarsest mip has no side larger than this; it always stays resident.
	static constexpr int FLOOR_MIP_SIZE = 4;

	struct Mip {
		uint32_t content_width = 0; // Size of the resized crop, excluding padding.
		uint32_t content_height = 0;
		uint32_t image_width = 0; // Padded size, a multiple of 4.
		uint32_t image_height = 0;
		uint64_t offset = 0; // Byte offset in the file of the first layer.
		uint32_t layer_size = 0; // Bytes per layer.
	};

	struct User {
		Rect2 inner; // The instance's lightmap rectangle in mip 0 content texels.
		float texel_world_size = 1.0; // Approximate world-space size of one mip 0 texel.
		AABB world_aabb;
		LocalVector<Mip> mips;
	};

	struct BuildUser {
		Rect2 uv_scale;
		int slice = 0;
		AABB world_aabb;
		float world_area = 0.0;
	};

private:
	uint32_t layer_count = 1;
	LocalVector<User> users;

protected:
	static void _bind_methods();

public:
	// Writes a stream file from the baked layers (slice-major, `p_layer_count` images per atlas slice).
	static Error build_and_save(const String &p_path, const Vector<Ref<Image>> &p_layers, int p_layer_count, const Vector<BuildUser> &p_users, float p_downscale);

	Error load_header(const String &p_path);
	// Reads all layers of one mip, concatenated. Thread-safe.
	PackedByteArray read_mip(uint32_t p_user, uint32_t p_mip) const;

	uint32_t get_layer_count() const { return layer_count; }
	uint32_t get_user_count() const { return users.size(); }
	const User &get_user(uint32_t p_user) const { return users[p_user]; }
};

class ResourceFormatLoaderLightmapStreamData : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderLightmapStreamData, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

class LightmapStreamer {
public:
	// Set by the editor so lightmaps stream around the editor camera.
	static Camera3D *(*editor_camera_func)();

	struct Placement {
		int slice = -1;
		int shelf = -1;
		Rect2i rect;

		bool is_valid() const { return slice >= 0; }
	};

private:
	// Shelf allocator for one pool slice. Shelf heights are powers of two, so freed space is easy to reuse.
	struct PoolSlice {
		struct Span {
			int x = 0;
			int width = 0;
		};
		struct Shelf {
			int y = 0;
			int height = 0;
			int used_width = 0;
			LocalVector<Span> free_spans;
		};
		LocalVector<Shelf> shelves;
		int next_y = 0;
	};

	// Shared with render thread callbacks, which may outlive the streamer.
	class PoolHandle : public RefCounted {
		GDSOFTCLASS(PoolHandle, RefCounted);

	public:
		RID rd_texture;
		bool compressed = true;
		SafeFlag ready;
	};

	struct Instance {
		ObjectID node;
		int32_t sub_instance = -1;
		uint32_t user = 0;
		int current_mip = -1;
		int finest_mip = 0; // Finest mip that fits in a pool slice.
		Placement placement;
		bool loading = false;
	};

	struct Job {
		uint32_t instance = 0;
		uint32_t user = 0;
		int mip = 0;
		Placement placement;
		PackedByteArray data;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};

	Ref<LightmapStreamData> data;
	RID lightmap_instance; // The LightmapGI instance, as passed to instance_geometry_set_lightmap().
	RID pool_texture;
	Ref<PoolHandle> pool;
	int pool_size = 2048;
	LocalVector<PoolSlice> slices;
	LocalVector<Instance> instances;
	LocalVector<Job *> jobs;
	bool floor_loaded = false;

	static void _create_pool_rd(Ref<PoolHandle> p_pool, int p_size, int p_layers);
	static void _free_pool_rd(Ref<PoolHandle> p_pool);
	static void _upload_rd(Ref<PoolHandle> p_pool, const PackedByteArray &p_data, int p_layers, Rect2i p_rect, int p_base_layer);

	Placement _allocate(int p_width, int p_height);
	void _free(const Placement &p_placement);

	PackedByteArray _prepare_layers(const PackedByteArray &p_data, uint32_t p_user, uint32_t p_mip) const;
	void _load_job(Job *p_job);
	void _apply(uint32_t p_instance, int p_mip, const Placement &p_placement, const PackedByteArray &p_data);
	RID _get_instance_rid(const Instance &p_instance) const;
	int _get_desired_mip(const Instance &p_instance, const Vector3 &p_camera_position, float p_pixel_scale, bool p_orthogonal, float p_distance_scale) const;
	void _finish_jobs(bool p_wait);
	void _load_floor_mips();

public:
	void setup(const Ref<LightmapStreamData> &p_data, RID p_lightmap_instance, int p_pool_size, int p_pool_slices);
	void add_instance(ObjectID p_node, int32_t p_sub_instance, uint32_t p_user);
	// Picks and streams mips for the given camera. Call every frame.
	void update(Camera3D *p_camera);

	RID get_pool_texture() const { return pool_texture; }

	~LightmapStreamer();
};
