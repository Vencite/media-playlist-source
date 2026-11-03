/*
Media Playlist Source
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "playlist-telemetry.h"

#include <string.h>

#include <util/bmem.h>

static const char *telemetry_state_name(enum obs_media_state state)
{
	/* OPENING/BUFFERING are active playback transitions in OBS; the public
	 * schema only exposes the stable playing/paused/stopped/ended/unknown
	 * states. */
	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
	case OBS_MEDIA_STATE_OPENING:
	case OBS_MEDIA_STATE_BUFFERING:
		return "playing";
	case OBS_MEDIA_STATE_PAUSED:
		return "paused";
	case OBS_MEDIA_STATE_STOPPED:
		return "stopped";
	case OBS_MEDIA_STATE_ENDED:
		return "ended";
	case OBS_MEDIA_STATE_NONE:
	case OBS_MEDIA_STATE_ERROR:
	default:
		return "unknown";
	}
}

static void set_string_if_known(obs_data_t *data, const char *key, const char *value)
{
	if (value)
		obs_data_set_string(data, key, value);
}

static void set_int_if_known(obs_data_t *data, const char *key, int64_t value)
{
	if (value >= 0)
		obs_data_set_int(data, key, value);
}

static void set_size_if_known(obs_data_t *data, const char *key, size_t value)
{
	if (value != MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE && value <= (size_t)INT64_MAX)
		obs_data_set_int(data, key, (int64_t)value);
}

void mps_playlist_telemetry_init(struct mps_playlist_telemetry *snapshot)
{
	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->state = OBS_MEDIA_STATE_NONE;
	snapshot->speed_percent = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->elapsed_media_ms = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->duration_media_ms = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->remaining_ms = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->playlist_remaining_ms = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->playlist_index = MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE;
	snapshot->folder_item_index = MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE;
	snapshot->items_remaining = MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE;
}

void mps_playlist_telemetry_free(struct mps_playlist_telemetry *snapshot)
{
	if (!snapshot)
		return;

	bfree(snapshot->source_uuid);
	bfree(snapshot->source_name);
	bfree(snapshot->current_item_id);
	bfree(snapshot->current_path);
	bfree(snapshot->current_file);
	snapshot->source_uuid = NULL;
	snapshot->source_name = NULL;
	snapshot->current_item_id = NULL;
	snapshot->current_path = NULL;
	snapshot->current_file = NULL;
}

obs_data_t *mps_playlist_telemetry_to_data(const struct mps_playlist_telemetry *snapshot)
{
	obs_data_t *data;

	if (!snapshot)
		return NULL;

	bool remaining_known = snapshot->remaining_ms >= 0;
	bool playlist_remaining_known = snapshot->playlist_remaining_known && snapshot->playlist_remaining_ms >= 0;
	bool items_remaining_known = snapshot->items_remaining != MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE &&
				     snapshot->items_remaining <= (size_t)INT64_MAX;

	data = obs_data_create();
	if (!data)
		return NULL;

	obs_data_set_int(data, "schema_version", MPS_PLAYLIST_TELEMETRY_SCHEMA_VERSION);
	set_string_if_known(data, "source_uuid", snapshot->source_uuid);
	set_string_if_known(data, "source_name", snapshot->source_name);
	obs_data_set_int(data, "snapshot_revision", (int64_t)snapshot->snapshot_revision);
	obs_data_set_string(data, "state", telemetry_state_name(snapshot->state));
	set_string_if_known(data, "current_item_id", snapshot->current_item_id);
	set_string_if_known(data, "current_path", snapshot->current_path);
	set_string_if_known(data, "current_file", snapshot->current_file);
	set_size_if_known(data, "playlist_index", snapshot->playlist_index);
	set_size_if_known(data, "folder_item_index", snapshot->folder_item_index);
	set_int_if_known(data, "elapsed_media_ms", snapshot->elapsed_media_ms);
	set_int_if_known(data, "duration_media_ms", snapshot->duration_media_ms);

	obs_data_set_bool(data, "remaining_known", remaining_known);
	if (remaining_known)
		obs_data_set_int(data, "remaining_ms", snapshot->remaining_ms);

	obs_data_set_bool(data, "playlist_remaining_known", playlist_remaining_known);
	if (playlist_remaining_known)
		obs_data_set_int(data, "playlist_remaining_ms", snapshot->playlist_remaining_ms);

	obs_data_set_bool(data, "items_remaining_known", items_remaining_known);
	if (items_remaining_known)
		obs_data_set_int(data, "items_remaining", (int64_t)snapshot->items_remaining);

	if (snapshot->speed_percent > 0)
		obs_data_set_int(data, "speed_percent", snapshot->speed_percent);

	obs_data_set_bool(data, "shuffle", snapshot->shuffle);
	obs_data_set_bool(data, "loop", snapshot->loop);
	obs_data_set_bool(data, "has_next", snapshot->has_next);
	obs_data_set_bool(data, "has_previous", snapshot->has_previous);

	return data;
}

static bool scaled_media_time_to_end(int64_t media_ms, int64_t speed_percent, int64_t *result)
{
	int64_t scaled;
	int64_t value;

	if (!result || media_ms < 0 || speed_percent <= 0 || media_ms > INT64_MAX / 100)
		return false;

	scaled = media_ms * 100;
	value = scaled / speed_percent;
	if (scaled % speed_percent)
		value++;

	*result = value;
	return true;
}

static bool add_future_media_duration(int64_t media_ms, int64_t speed_percent, int64_t *total)
{
	int64_t value;

	/* A non-positive future duration means the duration inventory is not
	 * usable, not that the entry is known to take no time. */
	if (media_ms <= 0 || !scaled_media_time_to_end(media_ms, speed_percent, &value))
		return false;
	if (*total > INT64_MAX - value)
		return false;

	*total += value;
	return true;
}

void mps_playlist_telemetry_calculate_at_speeds(struct mps_playlist_telemetry *snapshot,
						const int64_t *future_media_durations,
						const int64_t *future_speed_percents, size_t count, bool pass_known)
{
	bool has_current;
	bool current_known = false;
	int64_t current_remaining = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;

	if (!snapshot)
		return;

	has_current = snapshot->current_item_id != NULL || snapshot->current_path != NULL;
	if (has_current && snapshot->duration_media_ms > 0 && snapshot->elapsed_media_ms >= 0) {
		int64_t elapsed = snapshot->elapsed_media_ms;
		int64_t media_remaining = snapshot->duration_media_ms > elapsed ? snapshot->duration_media_ms - elapsed
										: 0;

		current_known = scaled_media_time_to_end(media_remaining, snapshot->speed_percent, &current_remaining);
	}

	snapshot->remaining_ms = current_known ? current_remaining : MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
	snapshot->items_remaining = pass_known && has_current ? count : MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE;

	if (!current_known || !pass_known) {
		snapshot->playlist_remaining_ms = MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
		snapshot->playlist_remaining_known = false;
		return;
	}

	int64_t total = current_remaining;
	bool known = true;
	for (size_t i = 0; i < count; i++) {
		int64_t future_speed = future_speed_percents ? future_speed_percents[i] : snapshot->speed_percent;
		if (!future_media_durations ||
		    !add_future_media_duration(future_media_durations[i], future_speed, &total)) {
			known = false;
			break;
		}
	}

	snapshot->playlist_remaining_known = known;
	snapshot->playlist_remaining_ms = known ? total : MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT;
}

void mps_playlist_telemetry_calculate(struct mps_playlist_telemetry *snapshot, const int64_t *future_media_durations,
				      size_t count, bool pass_known)
{
	mps_playlist_telemetry_calculate_at_speeds(snapshot, future_media_durations, NULL, count, pass_known);
}

static char *copy_stable_id(const struct media_file_data *media)
{
	if (!media)
		return NULL;
	if (media->id)
		return bstrdup(media->id);
	if (!media->parent_id || !media->filename)
		return NULL;

	size_t parent_length = strlen(media->parent_id);
	size_t filename_length = strlen(media->filename);
	char *stable_id = bmalloc(parent_length + filename_length + 2);
	if (!stable_id)
		return NULL;

	memcpy(stable_id, media->parent_id, parent_length);
	stable_id[parent_length] = '/';
	memcpy(stable_id + parent_length + 1, media->filename, filename_length + 1);
	return stable_id;
}

static bool copy_playlist_item(struct mps_playlist_item_snapshot *snapshot, const struct media_file_data *media,
			       size_t media_index, size_t folder_item_index, bool is_folder_child)
{
	snapshot->path = media && media->path ? bstrdup(media->path) : NULL;
	snapshot->filename = media && media->filename ? bstrdup(media->filename) : NULL;
	snapshot->stable_id = copy_stable_id(media);
	snapshot->media_index = media_index;
	snapshot->folder_item_index = folder_item_index;
	snapshot->is_folder = media && media->is_folder;
	snapshot->is_folder_child = is_folder_child;
	snapshot->is_current = false;

	return (!media || !media->path || snapshot->path) && (!media || !media->filename || snapshot->filename) &&
	       (!media || (!media->id && !media->parent_id) || snapshot->stable_id);
}

static bool copy_sequential_pass(const struct mps_playlist_context_input *input,
				 struct mps_playlist_item_snapshot **remaining, size_t *count)
{
	struct darray *files = input->files;
	const struct media_file_data *top_level;
	const struct media_file_data *current;
	struct mps_playlist_item_snapshot *output;
	size_t first_child = 0;
	size_t start_top;
	size_t total = 0;
	size_t written = 0;
	bool in_folder = false;

	if (!files || !files->num)
		return true;
	if (!files->array || !input->actual_media || !input->current_media || input->current_media_index >= files->num)
		return false;

	top_level = &((const struct media_file_data *)files->array)[input->current_media_index];
	current = input->current_media;
	if (current != top_level)
		return false;

	start_top = input->current_media_index + 1;
	if (current->is_folder) {
		first_child = input->current_folder_item_index;
		if (!input->actual_media || first_child >= current->folder_items.num ||
		    &current->folder_items.array[first_child] != input->actual_media)
			return false;
		in_folder = true;
		total += current->folder_items.num - first_child - 1;
	} else if (input->actual_media != current) {
		return false;
	}

	for (size_t i = start_top; i < files->num; i++) {
		const struct media_file_data *media = &((const struct media_file_data *)files->array)[i];
		total += media->is_folder ? media->folder_items.num : 1;
	}

	if (!total)
		return true;

	output = bzalloc(total * sizeof(*output));
	if (!output)
		return false;

	if (in_folder) {
		for (size_t i = first_child + 1; i < current->folder_items.num; i++) {
			const struct media_file_data *media = &current->folder_items.array[i];
			if (!copy_playlist_item(&output[written++], media, input->current_media_index, i, true))
				goto error;
		}
	}

	for (size_t i = start_top; i < files->num; i++) {
		const struct media_file_data *media = &((const struct media_file_data *)files->array)[i];
		if (media->is_folder) {
			for (size_t j = 0; j < media->folder_items.num; j++) {
				const struct media_file_data *child = &media->folder_items.array[j];
				if (!copy_playlist_item(&output[written++], child, i, j, true))
					goto error;
			}
		} else {
			if (!copy_playlist_item(&output[written++], media, i, 0, false))
				goto error;
		}
	}

	*remaining = output;
	*count = written;
	return true;

error:
	mps_playlist_entries_free(output, written);
	return false;
}

static size_t shuffle_media_index(const struct mps_playlist_context_input *input, const struct media_file_data *media)
{
	if (!media->parent_id)
		return media->index;
	if (media->parent)
		return media->parent->index;

	if (input->files && input->files->array) {
		for (size_t i = 0; i < input->files->num; i++) {
			const struct media_file_data *top_level =
				&((const struct media_file_data *)input->files->array)[i];
			if (top_level->id && strcmp(top_level->id, media->parent_id) == 0)
				return i;
		}
	}
	return media->index;
}

static bool copy_shuffle_pass(const struct mps_playlist_context_input *input,
			      struct mps_playlist_item_snapshot **remaining, size_t *count)
{
	struct shuffler *shuffler = input->shuffler;
	struct mps_playlist_item_snapshot *output;
	size_t num;

	if (!shuffler)
		return false;

	num = shuffler->shuffled_files.num;
	if (!num)
		return true;
	if (!shuffler->shuffled_files.array || shuffler->head > num || shuffler->next > num || shuffler->history > num)
		return false;
	if (!input->actual_media)
		return false;

	if (shuffler->next == num) {
		/* The current item is the last one in this pass; looping does not
		 * continue the current pass. This is also reachable while a next
		 * cycle has been prepeeked but head is still behind the cursor. */
		return shuffler->shuffled_files.array[num - 1] == input->actual_media;
	}
	if (shuffler->next > shuffler->head)
		return false;
	if (shuffler->next == 0)
		/* No cursor item to validate. This includes history traversal and
		 * an auto-prepeeked next cycle. */
		return false;

	if (shuffler->shuffled_files.array[shuffler->next - 1] != input->actual_media)
		return false;

	size_t total = num - shuffler->next;
	output = bzalloc(total * sizeof(*output));
	if (!output)
		return false;

	for (size_t i = shuffler->next; i < num; i++) {
		const struct media_file_data *media = shuffler->shuffled_files.array[i];
		size_t media_index = shuffle_media_index(input, media);
		size_t folder_item_index = media->parent_id ? media->index : 0;
		if (!copy_playlist_item(&output[i - shuffler->next], media, media_index, folder_item_index,
					media->parent_id != NULL)) {
			mps_playlist_entries_free(output, total);
			return false;
		}
	}

	*remaining = output;
	*count = total;
	return true;
}

bool mps_playlist_pass_copy(const struct mps_playlist_context_input *input,
			    struct mps_playlist_item_snapshot **remaining, size_t *count)
{
	if (!remaining || !count)
		return false;
	*remaining = NULL;
	*count = 0;
	if (!input)
		return false;

	if (input->shuffle)
		return copy_shuffle_pass(input, remaining, count);
	return copy_sequential_pass(input, remaining, count);
}
