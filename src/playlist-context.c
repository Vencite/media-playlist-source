#include "playlist-context.h"

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
