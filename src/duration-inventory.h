#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounds of the duration cache. One inventory keeps at most
 * MPS_DURATION_INVENTORY_MAX_ENTRIES entries, every inventory in the process
 * shares MPS_DURATION_INVENTORY_GLOBAL_MAX_ENTRIES entries, and a tracked path
 * occupies at most MPS_DURATION_INVENTORY_MAX_PATH_BYTES bytes including its
 * terminator. Paths beyond a bound are not tracked and stay unknown. Requests
 * are measured with a bounded scan, so an over-long path is never copied,
 * compared or probed. With these bounds the cached path bytes of the whole
 * process stay below about 32 MiB plus allocation overhead. */
#define MPS_DURATION_INVENTORY_MAX_ENTRIES 4096
#define MPS_DURATION_INVENTORY_GLOBAL_MAX_ENTRIES 8192
#define MPS_DURATION_INVENTORY_MAX_PATH_BYTES 4096

struct mps_duration_inventory;

/* Cancellation handle of a single probe. */
struct mps_duration_cancel;

/* Identity of a local file. A cached duration stays valid until this identity
 * changes. Every field is 0 when the identity is not valid. */
struct mps_duration_file_id {
	/* Canonical absolute path of the file, allocated with bmalloc() and owned
	 * by the inventory. NULL when the path has no local identity, or longer
	 * than MPS_DURATION_INVENTORY_MAX_PATH_BYTES. */
	char *path;
	int64_t size;
	/* Modification time. The nanosecond part stays 0 where the platform stat()
	 * does not report it (Windows through os_stat()), so there a rewrite that
	 * keeps the size and the second is not detected. */
	int64_t mtime_sec;
	int64_t mtime_nsec;
	bool valid;
};

enum mps_duration_probe_result {
	/* No answer; the path is probed again later. */
	MPS_DURATION_PROBE_UNKNOWN = 0,
	/* *media_ms holds the duration of the media file. */
	MPS_DURATION_PROBE_DURATION,
	/* The path is not a local regular media file. */
	MPS_DURATION_PROBE_UNSUPPORTED,
};

/* Probe implementation. Both callbacks run on the worker thread with no
 * inventory lock held, so they may block on the file system. Production code
 * leaves the ops unset and gets the defaults, which use os_stat()/
 * os_get_abs_path() and a disposable private ffmpeg_source. Tests inject a
 * deterministic implementation. */
struct mps_duration_probe_ops {
	/* Cheap identity check; fills *id (bmalloc()ed path) when the path is a
	 * readable regular file. */
	bool (*identify)(void *opaque, const char *path, struct mps_duration_file_id *id);
	/* Bounded metadata probe of a local regular file. Must return promptly
	 * once mps_duration_cancel_wait() reports cancellation. */
	enum mps_duration_probe_result (*probe)(void *opaque, const char *path, struct mps_duration_cancel *cancel,
						int64_t *media_ms);
	void *opaque;
};

struct mps_duration_inventory *mps_duration_inventory_create(void);

/* Releases the inventory without waiting for the worker: a probe that is in
 * flight for this inventory finishes on the worker thread and its result is
 * discarded. Safe to call from any thread, including the OBS destruction task
 * thread. The caller must not use the inventory afterwards. */
void mps_duration_inventory_destroy(struct mps_duration_inventory *inventory);

/* Overrides the probe implementation of one inventory. Call before the
 * inventory is used. Passing NULL restores the default implementation. */
void mps_duration_inventory_set_probe(struct mps_duration_inventory *inventory,
				      const struct mps_duration_probe_ops *ops);

/* Replaces the tracked paths, normally on every playlist update. Paths are
 * copied and duplicates are tracked once; NULL, empty and over-long paths are
 * ignored and stay unknown. The per-inventory bound applies to the first
 * MPS_DURATION_INVENTORY_MAX_ENTRIES requested paths in input order, before
 * duplicates and ignored paths are removed, so a list with duplicates tracks
 * fewer than that many distinct paths. Entries that are still listed keep their
 * cached duration, cancelled probes of the previous path set are discarded, and
 * every entry is scheduled for a background revalidation. */
void mps_duration_inventory_set_paths(struct mps_duration_inventory *inventory, const char *const *paths, size_t count);

/* Looks up a cached duration in milliseconds. Pure cache access: no file
 * system work and no probing. Returns false while the duration is unknown, the
 * path is not a local regular media file, or the path is NULL, empty or longer
 * than MPS_DURATION_INVENTORY_MAX_PATH_BYTES. */
bool mps_duration_inventory_get(struct mps_duration_inventory *inventory, const char *path, int64_t *media_ms);

/* Starts the single global worker thread that serves every inventory. The worker
 * lowers its own priority best effort (below normal on Windows, increased nice
 * value on Linux, QOS_CLASS_UTILITY on macOS) so that it never competes with
 * playback; a platform that refuses the change only keeps the default priority.
 * changed (may be NULL) is called on the worker thread, without any inventory
 * lock held, whenever a cached duration changes. Opaque must stay valid until
 * mps_duration_inventory_stop() returns. Call once at module load, from a build
 * whose frontend keeps the OBS core alive; configurations without a frontend
 * must not start the worker at all, because they offer no point at which it can
 * be stopped before OBS tears its core down. */
void mps_duration_inventory_start(void (*changed)(void *), void *opaque);

/* Cancels pending probes and joins the worker thread.
 *
 * Must be called while the OBS core is still fully alive, that is from
 * OBS_FRONTEND_EVENT_EXIT before OBS tears down source types, graphics and
 * audio. Stopping at module unload alone is not a barrier: obs_shutdown() frees
 * the registered source types before it unloads modules, so a worker that is
 * still alive at that point can create a source during registry teardown.
 * Calling stop() again at module unload is allowed and harmless.
 *
 * Must not be called from a source destroy callback or from a video/audio
 * thread. A probe in flight waits for OBS's destroy task queue to finish the
 * destroy of its released probe source, and a source destroy callback runs on
 * that queue, so joining the worker from there would deadlock. */
void mps_duration_inventory_stop(void);

/* Waits until the worker has no probe in flight and no due work left for this
 * inventory, or until the worker is not running. Bounded by timeout_ms.
 * Intended for tests and benchmarks. */
bool mps_duration_inventory_wait_idle(struct mps_duration_inventory *inventory, uint32_t timeout_ms);

bool mps_duration_cancel_requested(struct mps_duration_cancel *cancel);

/* Waits up to timeout_ms for cancellation. Returns true when the probe was
 * cancelled, false on timeout. */
bool mps_duration_cancel_wait(struct mps_duration_cancel *cancel, uint32_t timeout_ms);
