#include "duration-inventory.h"

#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#endif

#include <obs.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

/* A cached identity is checked against the file system this often. */
#define RECHECK_INTERVAL_NS (UINT64_C(10) * 1000 * 1000 * 1000)
/* A probe that returned no answer is retried this often. */
#define RETRY_INTERVAL_NS (UINT64_C(30) * 1000 * 1000 * 1000)
/* A single media probe never runs longer than this. Public OBS has no
 * metadata-ready signal, so the probe polls the source state instead. */
#define PROBE_TIMEOUT_NS (UINT64_C(2) * 1000 * 1000 * 1000)
#define PROBE_POLL_MS 10
/* Bounds demuxing of the disposable probe source; playback is not affected. */
#define PROBE_FFMPEG_OPTIONS "analyzeduration=2000000 probesize=4194304"
/* Upper bound of one idle wait, so a wakeup that was not observed cannot park
 * the worker indefinitely. */
#define WORKER_WAIT_MAX_MS 10000

enum duration_entry_state {
	DURATION_ENTRY_UNKNOWN = 0,
	DURATION_ENTRY_KNOWN,
	DURATION_ENTRY_INVALID,
};

struct mps_duration_entry {
	/* Path as supplied by the playlist; the cache lookup key. */
	char *path;
	/* Local file identity the cached result belongs to. */
	struct mps_duration_file_id id;
	int64_t media_ms;
	uint64_t next_check_ns;
	enum duration_entry_state state;
};

struct mps_duration_inventory {
	pthread_mutex_t mutex;
	struct mps_duration_inventory *next;
	DARRAY(struct mps_duration_entry) entries;
	size_t scan_cursor;
	size_t references;
	struct mps_duration_cancel *active_cancel;
	struct mps_duration_probe_ops probe;
	uint64_t generation;
	bool linked;
	bool destroying;
};

struct mps_duration_cancel {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool requested;
};

struct mps_duration_job {
	struct mps_duration_inventory *inventory;
	struct mps_duration_probe_ops probe;
	char *path;
	uint64_t generation;
	struct mps_duration_cancel cancel;
};

/* Lock order: worker mutex before inventory mutex before cancel mutex. */
static struct {
	pthread_mutex_t mutex;
	pthread_cond_t idle_cond;
	os_event_t *wakeup;
	pthread_t thread;
	struct mps_duration_inventory *inventories;
	size_t entry_total;
	size_t round_robin;
	void (*changed)(void *);
	void *changed_opaque;
	bool running;
} duration_worker = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.idle_cond = PTHREAD_COND_INITIALIZER,
};

static const struct mps_duration_probe_ops duration_default_probe;

static uint64_t duration_now(void)
{
	return os_gettime_ns();
}

static void duration_realtime_deadline(struct timespec *deadline, uint32_t timeout_ms)
{
	timespec_get(deadline, TIME_UTC);
	deadline->tv_sec += (time_t)(timeout_ms / 1000);
	deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec += 1;
		deadline->tv_nsec -= 1000000000L;
	}
}

static void duration_cancel_init(struct mps_duration_cancel *cancel)
{
	pthread_mutex_init(&cancel->mutex, NULL);
	pthread_cond_init(&cancel->cond, NULL);
	cancel->requested = false;
}

static void duration_cancel_free(struct mps_duration_cancel *cancel)
{
	pthread_mutex_destroy(&cancel->mutex);
	pthread_cond_destroy(&cancel->cond);
}

static void duration_cancel_request(struct mps_duration_cancel *cancel)
{
	if (!cancel)
		return;

	pthread_mutex_lock(&cancel->mutex);
	cancel->requested = true;
	pthread_cond_broadcast(&cancel->cond);
	pthread_mutex_unlock(&cancel->mutex);
}

bool mps_duration_cancel_requested(struct mps_duration_cancel *cancel)
{
	bool requested;

	if (!cancel)
		return true;

	pthread_mutex_lock(&cancel->mutex);
	requested = cancel->requested;
	pthread_mutex_unlock(&cancel->mutex);
	return requested;
}

bool mps_duration_cancel_wait(struct mps_duration_cancel *cancel, uint32_t timeout_ms)
{
	struct timespec deadline;
	bool requested;

	if (!cancel)
		return true;

	duration_realtime_deadline(&deadline, timeout_ms);
	pthread_mutex_lock(&cancel->mutex);
	while (!cancel->requested) {
		if (pthread_cond_timedwait(&cancel->cond, &cancel->mutex, &deadline) != 0)
			break;
	}
	requested = cancel->requested;
	pthread_mutex_unlock(&cancel->mutex);
	return requested;
}

static void duration_entry_clear_id(struct mps_duration_entry *entry)
{
	bfree(entry->id.path);
	entry->id = (struct mps_duration_file_id){0};
}

static void duration_entry_free(struct mps_duration_entry *entry)
{
	bfree(entry->path);
	duration_entry_clear_id(entry);
	*entry = (struct mps_duration_entry){0};
}

/* Takes ownership of id->path. */
static void duration_entry_adopt_id(struct mps_duration_entry *entry, struct mps_duration_file_id *id)
{
	duration_entry_clear_id(entry);
	entry->id = *id;
	*id = (struct mps_duration_file_id){0};
}

static bool duration_id_matches(const struct mps_duration_file_id *left, const struct mps_duration_file_id *right)
{
	if (left->valid != right->valid)
		return false;
	if (!left->valid || !right->valid)
		return true;

	return left->size == right->size && left->mtime_sec == right->mtime_sec &&
	       left->mtime_nsec == right->mtime_nsec && left->path && right->path &&
	       strcmp(left->path, right->path) == 0;
}

/* Sub-second part of the modification time. os_stat() only reports whole
 * seconds on Windows, so a same-size rewrite within one second is not detected
 * there; the field stays 0 and the identity comparison sees no change. */
static int64_t duration_stat_mtime_nsec(const struct stat *info)
{
#if defined(__APPLE__)
	return (int64_t)info->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
	UNUSED_PARAMETER(info);
	return 0;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
	return (int64_t)info->st_mtim.tv_nsec;
#else
	/* Strict ISO builds hide st_mtim, so only whole seconds are compared. */
	UNUSED_PARAMETER(info);
	return 0;
#endif
}

static bool duration_identify(void *opaque, const char *path, struct mps_duration_file_id *id)
{
	struct stat info;
	char *absolute;

	UNUSED_PARAMETER(opaque);
	*id = (struct mps_duration_file_id){0};

	if (!path || !*path || strstr(path, "://"))
		return false;

	absolute = os_get_abs_path_ptr(path);
	if (!absolute)
		absolute = bstrdup(path);
	if (!absolute)
		return false;

	if (os_stat(absolute, &info) != 0 || !S_ISREG(info.st_mode)) {
		bfree(absolute);
		return false;
	}

	id->path = absolute;
	id->size = (int64_t)info.st_size;
	id->mtime_sec = (int64_t)info.st_mtime;
	id->mtime_nsec = duration_stat_mtime_nsec(&info);
	id->valid = true;
	return true;
}

static obs_source_t *duration_create_probe_source(const char *path)
{
	obs_data_t *settings = obs_data_create();
	obs_source_t *source;

	if (!settings)
		return NULL;

	/* A local path is opened as a non-local input on purpose: mp_media only
	 * installs its interrupt_callback for non-local inputs, which is the only
	 * cancellation the public OBS API offers while a demuxer is opening. The
	 * source is private and never activated, so it opens the file without
	 * starting playback. */
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "input", path);
	obs_data_set_bool(settings, "seekable", false);
	/* close_when_inactive keeps the source from opening the file at all. */
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "hw_decode", false);
	obs_data_set_bool(settings, "log_changes", false);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_string(settings, "ffmpeg_options", PROBE_FFMPEG_OPTIONS);

	source = obs_source_create_private("ffmpeg_source", "mps duration probe", settings);
	obs_data_release(settings);
	return source;
}

/* Runs on the OBS destruction task thread after every task queued before it,
 * including the destroy task of the probe source released by the previous
 * probe. */
static void duration_destroy_marker(void *param)
{
	UNUSED_PARAMETER(param);
}

/* Waits until the previously released probe source is destroyed, so that only
 * one decoder is ever open. The destroy task queue is FIFO, so a marker queued
 * now runs after that destroy task. Unlike obs_wait_for_destroy_queue() this
 * does not queue and wait for tasks on the graphics and audio threads, so a
 * caller that OBS is itself waiting for (the frontend exit handler) cannot
 * deadlock here, and it leaves no callback of this module registered once it
 * returns. */
static void duration_wait_for_probe_destroy(void)
{
	obs_queue_task(OBS_TASK_DESTROY, duration_destroy_marker, NULL, true);
}

static enum mps_duration_probe_result duration_probe(void *opaque, const char *path, struct mps_duration_cancel *cancel,
						     int64_t *media_ms)
{
	enum mps_duration_probe_result result = MPS_DURATION_PROBE_UNKNOWN;
	obs_source_t *source;
	uint64_t deadline;

	UNUSED_PARAMETER(opaque);

	if (mps_duration_cancel_requested(cancel))
		return MPS_DURATION_PROBE_UNKNOWN;

	if (!obs_source_get_display_name("ffmpeg_source"))
		return MPS_DURATION_PROBE_UNSUPPORTED;

	source = duration_create_probe_source(path);
	if (!source)
		return MPS_DURATION_PROBE_UNSUPPORTED;

	deadline = duration_now() + PROBE_TIMEOUT_NS;
	for (;;) {
		int64_t duration_ms = obs_source_media_get_duration(source);

		if (duration_ms > 0) {
			*media_ms = duration_ms;
			result = MPS_DURATION_PROBE_DURATION;
			break;
		}
		/* ffmpeg_source reports ENDED when its media thread failed to open
		 * the file. A probe source is never started, so playback cannot be
		 * the reason for it. */
		if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_ENDED) {
			result = MPS_DURATION_PROBE_UNSUPPORTED;
			break;
		}
		if (duration_now() >= deadline || mps_duration_cancel_requested(cancel))
			break;
		mps_duration_cancel_wait(cancel, PROBE_POLL_MS);
	}

	obs_source_release(source);
	/* The decoder of a probe source is destroyed on the OBS destruction task
	 * thread; wait for it before the next probe creates another one. */
	duration_wait_for_probe_destroy();
	return result;
}

static const struct mps_duration_probe_ops duration_default_probe = {
	.identify = duration_identify,
	.probe = duration_probe,
	.opaque = NULL,
};

static size_t duration_entry_search(const struct mps_duration_entry *entries, size_t count, const char *path);

/* Inventory mutex held. Entries are kept sorted by path. */
static struct mps_duration_entry *duration_entry_find_locked(struct mps_duration_inventory *inventory, const char *path)
{
	size_t index = duration_entry_search(inventory->entries.array, inventory->entries.num, path);

	return index == DARRAY_INVALID ? NULL : &inventory->entries.array[index];
}

static bool duration_entry_due_locked(const struct mps_duration_inventory *inventory)
{
	uint64_t now = duration_now();

	for (size_t i = 0; i < inventory->entries.num; i++) {
		if (inventory->entries.array[i].next_check_ns <= now)
			return true;
	}
	return false;
}

static void duration_inventory_free(struct mps_duration_inventory *inventory)
{
	for (size_t i = 0; i < inventory->entries.num; i++)
		duration_entry_free(&inventory->entries.array[i]);
	da_free(inventory->entries);
	pthread_mutex_destroy(&inventory->mutex);
	bfree(inventory);
}

/* Worker mutex held. Returns true when the caller must free the inventory. */
static bool duration_inventory_release_locked(struct mps_duration_inventory *inventory)
{
	if (--inventory->references == 0)
		return true;
	return false;
}

static int duration_entry_compare(const void *left, const void *right)
{
	const struct mps_duration_entry *first = left;
	const struct mps_duration_entry *second = right;

	return strcmp(first->path, second->path);
}

static size_t duration_entry_search(const struct mps_duration_entry *entries, size_t count, const char *path)
{
	size_t low = 0;
	size_t high = count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;
		int order = strcmp(entries[middle].path, path);

		if (order == 0)
			return middle;
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return DARRAY_INVALID;
}

/* Finds the entry of an already sorted array that this path may reuse. Paths
 * are unique, so at most one entry matches. */
static size_t duration_entry_claim(const struct mps_duration_entry *entries, size_t count, const char *path,
				   const unsigned char *claimed)
{
	size_t index = duration_entry_search(entries, count, path);

	if (index == DARRAY_INVALID || claimed[index])
		return DARRAY_INVALID;
	return index;
}

struct mps_duration_inventory *mps_duration_inventory_create(void)
{
	struct mps_duration_inventory *inventory = bzalloc(sizeof(*inventory));

	if (!inventory)
		return NULL;

	pthread_mutex_init(&inventory->mutex, NULL);
	inventory->probe = duration_default_probe;
	inventory->references = 1;

	pthread_mutex_lock(&duration_worker.mutex);
	inventory->next = duration_worker.inventories;
	duration_worker.inventories = inventory;
	inventory->linked = true;
	pthread_mutex_unlock(&duration_worker.mutex);

	return inventory;
}

void mps_duration_inventory_destroy(struct mps_duration_inventory *inventory)
{
	struct mps_duration_inventory **link;
	bool free_inventory = false;

	if (!inventory)
		return;

	pthread_mutex_lock(&duration_worker.mutex);
	if (!inventory->linked) {
		pthread_mutex_unlock(&duration_worker.mutex);
		return;
	}
	inventory->linked = false;

	for (link = &duration_worker.inventories; *link; link = &(*link)->next) {
		if (*link == inventory) {
			*link = inventory->next;
			break;
		}
	}

	pthread_mutex_lock(&inventory->mutex);
	inventory->destroying = true;
	duration_worker.entry_total -= inventory->entries.num;
	duration_cancel_request(inventory->active_cancel);
	pthread_mutex_unlock(&inventory->mutex);

	free_inventory = duration_inventory_release_locked(inventory);
	pthread_mutex_unlock(&duration_worker.mutex);

	if (free_inventory)
		duration_inventory_free(inventory);
}

void mps_duration_inventory_set_probe(struct mps_duration_inventory *inventory,
				      const struct mps_duration_probe_ops *ops)
{
	if (!inventory)
		return;

	pthread_mutex_lock(&inventory->mutex);
	inventory->probe = ops ? *ops : duration_default_probe;
	pthread_mutex_unlock(&inventory->mutex);
}

struct duration_path_ref {
	const char *path;
	size_t index;
};

static int duration_path_ref_compare(const void *left, const void *right)
{
	const struct duration_path_ref *first = left;
	const struct duration_path_ref *second = right;

	return strcmp(first->path, second->path);
}

enum duration_path_state {
	/* The path can be admitted as a new entry. */
	DURATION_PATH_ADMIT = 0,
	/* A later occurrence of an already requested path. */
	DURATION_PATH_DUPLICATE,
	/* The entry of this path was reused. */
	DURATION_PATH_REUSED,
	/* NULL, empty or longer than MPS_DURATION_INVENTORY_MAX_PATH_BYTES. */
	DURATION_PATH_REJECTED,
};

/* True when the path is non-empty and shorter than
 * MPS_DURATION_INVENTORY_MAX_PATH_BYTES. memchr stops at the terminator, so a
 * path that does not fit the cache is never read as a whole, compared or
 * copied; such a path stays unknown. */
static bool duration_path_trackable(const char *path)
{
	const char *end;

	if (!path)
		return false;

	end = memchr(path, '\0', MPS_DURATION_INVENTORY_MAX_PATH_BYTES);
	return end != NULL && end != path;
}

/* Rejects untrackable paths and duplicate paths, which would otherwise shadow
 * each other, so only the first occurrence of a path is tracked. Returns NULL
 * when the scratch allocation failed. */
static unsigned char *duration_classify_paths(const char *const *paths, size_t limit)
{
	struct duration_path_ref *refs = bzalloc(limit * sizeof(*refs));
	unsigned char *state = bzalloc(limit);
	size_t count = 0;

	if (!refs || !state) {
		bfree(refs);
		bfree(state);
		return NULL;
	}

	for (size_t i = 0; i < limit; i++) {
		if (!duration_path_trackable(paths[i])) {
			state[i] = DURATION_PATH_REJECTED;
			continue;
		}
		refs[count].path = paths[i];
		refs[count].index = i;
		++count;
	}

	if (count > 1)
		qsort(refs, count, sizeof(*refs), duration_path_ref_compare);
	for (size_t i = 1; i < count; i++) {
		if (strcmp(refs[i - 1].path, refs[i].path) == 0)
			state[refs[i].index] = DURATION_PATH_DUPLICATE;
	}

	bfree(refs);
	return state;
}

void mps_duration_inventory_set_paths(struct mps_duration_inventory *inventory, const char *const *paths, size_t count)
{
	DARRAY(struct mps_duration_entry) previous;
	unsigned char *state = NULL;
	unsigned char *claimed = NULL;
	size_t limit;
	size_t budget;
	size_t dropped = 0;

	if (!inventory)
		return;

	if (!paths)
		count = 0;

	limit = count < MPS_DURATION_INVENTORY_MAX_ENTRIES ? count : MPS_DURATION_INVENTORY_MAX_ENTRIES;
	if (limit) {
		state = duration_classify_paths(paths, limit);
		if (!state)
			return;
	}

	da_init(previous);

	pthread_mutex_lock(&duration_worker.mutex);
	pthread_mutex_lock(&inventory->mutex);

	da_move(previous, inventory->entries);
	inventory->generation++;
	inventory->scan_cursor = 0;
	duration_cancel_request(inventory->active_cancel);

	/* Entries are kept sorted by path for binary search. */
	if (previous.num > 1)
		qsort(previous.array, previous.num, sizeof(*previous.array), duration_entry_compare);
	if (previous.num)
		claimed = bzalloc(previous.num);

	/* Keep the cached entries of paths that are still listed. */
	for (size_t i = 0; i < limit; i++) {
		struct mps_duration_entry entry;
		size_t index;

		if (state[i] != DURATION_PATH_ADMIT || !claimed)
			continue;

		index = duration_entry_claim(previous.array, previous.num, paths[i], claimed);
		if (index == DARRAY_INVALID)
			continue;

		entry = previous.array[index];
		entry.next_check_ns = 0;
		claimed[index] = 1;
		state[i] = DURATION_PATH_REUSED;
		da_push_back(inventory->entries, &entry);
	}

	/* Release the obsolete entries before admitting replacements, so a full
	 * global budget can be reused by the same update. */
	for (size_t i = 0; i < previous.num; i++) {
		if (claimed && claimed[i])
			continue;
		duration_entry_free(&previous.array[i]);
		++dropped;
	}
	duration_worker.entry_total -= dropped;

	da_free(previous);
	bfree(claimed);

	budget = duration_worker.entry_total < MPS_DURATION_INVENTORY_GLOBAL_MAX_ENTRIES
			 ? MPS_DURATION_INVENTORY_GLOBAL_MAX_ENTRIES - duration_worker.entry_total
			 : 0;

	for (size_t i = 0; i < limit; i++) {
		struct mps_duration_entry entry = {0};

		if (state[i] != DURATION_PATH_ADMIT || !budget)
			continue;

		entry.path = bstrdup(paths[i]);
		if (!entry.path)
			continue;

		--budget;
		++duration_worker.entry_total;
		da_push_back(inventory->entries, &entry);
	}

	bfree(state);

	if (inventory->entries.num > 1)
		qsort(inventory->entries.array, inventory->entries.num, sizeof(*inventory->entries.array),
		      duration_entry_compare);

	if (duration_worker.wakeup)
		os_event_signal(duration_worker.wakeup);
	pthread_cond_broadcast(&duration_worker.idle_cond);
	pthread_mutex_unlock(&inventory->mutex);
	pthread_mutex_unlock(&duration_worker.mutex);
}

bool mps_duration_inventory_get(struct mps_duration_inventory *inventory, const char *path, int64_t *media_ms)
{
	struct mps_duration_entry *entry;
	bool found = false;

	if (!inventory || !duration_path_trackable(path))
		return false;

	pthread_mutex_lock(&inventory->mutex);
	entry = duration_entry_find_locked(inventory, path);
	if (entry && entry->state == DURATION_ENTRY_KNOWN) {
		if (media_ms)
			*media_ms = entry->media_ms;
		found = true;
	}
	pthread_mutex_unlock(&inventory->mutex);

	return found;
}

/* Worker mutex and inventory mutex held. */
static bool duration_inventory_take_locked(struct mps_duration_inventory *inventory, struct mps_duration_job *job,
					   uint64_t now)
{
	size_t count = inventory->entries.num;

	for (size_t offset = 0; offset < count; offset++) {
		size_t index = (inventory->scan_cursor + offset) % count;
		struct mps_duration_entry *entry = &inventory->entries.array[index];

		if (entry->next_check_ns > now)
			continue;

		job->path = bstrdup(entry->path);
		if (!job->path) {
			entry->next_check_ns = now + RETRY_INTERVAL_NS;
			continue;
		}

		inventory->scan_cursor = (index + 1) % count;
		job->inventory = inventory;
		job->generation = inventory->generation;
		job->probe = inventory->probe;
		duration_cancel_init(&job->cancel);
		inventory->active_cancel = &job->cancel;
		++inventory->references;
		return true;
	}
	return false;
}

/* Worker mutex held. */
static uint64_t duration_worker_wait_ms_locked(uint64_t now)
{
	uint64_t next = 0;
	uint64_t wait;
	bool have_next = false;

	for (struct mps_duration_inventory *inventory = duration_worker.inventories; inventory;
	     inventory = inventory->next) {
		pthread_mutex_lock(&inventory->mutex);
		for (size_t i = 0; i < inventory->entries.num; i++) {
			uint64_t due = inventory->entries.array[i].next_check_ns;

			if (!have_next || due < next) {
				next = due;
				have_next = true;
			}
		}
		pthread_mutex_unlock(&inventory->mutex);
	}

	if (!have_next || next <= now)
		return 1;

	wait = (next - now + 999999) / 1000000;
	if (wait == 0)
		wait = 1;
	if (wait > WORKER_WAIT_MAX_MS)
		wait = WORKER_WAIT_MAX_MS;
	return wait;
}

/* Worker mutex held. */
static bool duration_worker_take_job_locked(struct mps_duration_job *job, uint64_t *wait_ms)
{
	struct mps_duration_inventory *inventory;
	uint64_t now = duration_now();
	size_t count = 0;
	size_t start;

	for (inventory = duration_worker.inventories; inventory; inventory = inventory->next)
		++count;

	if (count) {
		start = duration_worker.round_robin % count;
		++duration_worker.round_robin;

		inventory = duration_worker.inventories;
		for (size_t skip = 0; skip < start; skip++)
			inventory = inventory->next;

		for (size_t attempt = 0; attempt < count; attempt++) {
			bool taken;

			pthread_mutex_lock(&inventory->mutex);
			taken = duration_inventory_take_locked(inventory, job, now);
			pthread_mutex_unlock(&inventory->mutex);
			if (taken)
				return true;

			inventory = inventory->next ? inventory->next : duration_worker.inventories;
		}
	}

	*wait_ms = duration_worker_wait_ms_locked(now);
	return false;
}

/* Inventory mutex held. Returns the entry of this job, or NULL when the job was
 * replaced or its inventory is going away. */
static struct mps_duration_entry *duration_job_entry_locked(struct mps_duration_inventory *inventory,
							    const struct mps_duration_job *job)
{
	if (inventory->destroying || inventory->generation != job->generation)
		return NULL;
	return duration_entry_find_locked(inventory, job->path);
}

static bool duration_job_is_current(struct mps_duration_job *job)
{
	struct mps_duration_inventory *inventory = job->inventory;
	bool current;

	pthread_mutex_lock(&inventory->mutex);
	current = duration_job_entry_locked(inventory, job) != NULL;
	pthread_mutex_unlock(&inventory->mutex);
	return current;
}

/* Inventory mutex held. Publishes the job as finished. */
static void duration_job_finish_locked(struct mps_duration_job *job)
{
	job->inventory->active_cancel = NULL;
	duration_cancel_free(&job->cancel);
}

/* No inventory lock held. */
static void duration_run_job(struct mps_duration_job *job)
{
	struct mps_duration_inventory *inventory = job->inventory;
	struct mps_duration_file_id id = {0};
	struct mps_duration_entry *entry;
	enum mps_duration_probe_result result = MPS_DURATION_PROBE_UNKNOWN;
	int64_t media_ms = 0;
	bool identified;
	bool probed = false;
	bool notify = false;

	identified = job->probe.identify(job->probe.opaque, job->path, &id);
	if (identified && !duration_path_trackable(id.path)) {
		/* The canonical path can be longer than the requested one (symlink
		 * resolution, Windows working directory expansion) while the cache
		 * only stores bounded identities: never probe it and never cache it,
		 * the path stays unknown. */
		bfree(id.path);
		id = (struct mps_duration_file_id){0};
		identified = false;
	}

	pthread_mutex_lock(&inventory->mutex);
	entry = duration_job_entry_locked(inventory, job);
	if (!entry) {
		duration_job_finish_locked(job);
		pthread_mutex_unlock(&inventory->mutex);
		bfree(id.path);
		return;
	}
	if (identified && entry->state != DURATION_ENTRY_UNKNOWN && duration_id_matches(&entry->id, &id)) {
		/* The file did not change: keep the cached result. */
		entry->next_check_ns = duration_now() + RECHECK_INTERVAL_NS;
		duration_job_finish_locked(job);
		pthread_mutex_unlock(&inventory->mutex);
		bfree(id.path);
		return;
	}
	pthread_mutex_unlock(&inventory->mutex);

	/* Both callbacks run without the inventory lock, so the job can be
	 * cancelled or replaced meanwhile. Such a job must not start a probe. */
	if (!identified) {
		result = MPS_DURATION_PROBE_UNSUPPORTED;
	} else if (!mps_duration_cancel_requested(&job->cancel) && duration_job_is_current(job)) {
		const char *probe_path = id.path ? id.path : job->path;

		result = job->probe.probe(job->probe.opaque, probe_path, &job->cancel, &media_ms);
		probed = true;
	}

	pthread_mutex_lock(&inventory->mutex);
	entry = duration_job_entry_locked(inventory, job);
	if (entry && (probed || !identified) && !mps_duration_cancel_requested(&job->cancel)) {
		enum duration_entry_state state = DURATION_ENTRY_UNKNOWN;
		uint64_t recheck = RECHECK_INTERVAL_NS;

		if (result == MPS_DURATION_PROBE_DURATION && media_ms > 0)
			state = DURATION_ENTRY_KNOWN;
		else if (result == MPS_DURATION_PROBE_UNSUPPORTED)
			state = DURATION_ENTRY_INVALID;
		else
			recheck = RETRY_INTERVAL_NS;

		notify = entry->state != state || (state == DURATION_ENTRY_KNOWN && entry->media_ms != media_ms);
		entry->state = state;
		entry->media_ms = state == DURATION_ENTRY_KNOWN ? media_ms : 0;
		duration_entry_adopt_id(entry, &id);
		entry->next_check_ns = duration_now() + recheck;
	} else if (entry) {
		/* The job was cancelled: retry later and keep the cached result. */
		entry->next_check_ns = duration_now() + RETRY_INTERVAL_NS;
	}
	duration_job_finish_locked(job);
	pthread_mutex_unlock(&inventory->mutex);

	bfree(id.path);

	if (notify) {
		void (*changed)(void *) = duration_worker.changed;

		if (changed)
			changed(duration_worker.changed_opaque);
	}
}

/* Best effort: the worker must not compete with playback and rendering
 * threads, but a refused priority change must never prevent probing. */
static void duration_worker_lower_priority(void)
{
#if defined(_WIN32)
	(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
	/* Only this thread's QoS class changes; the process nice value is left
	 * alone. */
	(void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
	/* On Linux a thread is its own scheduling entity, so this raises the nice
	 * value of the worker thread only. */
	(void)setpriority(PRIO_PROCESS, 0, 10);
#endif
}

static void *duration_worker_run(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("mps_duration_worker");
	duration_worker_lower_priority();

	for (;;) {
		struct mps_duration_job job = {0};
		uint64_t wait_ms = 0;
		bool have_job;
		bool free_inventory;

		pthread_mutex_lock(&duration_worker.mutex);
		if (!duration_worker.running) {
			pthread_mutex_unlock(&duration_worker.mutex);
			break;
		}

		have_job = duration_worker_take_job_locked(&job, &wait_ms);
		if (!have_job) {
			os_event_reset(duration_worker.wakeup);
			pthread_cond_broadcast(&duration_worker.idle_cond);
		}
		pthread_mutex_unlock(&duration_worker.mutex);

		if (!have_job) {
			if (duration_worker.wakeup)
				os_event_timedwait(duration_worker.wakeup, (unsigned long)wait_ms);
			continue;
		}

		duration_run_job(&job);
		bfree(job.path);

		pthread_mutex_lock(&duration_worker.mutex);
		free_inventory = duration_inventory_release_locked(job.inventory);
		pthread_mutex_unlock(&duration_worker.mutex);
		if (free_inventory)
			duration_inventory_free(job.inventory);
	}

	return NULL;
}

void mps_duration_inventory_start(void (*changed)(void *), void *opaque)
{
	os_event_t *wakeup = NULL;
	pthread_t thread;

	pthread_mutex_lock(&duration_worker.mutex);
	duration_worker.changed = changed;
	duration_worker.changed_opaque = opaque;

	if (!duration_worker.running) {
		if (os_event_init(&wakeup, OS_EVENT_TYPE_MANUAL) != 0) {
			pthread_mutex_unlock(&duration_worker.mutex);
			return;
		}

		duration_worker.wakeup = wakeup;
		duration_worker.running = true;
		if (pthread_create(&thread, NULL, duration_worker_run, NULL) != 0) {
			duration_worker.running = false;
			duration_worker.wakeup = NULL;
			pthread_mutex_unlock(&duration_worker.mutex);
			os_event_destroy(wakeup);
			return;
		}
		duration_worker.thread = thread;
	}
	pthread_mutex_unlock(&duration_worker.mutex);
}

void mps_duration_inventory_stop(void)
{
	pthread_t thread;

	pthread_mutex_lock(&duration_worker.mutex);
	if (!duration_worker.running) {
		duration_worker.changed = NULL;
		duration_worker.changed_opaque = NULL;
		pthread_mutex_unlock(&duration_worker.mutex);
		return;
	}

	duration_worker.running = false;
	for (struct mps_duration_inventory *inventory = duration_worker.inventories; inventory;
	     inventory = inventory->next) {
		pthread_mutex_lock(&inventory->mutex);
		duration_cancel_request(inventory->active_cancel);
		pthread_mutex_unlock(&inventory->mutex);
	}

	thread = duration_worker.thread;
	pthread_cond_broadcast(&duration_worker.idle_cond);
	os_event_signal(duration_worker.wakeup);
	pthread_mutex_unlock(&duration_worker.mutex);

	pthread_join(thread, NULL);

	pthread_mutex_lock(&duration_worker.mutex);
	os_event_destroy(duration_worker.wakeup);
	duration_worker.wakeup = NULL;
	duration_worker.changed = NULL;
	duration_worker.changed_opaque = NULL;
	pthread_mutex_unlock(&duration_worker.mutex);
}

bool mps_duration_inventory_wait_idle(struct mps_duration_inventory *inventory, uint32_t timeout_ms)
{
	struct timespec deadline;
	bool idle = false;

	if (!inventory)
		return false;

	duration_realtime_deadline(&deadline, timeout_ms);

	pthread_mutex_lock(&duration_worker.mutex);
	for (;;) {
		bool busy;
		bool due;

		pthread_mutex_lock(&inventory->mutex);
		busy = inventory->active_cancel != NULL;
		due = duration_entry_due_locked(inventory);
		pthread_mutex_unlock(&inventory->mutex);

		if (!busy && !due) {
			idle = true;
			break;
		}
		if (!duration_worker.running) {
			/* Nothing will ever run: report the inventory as settled. */
			idle = true;
			break;
		}
		if (pthread_cond_timedwait(&duration_worker.idle_cond, &duration_worker.mutex, &deadline) != 0)
			break;
	}
	pthread_mutex_unlock(&duration_worker.mutex);

	return idle;
}
