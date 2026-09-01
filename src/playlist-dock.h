#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

#include "playlist-context.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mps_playlist_snapshot {
	char *previous;
	char *current;
	char *next;
	struct mps_playlist_item_snapshot *items;
	size_t item_count;
	int64_t time_ms;
	int64_t duration_ms;
	bool shuffle;
};

typedef void (*mps_playlist_change_callback_t)(void *data, const char *source_uuid);

/* The caller must hold a valid OBS source reference for the complete query. */
bool mps_playlist_snapshot_get(obs_source_t *source, struct mps_playlist_snapshot *snapshot);
void mps_playlist_snapshot_free(struct mps_playlist_snapshot *snapshot);
bool mps_playlist_timing_get(obs_source_t *source, int64_t *time_ms, int64_t *duration_ms);

void mps_playlist_change_add_listener(mps_playlist_change_callback_t callback, void *data);
void mps_playlist_change_remove_listener(mps_playlist_change_callback_t callback, void *data);

void mps_playlist_dock_register(void);
void mps_playlist_dock_unregister(void);

#ifdef __cplusplus
}
#endif
