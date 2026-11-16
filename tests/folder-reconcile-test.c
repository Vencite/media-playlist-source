#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <util/platform.h>
#include <string.h>
#include "folder-reconcile.h"

static size_t allocation_calls, sort_calls;
static char *tracked_strdup(const char *value)
{
	++allocation_calls;
	return bstrdup(value);
}
static void *tracked_zero(size_t size)
{
	++allocation_calls;
	return bzalloc(size);
}
static void *tracked_realloc(void *value, size_t size)
{
	++allocation_calls;
	return brealloc(value, size);
}
static void tracked_sort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
	++sort_calls;
	qsort(base, count, size, compare);
}
#define bstrdup tracked_strdup
#define bzalloc tracked_zero
#define brealloc tracked_realloc
#define qsort tracked_sort
#include "../src/folder-reconcile.c"
#undef bstrdup
#undef bzalloc
#undef brealloc
#undef qsort

static void assert_noop(struct media_file_data *folder, const struct mps_folder_snapshot *scan,
			const struct mps_folder_snapshot *previous, enum mps_folder_sort sort, bool wait,
			struct darray *children)
{
	size_t allocations = allocation_calls, sorts = sort_calls;
	assert(!mps_folder_reconcile(folder, scan, previous, sort, wait, children));
	assert(allocation_calls == allocations);
	assert(sort_calls == sorts);
}

static void replace(struct media_file_data *folder, struct darray *children)
{
	mps_folder_children_free(&folder->folder_items.da);
	folder->folder_items.da = *children;
}

static int benchmark_noop(void)
{
	const size_t count = 10000, repeats = 100;
	struct media_file_data folder = {.id = "benchmark", .path = "/media"};
	struct mps_folder_entry *entries = bzalloc(count * sizeof(*entries));
	for (size_t i = 0; i < count; ++i) {
		char name[32];
		snprintf(name, sizeof(name), "clip-%05zu.mp4", i);
		entries[i].name = bstrdup(name);
		entries[i].size = 100;
		entries[i].mtime = (int64_t)i;
	}
	struct mps_folder_snapshot scan = {.success = true, .entries = entries, .count = count};
	struct darray children;
	assert(mps_folder_reconcile(&folder, &scan, NULL, MPS_FOLDER_AZ, false, &children));
	replace(&folder, &children);
	size_t allocations = allocation_calls, sorts = sort_calls;
	uint64_t started = os_gettime_ns();
	for (size_t i = 0; i < repeats; ++i)
		assert(!mps_folder_reconcile(&folder, &scan, &scan, MPS_FOLDER_AZ, true, &children));
	double milliseconds = (double)(os_gettime_ns() - started) / 1e6 / repeats;
	printf("10000 entries: %.3f ms/scan, %zu clone/allocation calls, %zu sorts (%zu scans)\n", milliseconds,
	       allocation_calls - allocations, sort_calls - sorts, repeats);
	mps_folder_children_free(&folder.folder_items.da);
	for (size_t i = 0; i < count; ++i)
		bfree(entries[i].name);
	bfree(entries);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--benchmark") == 0)
		return benchmark_noop();
	struct media_file_data folder = {.id = "folder", .path = "/media"};
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10, .mtime = 20},
					     {.name = "b.mp4", .size = 20, .mtime = 10},
					     {.name = "c.mp4", .size = 30, .mtime = 20}};
	struct mps_folder_snapshot scan = {.success = true, .entries = entries, .count = 3};
	struct darray children;
	/* First scan cannot admit new files. Second stable scan can. */
	assert_noop(&folder, &scan, NULL, MPS_FOLDER_KEEP, true, &children);
	assert(mps_folder_reconcile(&folder, &scan, &scan, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(folder.folder_items.num == 3);
	assert_noop(&folder, &scan, &scan, MPS_FOLDER_KEEP, true, &children);
	const enum mps_folder_sort modes[] = {MPS_FOLDER_ZA, MPS_FOLDER_AZ, MPS_FOLDER_NEWEST, MPS_FOLDER_OLDEST};
	const char *orders[][3] = {{"c.mp4", "b.mp4", "a.mp4"},
				   {"a.mp4", "b.mp4", "c.mp4"},
				   {"a.mp4", "c.mp4", "b.mp4"},
				   {"b.mp4", "a.mp4", "c.mp4"}};
	for (size_t i = 0; i < 4; ++i) {
		assert(mps_folder_reconcile(&folder, &scan, &scan, modes[i], true, &children));
		replace(&folder, &children);
		for (size_t j = 0; j < 3; ++j)
			assert(strcmp(folder.folder_items.array[j].filename, orders[i][j]) == 0);
		assert_noop(&folder, &scan, &scan, modes[i], true, &children);
	}
	/* Existing order is retained; new files append in scan's deterministic order. */
	struct mps_folder_entry expanded[] = {{.name = "0.mp4", .size = 40}, entries[0], entries[1], entries[2]};
	struct mps_folder_snapshot added = {.success = true, .entries = expanded, .count = 4};
	assert_noop(&folder, &added, &scan, MPS_FOLDER_KEEP, true, &children);
	assert(mps_folder_reconcile(&folder, &added, &added, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(strcmp(folder.folder_items.array[0].filename, "b.mp4") == 0);
	assert(strcmp(folder.folder_items.array[3].filename, "0.mp4") == 0);
	/* An admitted file being overwritten keeps its old stamp until stable. */
	expanded[0].size = 99;
	assert_noop(&folder, &added, &scan, MPS_FOLDER_KEEP, true, &children);
	assert(folder.folder_items.array[3].monitor_size == 40);
	assert(mps_folder_reconcile(&folder, &added, &added, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(folder.folder_items.array[3].monitor_size == 99);
	/* Failure retains contents; successful empty enumeration removes them. */
	struct mps_folder_snapshot failed = {0}, empty = {.success = true};
	assert_noop(&folder, &failed, NULL, MPS_FOLDER_KEEP, false, &children);
	assert(folder.folder_items.num == 4);
	assert(mps_folder_reconcile(&folder, &empty, NULL, MPS_FOLDER_KEEP, false, &children));
	replace(&folder, &children);
	assert(folder.folder_items.num == 0);
	/* Wait off admits immediately; nanosecond changes are effective changes. */
	assert(mps_folder_reconcile(&folder, &scan, NULL, MPS_FOLDER_KEEP, false, &children));
	replace(&folder, &children);
	entries[0].mtime_nsec = 1;
	assert(mps_folder_reconcile(&folder, &scan, NULL, MPS_FOLDER_KEEP, false, &children));
	replace(&folder, &children);
	mps_folder_children_free(&folder.folder_items.da);
	puts("folder reconciliation tests passed");
	return 0;
}
