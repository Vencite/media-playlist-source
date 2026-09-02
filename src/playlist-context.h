#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <util/darray.h>

#include "playlist.h"
#include "shuffler.h"

struct mps_playlist_context_input {
	struct darray *files;
	struct shuffler *shuffler;
	bool shuffle;
	bool loop;
	struct media_file_data *current_media;
	struct media_file_data *actual_media;
	size_t current_media_index;
	size_t current_folder_item_index;
	const char *logical_next_path;
};

struct mps_playlist_context {
	const struct media_file_data *previous;
	const struct media_file_data *current;
	const char *next_path;
};

struct mps_playlist_item_snapshot {
	char *path;
	char *filename;
	char *stable_id;
	size_t media_index;
	size_t folder_item_index;
	bool is_folder;
	bool is_folder_child;
	bool is_current;
};

struct media_file_data *mps_next_sequential_media(struct darray *files, struct media_file_data *current_media,
						  size_t current_media_index, size_t current_folder_item_index,
						  bool loop);
struct media_file_data *mps_previous_sequential_media(struct darray *files, struct media_file_data *current_media,
						      size_t current_media_index, size_t current_folder_item_index,
						      bool loop);

void mps_playlist_context_resolve(const struct mps_playlist_context_input *input, struct mps_playlist_context *output);

bool mps_playlist_entries_copy(const struct darray *files, const struct media_file_data *actual_media,
			       struct mps_playlist_item_snapshot **items, size_t *item_count);
void mps_playlist_entries_free(struct mps_playlist_item_snapshot *items, size_t item_count);
