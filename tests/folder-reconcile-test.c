#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "folder-reconcile.h"

static void replace(struct media_file_data *folder, struct darray *children)
{
	mps_folder_children_free(&folder->folder_items.da);
	folder->folder_items.da = *children;
}

int main(void)
{
	struct media_file_data folder = {.id = "folder", .path = "/media"};
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10, .mtime = 20},
					     {.name = "b.mp4", .size = 20, .mtime = 10},
					     {.name = "c.mp4", .size = 30, .mtime = 20}};
	struct mps_folder_snapshot scan = {.success = true, .entries = entries, .count = 3};
	struct darray children;
	/* First scan cannot admit new files. Second stable scan can. */
	assert(!mps_folder_reconcile(&folder, &scan, NULL, MPS_FOLDER_KEEP, true, &children));
	assert(mps_folder_reconcile(&folder, &scan, &scan, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(folder.folder_items.num == 3);
	assert(!mps_folder_reconcile(&folder, &scan, &scan, MPS_FOLDER_KEEP, true, &children));
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
		assert(!mps_folder_reconcile(&folder, &scan, &scan, modes[i], true, &children));
	}
	/* Existing order is retained; new files append in scan's deterministic order. */
	struct mps_folder_entry expanded[] = {{.name = "0.mp4", .size = 40}, entries[0], entries[1], entries[2]};
	struct mps_folder_snapshot added = {.success = true, .entries = expanded, .count = 4};
	assert(!mps_folder_reconcile(&folder, &added, &scan, MPS_FOLDER_KEEP, true, &children));
	assert(mps_folder_reconcile(&folder, &added, &added, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(strcmp(folder.folder_items.array[0].filename, "b.mp4") == 0);
	assert(strcmp(folder.folder_items.array[3].filename, "0.mp4") == 0);
	/* An admitted file being overwritten keeps its old stamp until stable. */
	expanded[0].size = 99;
	assert(!mps_folder_reconcile(&folder, &added, &scan, MPS_FOLDER_KEEP, true, &children));
	assert(folder.folder_items.array[3].monitor_size == 40);
	assert(mps_folder_reconcile(&folder, &added, &added, MPS_FOLDER_KEEP, true, &children));
	replace(&folder, &children);
	assert(folder.folder_items.array[3].monitor_size == 99);
	/* Failure retains contents; successful empty enumeration removes them. */
	struct mps_folder_snapshot failed = {0}, empty = {.success = true};
	assert(!mps_folder_reconcile(&folder, &failed, NULL, MPS_FOLDER_KEEP, false, &children));
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
