#include "folder-reconcile.h"

#include <stdlib.h>
#include <string.h>
#include <util/dstr.h>

static const struct mps_folder_entry *find_entry(const struct mps_folder_snapshot *scan, const char *name)
{
	if (!scan || !scan->success || !scan->count)
		return NULL;
	struct mps_folder_entry key = {.name = (char *)name};
	size_t low = 0, high = scan->count;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		int cmp = strcmp(scan->entries[mid].name, key.name);
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low < scan->count && strcmp(scan->entries[low].name, name) == 0 ? &scan->entries[low] : NULL;
}

static bool equal_stamp(const struct mps_folder_entry *a, const struct mps_folder_entry *b)
{
	return a && b && a->size == b->size && a->mtime == b->mtime && a->mtime_nsec == b->mtime_nsec;
}

static bool media_stamp_equal(const struct media_file_data *media, const struct mps_folder_entry *entry)
{
	return media->monitor_size == entry->size && media->monitor_mtime == entry->mtime &&
	       media->monitor_mtime_nsec == entry->mtime_nsec;
}

static void set_stamp(struct media_file_data *media, const struct mps_folder_entry *entry)
{
	media->monitor_stamp = true;
	media->monitor_size = entry->size;
	media->monitor_mtime = entry->mtime;
	media->monitor_mtime_nsec = entry->mtime_nsec;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(((const struct media_file_data *)a)->filename, ((const struct media_file_data *)b)->filename);
}

static int by_name_reverse(const void *a, const void *b)
{
	return by_name(b, a);
}

static int by_date(const void *a, const void *b)
{
	const struct media_file_data *left = a, *right = b;
	if (left->monitor_mtime != right->monitor_mtime)
		return left->monitor_mtime < right->monitor_mtime ? -1 : 1;
	if (left->monitor_mtime_nsec != right->monitor_mtime_nsec)
		return left->monitor_mtime_nsec < right->monitor_mtime_nsec ? -1 : 1;
	return by_name(a, b);
}

static int by_date_reverse(const void *a, const void *b)
{
	const struct media_file_data *left = a, *right = b;
	if (left->monitor_mtime != right->monitor_mtime)
		return left->monitor_mtime > right->monitor_mtime ? -1 : 1;
	if (left->monitor_mtime_nsec != right->monitor_mtime_nsec)
		return left->monitor_mtime_nsec > right->monitor_mtime_nsec ? -1 : 1;
	return by_name(a, b);
}

void mps_folder_children_free(struct darray *children)
{
	DARRAY(struct media_file_data) items;
	items.da = *children;
	for (size_t i = 0; i < items.num; ++i) {
		bfree(items.array[i].path);
		bfree(items.array[i].filename);
		bfree(items.array[i].id);
	}
	da_free(items);
	*children = items.da;
}

bool mps_folder_reconcile(struct media_file_data *folder, const struct mps_folder_snapshot *scan,
			  const struct mps_folder_snapshot *previous, enum mps_folder_sort sort, bool wait,
			  struct darray *children)
{
	DARRAY(struct media_file_data) result;
	da_init(result);
	*children = result.da;
	if (!scan || !scan->success)
		return false;
	bool *used = scan->count ? bzalloc(scan->count * sizeof(bool)) : NULL;
	bool modified = false;
	for (size_t i = 0; i < folder->folder_items.num; ++i) {
		struct media_file_data *old = &folder->folder_items.array[i];
		const struct mps_folder_entry *entry = find_entry(scan, old->filename);
		if (!entry)
			continue;
		used[entry - scan->entries] = true;
		struct media_file_data item = *old;
		item.path = bstrdup(old->path);
		item.filename = bstrdup(old->filename);
		item.id = old->id ? bstrdup(old->id) : NULL;
		bool ready = !wait || equal_stamp(entry, find_entry(previous, entry->name));
		if (!old->monitor_stamp) {
			/* First observation establishes metadata without a fake playlist change. */
			set_stamp(old, entry);
			set_stamp(&item, entry);
		} else if (ready && !media_stamp_equal(old, entry)) {
			set_stamp(&item, entry);
			modified = true;
		}
		da_push_back(result, &item);
	}
	for (size_t i = 0; i < scan->count; ++i) {
		const struct mps_folder_entry *entry = &scan->entries[i];
		if (used[i] || (wait && !equal_stamp(entry, find_entry(previous, entry->name))))
			continue;
		struct dstr path = {0};
		dstr_copy(&path, folder->path);
		dstr_cat_ch(&path, '/');
		dstr_cat(&path, entry->name);
		struct media_file_data item = {.filename = bstrdup(entry->name), .path = path.array};
		set_stamp(&item, entry);
		da_push_back(result, &item);
	}
	bfree(used);
	int (*compare)(const void *, const void *) = NULL;
	if (sort == MPS_FOLDER_AZ)
		compare = by_name;
	else if (sort == MPS_FOLDER_ZA)
		compare = by_name_reverse;
	else if (sort == MPS_FOLDER_OLDEST)
		compare = by_date;
	else if (sort == MPS_FOLDER_NEWEST)
		compare = by_date_reverse;
	if (compare && result.num > 1)
		qsort(result.array, result.num, sizeof(*result.array), compare);
	bool changed = modified || result.num != folder->folder_items.num;
	for (size_t i = 0; i < result.num; ++i) {
		result.array[i].index = i;
		result.array[i].parent = folder;
		result.array[i].parent_id = folder->id;
		if (!changed && strcmp(result.array[i].filename, folder->folder_items.array[i].filename) != 0)
			changed = true;
	}
	*children = result.da;
	if (!changed)
		mps_folder_children_free(children);
	return changed;
}
