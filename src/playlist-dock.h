#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

#include "playlist-context.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mps_playlist_context_snapshot {
	char *previous;
	char *current;
	char *next;
	bool shuffle;
};

struct mps_playlist_entries_snapshot {
	struct mps_playlist_item_snapshot *items;
	size_t item_count;
	size_t total_count;
	size_t offset;
	uint64_t playlist_revision;
	bool shuffle;
	bool loop;
};

typedef void (*mps_playlist_change_callback_t)(void *data, const char *source_uuid);

/* The caller must hold a valid OBS source reference for the complete query. */
bool mps_playlist_context_snapshot_get(obs_source_t *source, struct mps_playlist_context_snapshot *snapshot);
void mps_playlist_context_snapshot_free(struct mps_playlist_context_snapshot *snapshot);
bool mps_playlist_entries_snapshot_get(obs_source_t *source, struct mps_playlist_entries_snapshot *snapshot);
bool mps_playlist_entries_page_get(obs_source_t *source, size_t offset, size_t limit,
				   struct mps_playlist_entries_snapshot *snapshot);
void mps_playlist_entries_snapshot_free(struct mps_playlist_entries_snapshot *snapshot);
/* Caller holds a source reference and runs on the OBS graphics thread, serializing
 * stable-ID lookup with playlist edits. Returns false without playback changes for
 * missing/ambiguous IDs, folder headers, removed sources or unavailable instances.
 * A nonzero expected_revision must match the current playlist revision. */
bool mps_playlist_select_item(obs_source_t *source, const char *stable_id, uint64_t expected_revision);
bool mps_playlist_timing_get(obs_source_t *source, int64_t *time_ms, int64_t *duration_ms);
bool mps_playlist_standby_duration_get(obs_source_t *source, const char *expected_path, int64_t *duration_ms);

void mps_playlist_change_add_listener(mps_playlist_change_callback_t callback, void *data);
void mps_playlist_change_remove_listener(mps_playlist_change_callback_t callback, void *data);

void mps_playlist_dock_register(void);
void mps_playlist_dock_unregister(void);

#ifdef __cplusplus
}
#endif
