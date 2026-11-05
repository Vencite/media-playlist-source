#include "playlist-context.h"

#include <string.h>

static struct media_file_data *first_media_in_item(struct media_file_data *media)
{
	if (!media)
		return NULL;
	if (!media->is_folder)
		return media;
	if (!media->folder_items.num)
		return NULL;
	return &media->folder_items.array[0];
}

static struct media_file_data *last_media_in_item(struct media_file_data *media)
{
	if (!media)
		return NULL;
	if (!media->is_folder)
		return media;
	if (!media->folder_items.num)
		return NULL;
	return &media->folder_items.array[media->folder_items.num - 1];
}

struct media_file_data *mps_next_sequential_media(struct darray *files, struct media_file_data *current_media,
						  size_t current_media_index, size_t current_folder_item_index,
						  bool loop)
{
	if (current_media && current_media->is_folder &&
	    current_folder_item_index + 1 < current_media->folder_items.num)
		return &current_media->folder_items.array[current_folder_item_index + 1];

	if (!files || !files->num)
		return NULL;

	for (size_t offset = 1; offset <= files->num; offset++) {
		size_t index = current_media_index + offset;
		if (index >= files->num) {
			if (!loop)
				return NULL;
			index %= files->num;
		}

		struct media_file_data *media = &((struct media_file_data *)files->array)[index];
		struct media_file_data *actual = first_media_in_item(media);
		if (actual)
			return actual;
	}
	return NULL;
}

struct media_file_data *mps_previous_sequential_media(struct darray *files, struct media_file_data *current_media,
						      size_t current_media_index, size_t current_folder_item_index,
						      bool loop)
{
	if (current_media && current_media->is_folder && current_folder_item_index > 0 &&
	    current_folder_item_index <= current_media->folder_items.num)
		return &current_media->folder_items.array[current_folder_item_index - 1];

	if (!files || !files->num || !current_media)
		return NULL;

	for (size_t offset = 1; offset <= files->num; offset++) {
		size_t index;
		if (offset <= current_media_index) {
			index = current_media_index - offset;
		} else {
			if (!loop)
				return NULL;
			index = files->num - (offset - current_media_index);
		}

		struct media_file_data *media = &((struct media_file_data *)files->array)[index];
		struct media_file_data *actual = last_media_in_item(media);
		if (actual)
			return actual;
	}
	return NULL;
}

void mps_playlist_context_resolve(const struct mps_playlist_context_input *input, struct mps_playlist_context *output)
{
	if (!output)
		return;
	*output = (struct mps_playlist_context){0};
	if (!input || !input->files || !input->files->num)
		return;

	output->current = input->actual_media;
	output->next_path = input->logical_next_path;
	if (input->shuffle && input->shuffler) {
		if (shuffler_has_prev(input->shuffler))
			output->previous = shuffler_peek_prev(input->shuffler);
	} else {
		output->previous = mps_previous_sequential_media(input->files, input->current_media,
								 input->current_media_index,
								 input->current_folder_item_index, input->loop);
	}
}

static bool copy_playlist_item_snapshot(struct mps_playlist_item_snapshot *snapshot,
					const struct media_file_data *media, size_t media_index,
					size_t folder_item_index, bool is_folder_child,
					const struct media_file_data *actual_media)
{
	snapshot->path = media && media->path ? bstrdup(media->path) : NULL;
	snapshot->filename = media && media->filename ? bstrdup(media->filename) : NULL;
	if (media && media->id) {
		snapshot->stable_id = bstrdup(media->id);
	} else if (media && media->parent_id && media->filename) {
		const size_t parent_length = strlen(media->parent_id);
		const size_t filename_length = strlen(media->filename);
		snapshot->stable_id = bmalloc(parent_length + filename_length + 2);
		if (snapshot->stable_id) {
			memcpy(snapshot->stable_id, media->parent_id, parent_length);
			snapshot->stable_id[parent_length] = '/';
			memcpy(snapshot->stable_id + parent_length + 1, media->filename, filename_length + 1);
		}
	}
	snapshot->media_index = media_index;
	snapshot->folder_item_index = folder_item_index;
	snapshot->is_folder = media && media->is_folder;
	snapshot->is_folder_child = is_folder_child;
	snapshot->is_current = media == actual_media;
	return (!media || !media->path || snapshot->path) && (!media || !media->filename || snapshot->filename) &&
	       (!media || (!media->id && !media->parent_id) || snapshot->stable_id);
}

bool mps_playlist_entries_copy(const struct darray *files, const struct media_file_data *actual_media,
			       struct mps_playlist_item_snapshot **items, size_t *item_count)
{
	size_t total;
	return mps_playlist_entries_copy_range(files, actual_media, 0, SIZE_MAX, items, item_count, &total);
}

bool mps_playlist_entries_copy_range(const struct darray *files, const struct media_file_data *actual_media,
				     size_t offset, size_t limit, struct mps_playlist_item_snapshot **items,
				     size_t *item_count, size_t *total_count)
{
	if (!items || !item_count || !total_count)
		return false;
	*items = NULL;
	*item_count = 0;
	*total_count = 0;
	if (!files || !files->num)
		return true;
	if (!files->array)
		return false;
	size_t total = 0;
	for (size_t i = 0; i < files->num; i++) {
		const struct media_file_data *media = &((const struct media_file_data *)files->array)[i];
		size_t children = media->is_folder ? media->folder_items.num : 0;
		if (total == SIZE_MAX || children > SIZE_MAX - total - 1)
			return false;
		total += 1 + children;
	}
	*total_count = total;
	if (offset >= total || !limit)
		return true;
	size_t count = total - offset;
	if (count > limit)
		count = limit;
	if (count > SIZE_MAX / sizeof(struct mps_playlist_item_snapshot))
		return false;
	struct mps_playlist_item_snapshot *output = bzalloc(count * sizeof(*output));
	if (!output)
		return false;
	size_t cursor = 0;
	size_t written = 0;
	for (size_t i = 0; i < files->num && written < count; i++) {
		const struct media_file_data *media = &((const struct media_file_data *)files->array)[i];
		if (cursor++ >= offset) {
			if (!copy_playlist_item_snapshot(&output[written++], media, i, 0, false, actual_media))
				goto error;
		}
		for (size_t j = 0; media->is_folder && j < media->folder_items.num && written < count; j++) {
			if (cursor++ >= offset) {
				if (!copy_playlist_item_snapshot(&output[written++], &media->folder_items.array[j], i,
								 j, true, actual_media))
					goto error;
			}
		}
	}
	*items = output;
	*item_count = written;
	return true;
error:
	mps_playlist_entries_free(output, written);
	return false;
}

void mps_playlist_entries_free(struct mps_playlist_item_snapshot *items, size_t item_count)
{
	if (!items)
		return;
	for (size_t i = 0; i < item_count; i++) {
		bfree(items[i].path);
		bfree(items[i].filename);
		bfree(items[i].stable_id);
	}
	bfree(items);
}
