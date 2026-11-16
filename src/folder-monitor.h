#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mps_folder_sort {
	MPS_FOLDER_KEEP,
	MPS_FOLDER_NEWEST,
	MPS_FOLDER_OLDEST,
	MPS_FOLDER_AZ,
	MPS_FOLDER_ZA,
};

struct mps_folder_entry {
	char *name;
	int64_t size;
	int64_t mtime;
	int64_t mtime_nsec;
};

struct mps_folder_snapshot {
	char *id;
	char *path;
	bool success;
	struct mps_folder_entry *entries;
	size_t count;
};

struct mps_folder_result {
	uint64_t generation;
	struct mps_folder_snapshot *folders;
	size_t count;
};

struct mps_folder_monitor;

struct mps_folder_monitor *mps_folder_monitor_create(void);
/* Does not wait for filesystem calls; the worker retains its own reference. */
void mps_folder_monitor_destroy(struct mps_folder_monitor *monitor);
void mps_folder_monitor_configure(struct mps_folder_monitor *monitor, uint64_t generation, const char *const *ids,
				  const char *const *paths, size_t count, unsigned interval_seconds);
/* Transfers ownership of the latest completed scan, or returns NULL. */
struct mps_folder_result *mps_folder_monitor_take(struct mps_folder_monitor *monitor);
void mps_folder_result_free(struct mps_folder_result *result);
/* Call before unloading the plugin, outside source destruction. */
void mps_folder_monitor_shutdown(void);
