#pragma once

#include <stdbool.h>

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mps_playlist_snapshot {
	char *previous;
	char *current;
	char *next;
};

typedef void (*mps_playlist_change_callback_t)(void *data, const char *source_uuid);

/* The caller must hold a valid OBS source reference for the complete query. */
bool mps_playlist_snapshot_get(obs_source_t *source, struct mps_playlist_snapshot *snapshot);
void mps_playlist_snapshot_free(struct mps_playlist_snapshot *snapshot);

void mps_playlist_change_add_listener(mps_playlist_change_callback_t callback, void *data);
void mps_playlist_change_remove_listener(mps_playlist_change_callback_t callback, void *data);

void mps_playlist_dock_register(void);
void mps_playlist_dock_unregister(void);

#ifdef __cplusplus
}
#endif
