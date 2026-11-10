/**************************************************************************/
/*  gcas_monitors.cpp                                                     */
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

#include "gcas_monitors.h"

#include "core/io/file_access_gcas.h"
#include "core/object/callable_mp.h"
#include "main/performance.h"

namespace {

enum Monitor {
	MONITOR_FILES_OPENED,
	MONITOR_PACK_HANDLES_OPENED,
	MONITOR_READS,
	MONITOR_BYTES_READ,
	MONITOR_BYTES_DECOMPRESSED,
	MONITOR_DECOMPRESS_TIME,
	MONITOR_READ_TIME,
	MONITOR_CACHE_HITS,
	MONITOR_CACHE_MISSES,
	MONITOR_CACHE_USED,
	MONITOR_MAX,
};

struct MonitorInfo {
	const char *id;
	Performance::MonitorType type;
};

const MonitorInfo monitors[MONITOR_MAX] = {
	{ "GCAS/files_opened", Performance::MONITOR_TYPE_QUANTITY },
	{ "GCAS/pack_handles_opened", Performance::MONITOR_TYPE_QUANTITY },
	{ "GCAS/reads", Performance::MONITOR_TYPE_QUANTITY },
	{ "GCAS/bytes_read", Performance::MONITOR_TYPE_MEMORY },
	{ "GCAS/bytes_decompressed", Performance::MONITOR_TYPE_MEMORY },
	{ "GCAS/decompress_time", Performance::MONITOR_TYPE_TIME },
	{ "GCAS/read_time", Performance::MONITOR_TYPE_TIME },
	{ "GCAS/cache_hits", Performance::MONITOR_TYPE_QUANTITY },
	{ "GCAS/cache_misses", Performance::MONITOR_TYPE_QUANTITY },
	{ "GCAS/cache_used", Performance::MONITOR_TYPE_MEMORY },
};

Variant get_monitor(int p_monitor) {
	switch (p_monitor) {
		case MONITOR_FILES_OPENED:
			return GCASIOStats::files_opened.get();
		case MONITOR_PACK_HANDLES_OPENED:
			return GCASIOStats::pack_handles_opened.get();
		case MONITOR_READS:
			return GCASIOStats::reads.get();
		case MONITOR_BYTES_READ:
			return GCASIOStats::bytes_read.get();
		case MONITOR_BYTES_DECOMPRESSED:
			return GCASIOStats::bytes_decompressed.get();
		case MONITOR_DECOMPRESS_TIME:
			return GCASIOStats::decompress_usec.get() / 1000000.0; // Time monitors are in seconds.
		case MONITOR_READ_TIME:
			return GCASIOStats::read_usec.get() / 1000000.0;
		default:
			break;
	}
	GCASChunkCache *cache = GCASChunkCache::get_singleton();
	if (!cache) {
		return 0;
	}
	Dictionary stats = cache->get_stats();
	switch (p_monitor) {
		case MONITOR_CACHE_HITS:
			return stats["hits"];
		case MONITOR_CACHE_MISSES:
			return stats["misses"];
		case MONITOR_CACHE_USED:
			return stats["used_bytes"];
		default:
			return 0;
	}
}

} // namespace

void gcas_monitors_register() {
	Performance *performance = Performance::get_singleton();
	if (!performance) {
		return;
	}
	for (int i = 0; i < MONITOR_MAX; i++) {
		if (!performance->has_custom_monitor(monitors[i].id)) {
			performance->add_custom_monitor(monitors[i].id, callable_mp_static(&get_monitor), varray(i), monitors[i].type);
		}
	}
}

void gcas_monitors_unregister() {
	Performance *performance = Performance::get_singleton();
	if (!performance) {
		return;
	}
	for (int i = 0; i < MONITOR_MAX; i++) {
		if (performance->has_custom_monitor(monitors[i].id)) {
			performance->remove_custom_monitor(monitors[i].id);
		}
	}
}
