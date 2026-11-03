#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <util/bmem.h>
#include <util/threading.h>

#include "duration-inventory.h"

#define CHECK(condition)                                                                                \
	do {                                                                                            \
		if (!(condition)) {                                                                     \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE);                                                             \
		}                                                                                       \
	} while (false)

#define WAIT_TIMEOUT_MS 10000
#define BLOCKED_PROBE_CEILING 500

/* identify() reports this prefix so a probe that receives the raw playlist key
 * instead of the identified path is detected. */
#define FAKE_ABS_PREFIX "abs:"

struct fake_media {
	char path[128];
	int64_t media_ms;
	int64_t size;
	int64_t mtime_nsec;
	bool blocks;
	bool blocks_identify;
	int calls;
};

/* Injected probe. Every probe blocks on the cancellation handle instead of the
 * file system, so tests never depend on timing. */
static struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct fake_media media[8];
	size_t media_count;
	bool default_identify;
	int64_t default_media_ms;
	int64_t default_size;
	int probe_calls;
	int identify_calls;
	int change_calls;
	int blocked_calls;
	int cancelled_calls;
	int identify_blocked_calls;
	int probe_path_mismatch;
	bool release_identify;
	bool identify_returns_raw;
} fake;

static void test_deadline(struct timespec *deadline, uint32_t timeout_ms)
{
	timespec_get(deadline, TIME_UTC);
	deadline->tv_sec += (time_t)(timeout_ms / 1000);
	deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec += 1;
		deadline->tv_nsec -= 1000000000L;
	}
}

static void fake_reset(void)
{
	pthread_mutex_lock(&fake.mutex);
	memset(fake.media, 0, sizeof(fake.media));
	fake.media_count = 0;
	fake.default_identify = false;
	fake.default_media_ms = 0;
	fake.default_size = 0;
	fake.probe_calls = 0;
	fake.identify_calls = 0;
	fake.change_calls = 0;
	fake.blocked_calls = 0;
	fake.cancelled_calls = 0;
	fake.identify_blocked_calls = 0;
	fake.probe_path_mismatch = 0;
	fake.release_identify = false;
	fake.identify_returns_raw = false;
	pthread_cond_broadcast(&fake.cond);
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_add(const char *path, int64_t media_ms, int64_t size, bool blocks)
{
	struct fake_media *media;

	pthread_mutex_lock(&fake.mutex);
	CHECK(fake.media_count < sizeof(fake.media) / sizeof(*fake.media));
	media = &fake.media[fake.media_count++];
	snprintf(media->path, sizeof(media->path), "%s", path);
	media->media_ms = media_ms;
	media->size = size;
	media->blocks = blocks;
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_set_media(const char *path, int64_t media_ms, int64_t size)
{
	pthread_mutex_lock(&fake.mutex);
	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, path) != 0)
			continue;
		fake.media[i].media_ms = media_ms;
		fake.media[i].size = size;
	}
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_set_media_nsec(const char *path, int64_t mtime_nsec)
{
	pthread_mutex_lock(&fake.mutex);
	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, path) == 0)
			fake.media[i].mtime_nsec = mtime_nsec;
	}
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_set_identify_raw(bool raw)
{
	pthread_mutex_lock(&fake.mutex);
	fake.identify_returns_raw = raw;
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_set_identify_block(const char *path, bool blocks)
{
	pthread_mutex_lock(&fake.mutex);
	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, path) == 0)
			fake.media[i].blocks_identify = blocks;
	}
	pthread_mutex_unlock(&fake.mutex);
}

static void fake_release_identify(void)
{
	pthread_mutex_lock(&fake.mutex);
	fake.release_identify = true;
	pthread_cond_broadcast(&fake.cond);
	pthread_mutex_unlock(&fake.mutex);
}

static int fake_calls(const char *path)
{
	int calls = 0;

	pthread_mutex_lock(&fake.mutex);
	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, path) == 0) {
			calls = fake.media[i].calls;
			break;
		}
	}
	pthread_mutex_unlock(&fake.mutex);
	return calls;
}

static char *fake_absolute_path(const char *path)
{
	size_t length = strlen(FAKE_ABS_PREFIX) + strlen(path) + 1;
	char *absolute = bmalloc(length);

	if (absolute)
		snprintf(absolute, length, "%s%s", FAKE_ABS_PREFIX, path);
	return absolute;
}

static int fake_value(int *counter)
{
	int value;

	pthread_mutex_lock(&fake.mutex);
	value = *counter;
	pthread_mutex_unlock(&fake.mutex);
	return value;
}

static bool fake_wait(int *counter, int minimum)
{
	struct timespec deadline;
	bool reached;

	test_deadline(&deadline, WAIT_TIMEOUT_MS);

	pthread_mutex_lock(&fake.mutex);
	while (*counter < minimum) {
		if (pthread_cond_timedwait(&fake.cond, &fake.mutex, &deadline) != 0)
			break;
	}
	reached = *counter >= minimum;
	pthread_mutex_unlock(&fake.mutex);
	return reached;
}

static bool fake_identify(void *opaque, const char *path, struct mps_duration_file_id *id)
{
	struct timespec deadline;
	struct fake_media *media = NULL;
	bool found = false;
	bool blocks = false;

	UNUSED_PARAMETER(opaque);
	*id = (struct mps_duration_file_id){0};

	test_deadline(&deadline, WAIT_TIMEOUT_MS);

	pthread_mutex_lock(&fake.mutex);
	++fake.identify_calls;
	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, path) == 0) {
			media = &fake.media[i];
			blocks = media->blocks_identify;
			break;
		}
	}

	if (blocks) {
		++fake.identify_blocked_calls;
		pthread_cond_broadcast(&fake.cond);
		while (!fake.release_identify) {
			if (pthread_cond_timedwait(&fake.cond, &fake.mutex, &deadline) != 0)
				break;
		}
	}

	if (media && media->size > 0) {
		id->path = fake.identify_returns_raw ? bstrdup(path) : fake_absolute_path(path);
		id->size = media->size;
		id->mtime_sec = media->size;
		id->mtime_nsec = media->mtime_nsec;
		id->valid = id->path != NULL;
		found = id->valid;
	} else if (!media && fake.default_identify && fake.default_size > 0) {
		id->path = fake.identify_returns_raw ? bstrdup(path) : fake_absolute_path(path);
		id->size = fake.default_size;
		id->mtime_sec = fake.default_size;
		id->valid = id->path != NULL;
		found = id->valid;
	}
	pthread_mutex_unlock(&fake.mutex);

	return found;
}

static enum mps_duration_probe_result fake_probe(void *opaque, const char *path, struct mps_duration_cancel *cancel,
						 int64_t *media_ms)
{
	enum mps_duration_probe_result result;
	const char *key = path;
	size_t prefix_length = strlen(FAKE_ABS_PREFIX);
	int64_t media_duration = 0;
	bool blocks = false;

	UNUSED_PARAMETER(opaque);

	pthread_mutex_lock(&fake.mutex);
	++fake.probe_calls;
	if (strncmp(path, FAKE_ABS_PREFIX, prefix_length) == 0)
		key = path + prefix_length;
	else
		++fake.probe_path_mismatch;

	for (size_t i = 0; i < fake.media_count; i++) {
		if (strcmp(fake.media[i].path, key) != 0)
			continue;
		++fake.media[i].calls;
		media_duration = fake.media[i].media_ms;
		blocks = fake.media[i].blocks;
		break;
	}
	if (media_duration == 0 && fake.default_identify)
		media_duration = fake.default_media_ms;
	if (blocks)
		++fake.blocked_calls;
	pthread_cond_broadcast(&fake.cond);
	pthread_mutex_unlock(&fake.mutex);

	if (blocks) {
		for (int attempt = 0; attempt < BLOCKED_PROBE_CEILING; attempt++) {
			if (!mps_duration_cancel_wait(cancel, 20))
				continue;

			pthread_mutex_lock(&fake.mutex);
			++fake.cancelled_calls;
			pthread_cond_broadcast(&fake.cond);
			pthread_mutex_unlock(&fake.mutex);
			return MPS_DURATION_PROBE_UNKNOWN;
		}
		return MPS_DURATION_PROBE_UNKNOWN;
	}

	if (media_duration > 0) {
		*media_ms = media_duration;
		result = MPS_DURATION_PROBE_DURATION;
	} else {
		result = MPS_DURATION_PROBE_UNSUPPORTED;
	}
	return result;
}

static void test_changed(void *opaque)
{
	UNUSED_PARAMETER(opaque);

	pthread_mutex_lock(&fake.mutex);
	++fake.change_calls;
	pthread_cond_broadcast(&fake.cond);
	pthread_mutex_unlock(&fake.mutex);
}

static const struct mps_duration_probe_ops fake_ops = {
	.identify = fake_identify,
	.probe = fake_probe,
	.opaque = NULL,
};

static struct mps_duration_inventory *test_inventory(void)
{
	struct mps_duration_inventory *inventory = mps_duration_inventory_create();

	CHECK(inventory != NULL);
	mps_duration_inventory_set_probe(inventory, &fake_ops);
	return inventory;
}

static void test_idle(struct mps_duration_inventory *inventory)
{
	CHECK(mps_duration_inventory_wait_idle(inventory, WAIT_TIMEOUT_MS));
}

static void test_null_arguments(void)
{
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();

	mps_duration_inventory_destroy(NULL);
	mps_duration_inventory_set_probe(NULL, &fake_ops);
	mps_duration_inventory_set_paths(NULL, NULL, 0);
	CHECK(!mps_duration_inventory_get(NULL, "/media/one.mp4", &media_ms));
	CHECK(!mps_duration_inventory_get(NULL, NULL, &media_ms));
	CHECK(!mps_duration_inventory_wait_idle(NULL, 0));

	inventory = test_inventory();
	mps_duration_inventory_set_paths(inventory, NULL, 0);
	CHECK(mps_duration_inventory_wait_idle(inventory, 0));
	CHECK(!mps_duration_inventory_get(inventory, NULL, &media_ms));
	CHECK(!mps_duration_inventory_get(inventory, "/media/one.mp4", NULL));

	mps_duration_inventory_destroy(inventory);
}

static void test_cache_and_reuse(void)
{
	const char *paths[] = {"/media/one.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 1500, 1, false);
	inventory = test_inventory();

	/* Cache-only lookup: nothing is probed before the worker ran. */
	CHECK(!mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(fake_value(&fake.probe_calls) == 0);

	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1500);
	CHECK(fake_value(&fake.probe_calls) == 1);
	/* The probe receives the identified absolute path, not the playlist key. */
	CHECK(fake_value(&fake.probe_path_mismatch) == 0);

	/* A playlist update that still lists the path reuses the cached duration. */
	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(fake_value(&fake.change_calls) == 1);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1500);

	mps_duration_inventory_destroy(inventory);
}

static void test_file_change_invalidates(void)
{
	const char *paths[] = {"/media/two.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 1000, 10, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1000);

	/* Same identity: the duration stays cached even if the media changed
	 * without the file identity changing. */
	fake_set_media(paths[0], 2000, 10);
	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1000);

	/* New identity: the path is probed again and reports the new duration. */
	fake_set_media(paths[0], 2000, 11);
	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 2));
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 2);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 2000);

	mps_duration_inventory_destroy(inventory);
}

static void test_mtime_nsec_invalidates(void)
{
	const char *paths[] = {"/media/nsec.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 1000, 10, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1000);

	/* Same size and same second: only the sub-second part of the modification
	 * time can reveal this rewrite, so the cached duration is kept. */
	fake_set_media(paths[0], 1500, 10);
	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1000);

	/* A different nanosecond part invalidates the cached duration. */
	fake_set_media_nsec(paths[0], 5);
	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 2));
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 2);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1500);

	mps_duration_inventory_destroy(inventory);
}

static void test_non_local_paths(void)
{
	const char *paths[] = {"https://example.com/stream.m3u8", "/media/missing.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 5000, 0, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, paths, 2);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 0);
	CHECK(!mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(!mps_duration_inventory_get(inventory, paths[1], &media_ms));

	/* Unreadable paths are rechecked without being probed. */
	mps_duration_inventory_set_paths(inventory, paths, 2);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 0);

	mps_duration_inventory_destroy(inventory);
}

static void test_failed_probe_is_sticky(void)
{
	const char *paths[] = {"/media/corrupt.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 0, 10, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(!mps_duration_inventory_get(inventory, paths[0], &media_ms));

	/* The file did not change: no repeated probe. */
	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 1);

	/* Replacing the file makes it probed again. */
	fake_set_media(paths[0], 900, 12);
	mps_duration_inventory_set_paths(inventory, paths, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(fake_value(&fake.probe_calls) == 2);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 900);

	mps_duration_inventory_destroy(inventory);
}

static void test_cancelled_generation(void)
{
	const char *slow[] = {"/media/slow.mp4"};
	const char *fast[] = {"/media/fast.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(slow[0], 4000, 10, true);
	fake_add(fast[0], 2500, 10, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, slow, 1);
	CHECK(fake_wait(&fake.blocked_calls, 1));

	/* Replacing the path set cancels the probe of the previous generation. */
	mps_duration_inventory_set_paths(inventory, fast, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);

	CHECK(fake_value(&fake.cancelled_calls) == 1);
	CHECK(fake_value(&fake.probe_calls) == 2);
	CHECK(!mps_duration_inventory_get(inventory, slow[0], &media_ms));
	CHECK(mps_duration_inventory_get(inventory, fast[0], &media_ms));
	CHECK(media_ms == 2500);

	mps_duration_inventory_destroy(inventory);
}

static void test_destroy_during_probe(void)
{
	const char *slow[] = {"/media/slow.mp4"};
	const char *after[] = {"/media/after.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(slow[0], 4000, 10, true);
	fake_add(after[0], 700, 10, false);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, slow, 1);
	CHECK(fake_wait(&fake.blocked_calls, 1));

	/* Destroying must neither wait for the worker nor use freed state. */
	mps_duration_inventory_destroy(inventory);
	CHECK(fake_wait(&fake.cancelled_calls, 1));

	inventory = test_inventory();
	mps_duration_inventory_set_paths(inventory, after, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(mps_duration_inventory_get(inventory, after[0], &media_ms));
	CHECK(media_ms == 700);

	mps_duration_inventory_destroy(inventory);
}

static void test_entry_bounds(void)
{
	size_t const total = MPS_DURATION_INVENTORY_MAX_ENTRIES + 16;
	size_t const stride = 32;
	struct mps_duration_inventory *inventory;
	char *storage = calloc(total, stride);
	const char **paths = calloc(total, sizeof(*paths));
	int64_t media_ms = 0;

	CHECK(storage != NULL);
	CHECK(paths != NULL);

	fake_reset();
	pthread_mutex_lock(&fake.mutex);
	fake.default_identify = true;
	fake.default_media_ms = 100;
	fake.default_size = 1;
	pthread_mutex_unlock(&fake.mutex);

	for (size_t i = 0; i < total; i++) {
		paths[i] = storage + i * stride;
		snprintf(storage + i * stride, stride, "/bounds/%zu.mp4", i);
	}

	inventory = test_inventory();
	mps_duration_inventory_set_paths(inventory, paths, total);
	test_idle(inventory);

	CHECK(fake_value(&fake.probe_calls) == (int)MPS_DURATION_INVENTORY_MAX_ENTRIES);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 100);
	CHECK(mps_duration_inventory_get(inventory, paths[MPS_DURATION_INVENTORY_MAX_ENTRIES - 1], &media_ms));
	CHECK(!mps_duration_inventory_get(inventory, paths[MPS_DURATION_INVENTORY_MAX_ENTRIES], &media_ms));

	mps_duration_inventory_destroy(inventory);
	free(paths);
	free(storage);
}

static char *bounds_paths(size_t total, size_t stride, const char *prefix, const char ***paths_out)
{
	char *storage = calloc(total, stride);
	const char **paths = calloc(total, sizeof(*paths));

	CHECK(storage != NULL);
	CHECK(paths != NULL);
	for (size_t i = 0; i < total; i++) {
		paths[i] = storage + i * stride;
		snprintf(storage + i * stride, stride, "/%s/%zu.mp4", prefix, i);
	}
	*paths_out = paths;
	return storage;
}

static void test_global_entry_bounds(void)
{
	size_t const stride = 32;
	size_t const total = MPS_DURATION_INVENTORY_MAX_ENTRIES;
	struct mps_duration_inventory *first;
	struct mps_duration_inventory *second;
	struct mps_duration_inventory *third;
	const char **first_paths;
	const char **second_paths;
	const char **third_paths;
	char *first_storage;
	char *second_storage;
	char *third_storage;
	int64_t media_ms = 0;

	fake_reset();
	pthread_mutex_lock(&fake.mutex);
	fake.default_identify = true;
	fake.default_media_ms = 100;
	fake.default_size = 1;
	pthread_mutex_unlock(&fake.mutex);

	first_storage = bounds_paths(total, stride, "first", &first_paths);
	second_storage = bounds_paths(total, stride, "second", &second_paths);
	third_storage = bounds_paths(1, stride, "third", &third_paths);

	first = test_inventory();
	mps_duration_inventory_set_paths(first, first_paths, total);
	test_idle(first);

	second = test_inventory();
	mps_duration_inventory_set_paths(second, second_paths, total);
	test_idle(second);

	/* The global budget is exhausted: the third inventory tracks nothing. */
	third = test_inventory();
	mps_duration_inventory_set_paths(third, third_paths, 1);
	test_idle(third);
	CHECK(!mps_duration_inventory_get(third, third_paths[0], &media_ms));

	/* Destroying an inventory returns its entries to the global budget. */
	mps_duration_inventory_destroy(first);
	mps_duration_inventory_set_paths(third, third_paths, 1);
	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(third);
	CHECK(mps_duration_inventory_get(third, third_paths[0], &media_ms));
	CHECK(media_ms == 100);

	mps_duration_inventory_destroy(second);
	mps_duration_inventory_destroy(third);

	free(first_paths);
	free(first_storage);
	free(second_paths);
	free(second_storage);
	free(third_paths);
	free(third_storage);
}

static void test_duplicate_paths_settle(void)
{
	const char *paths[] = {"/media/duplicate.mp4", "/media/duplicate.mp4", "/media/duplicate.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(paths[0], 1200, 10, false);
	inventory = test_inventory();

	/* A duplicated path used to shadow itself: the second entry never settled
	 * and stayed due forever. */
	mps_duration_inventory_set_paths(inventory, paths, 3);
	CHECK(fake_wait(&fake.change_calls, 1));
	CHECK(mps_duration_inventory_wait_idle(inventory, 2000));
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(mps_duration_inventory_get(inventory, paths[0], &media_ms));
	CHECK(media_ms == 1200);

	mps_duration_inventory_set_paths(inventory, paths, 3);
	CHECK(mps_duration_inventory_wait_idle(inventory, 2000));
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(fake_value(&fake.change_calls) == 1);

	mps_duration_inventory_destroy(inventory);
}

static void test_cancelled_before_probe(void)
{
	const char *slow[] = {"/media/slow-identify.mp4"};
	const char *fast[] = {"/media/fast.mp4"};
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	fake_reset();
	fake_add(slow[0], 4000, 10, false);
	fake_add(fast[0], 2200, 10, false);
	fake_set_identify_block(slow[0], true);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, slow, 1);
	CHECK(fake_wait(&fake.identify_blocked_calls, 1));

	/* Replace the path set while identification of the old path still runs. */
	mps_duration_inventory_set_paths(inventory, fast, 1);
	fake_release_identify();

	CHECK(fake_wait(&fake.change_calls, 1));
	test_idle(inventory);
	CHECK(fake_calls(slow[0]) == 0);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(!mps_duration_inventory_get(inventory, slow[0], &media_ms));
	CHECK(mps_duration_inventory_get(inventory, fast[0], &media_ms));
	CHECK(media_ms == 2200);

	mps_duration_inventory_destroy(inventory);
}

static void test_global_budget_replacement(void)
{
	size_t const stride = 32;
	size_t const total = MPS_DURATION_INVENTORY_MAX_ENTRIES;
	struct mps_duration_inventory *first;
	struct mps_duration_inventory *second;
	const char **first_paths;
	const char **second_paths;
	const char **replacement_paths;
	char *first_storage;
	char *second_storage;
	char *replacement_storage;
	int64_t media_ms = 0;

	fake_reset();
	pthread_mutex_lock(&fake.mutex);
	fake.default_identify = true;
	fake.default_media_ms = 100;
	fake.default_size = 1;
	pthread_mutex_unlock(&fake.mutex);

	first_storage = bounds_paths(total, stride, "fill", &first_paths);
	second_storage = bounds_paths(total, stride, "replaceable", &second_paths);
	replacement_storage = bounds_paths(total, stride, "replacement", &replacement_paths);

	first = test_inventory();
	mps_duration_inventory_set_paths(first, first_paths, total);
	test_idle(first);

	second = test_inventory();
	mps_duration_inventory_set_paths(second, second_paths, total);
	test_idle(second);

	/* The global budget is full. Replacing every path of the second inventory
	 * must reuse the slots its own obsolete entries release. */
	mps_duration_inventory_set_paths(second, replacement_paths, total);
	test_idle(second);
	CHECK(mps_duration_inventory_get(second, replacement_paths[0], &media_ms));
	CHECK(media_ms == 100);
	CHECK(mps_duration_inventory_get(second, replacement_paths[total - 1], &media_ms));
	CHECK(!mps_duration_inventory_get(second, second_paths[0], &media_ms));

	mps_duration_inventory_destroy(first);
	mps_duration_inventory_destroy(second);

	free(first_paths);
	free(first_storage);
	free(second_paths);
	free(second_storage);
	free(replacement_paths);
	free(replacement_storage);
}

static void test_path_length_bounds(void)
{
	size_t const cap = MPS_DURATION_INVENTORY_MAX_PATH_BYTES;
	/* Exactly the cap, without a terminator: a bounded check must not read
	 * past these bytes, and the path stays unknown and unprobed. */
	char *overlong = malloc(cap);
	/* One byte shorter than the cap including its terminator: accepted. */
	char *boundary = malloc(cap);
	const char *invalid[] = {NULL, ""};
	const char *paths[2];
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	CHECK(overlong != NULL);
	CHECK(boundary != NULL);
	memset(overlong, 'a', cap);
	memset(boundary, 'a', cap);
	boundary[cap - 1] = '\0';

	fake_reset();
	pthread_mutex_lock(&fake.mutex);
	fake.default_identify = true;
	fake.default_media_ms = 100;
	fake.default_size = 1;
	pthread_mutex_unlock(&fake.mutex);

	inventory = test_inventory();
	/* The identified path of this test must not grow past the cap itself. */
	fake_set_identify_raw(true);

	/* NULL and empty paths are ignored without being probed. */
	mps_duration_inventory_set_paths(inventory, invalid, 2);
	test_idle(inventory);
	CHECK(fake_value(&fake.identify_calls) == 0);
	CHECK(fake_value(&fake.probe_calls) == 0);
	CHECK(!mps_duration_inventory_get(inventory, NULL, &media_ms));
	CHECK(!mps_duration_inventory_get(inventory, "", &media_ms));

	/* The over-long path is rejected before it is copied or compared. */
	paths[0] = overlong;
	paths[1] = boundary;
	mps_duration_inventory_set_paths(inventory, paths, 2);
	test_idle(inventory);
	CHECK(fake_value(&fake.identify_calls) == 1);
	CHECK(fake_value(&fake.probe_calls) == 1);
	CHECK(!mps_duration_inventory_get(inventory, overlong, &media_ms));
	CHECK(mps_duration_inventory_get(inventory, boundary, &media_ms));
	CHECK(media_ms == 100);

	/* A query that does not fit the cache is answered without a scan. */
	CHECK(!mps_duration_inventory_get(inventory, overlong, NULL));

	mps_duration_inventory_destroy(inventory);
	free(overlong);
	free(boundary);
}

static void test_overlong_canonical_rejected(void)
{
	size_t const cap = MPS_DURATION_INVENTORY_MAX_PATH_BYTES;
	size_t const prefix = strlen(FAKE_ABS_PREFIX);
	/* The requested path fits the cache, the identified canonical form (prefix
	 * plus path) is exactly one byte too long for it. */
	size_t const requested_length = cap - prefix;
	char *requested = malloc(requested_length + 1);
	const char *paths[1];
	struct mps_duration_inventory *inventory;
	int64_t media_ms = 0;

	CHECK(requested != NULL);
	memset(requested, 'a', requested_length);
	requested[requested_length] = '\0';

	fake_reset();
	pthread_mutex_lock(&fake.mutex);
	fake.default_identify = true;
	fake.default_media_ms = 100;
	fake.default_size = 1;
	pthread_mutex_unlock(&fake.mutex);

	inventory = test_inventory();
	paths[0] = requested;
	mps_duration_inventory_set_paths(inventory, paths, 1);
	test_idle(inventory);

	/* Identification ran, but the identity is not representable within the
	 * cache bound: no probe source and no cached identity. */
	CHECK(fake_value(&fake.identify_calls) == 1);
	CHECK(fake_value(&fake.probe_calls) == 0);
	CHECK(!mps_duration_inventory_get(inventory, requested, &media_ms));

	mps_duration_inventory_destroy(inventory);
	free(requested);
}

static void test_stop_and_restart(void)
{
	const char *slow[] = {"/media/slow.mp4"};
	struct mps_duration_inventory *inventory;

	fake_reset();
	fake_add(slow[0], 4000, 10, true);
	inventory = test_inventory();

	mps_duration_inventory_set_paths(inventory, slow, 1);
	CHECK(fake_wait(&fake.blocked_calls, 1));

	/* Stopping cancels the probe and joins the worker. */
	mps_duration_inventory_stop();
	CHECK(fake_value(&fake.cancelled_calls) == 1);
	CHECK(mps_duration_inventory_wait_idle(inventory, 0));

	mps_duration_inventory_destroy(inventory);
	mps_duration_inventory_start(test_changed, NULL);
}

int main(void)
{
	pthread_mutex_init(&fake.mutex, NULL);
	pthread_cond_init(&fake.cond, NULL);

	mps_duration_inventory_start(test_changed, NULL);

	test_null_arguments();
	test_cache_and_reuse();
	test_file_change_invalidates();
	test_mtime_nsec_invalidates();
	test_non_local_paths();
	test_failed_probe_is_sticky();
	test_cancelled_generation();
	test_cancelled_before_probe();
	test_destroy_during_probe();
	test_duplicate_paths_settle();
	test_path_length_bounds();
	test_overlong_canonical_rejected();
	test_entry_bounds();
	test_global_entry_bounds();
	test_global_budget_replacement();
	test_stop_and_restart();

	mps_duration_inventory_stop();

	printf("duration-inventory: all tests passed\n");
	return 0;
}
