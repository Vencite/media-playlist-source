#pragma once

#include "folder-monitor.h"
#include "playlist.h"

/* Produces owned children, preserving admitted files while a replacement is unstable. */
bool mps_folder_reconcile(struct media_file_data *folder, const struct mps_folder_snapshot *scan,
			  const struct mps_folder_snapshot *previous, enum mps_folder_sort sort, bool wait,
			  struct darray *children);
void mps_folder_children_free(struct darray *children);
