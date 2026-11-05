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
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "media-playlist-source.h"
#include "playlist-context.h"
#include "playlist-dock.h"
#include "playlist-telemetry.h"
#include "duration-inventory.h"

#define S_PLAYLIST "playlist"
#define S_LOOP "loop"
#define S_SHUFFLE "shuffle"
#define S_SHUFFLE_RESET_BEHAVIOR "shuffle_reset_behavior"
#define S_VISIBILITY_BEHAVIOR "visibility_behavior"
#define S_RESTART_BEHAVIOR "restart_behavior"
#define S_CURRENT_FILE_NAME "current_file_name"
#define S_SELECT_FILE "select_file"

#define S_CURRENT_MEDIA_INDEX "current_media_index"
#define S_CURRENT_FOLDER_ITEM_FILENAME "current_folder_item_filename"
#define S_ID "uuid"
#define S_IS_URL "is_url"
#define S_SPEED "speed_percent"
#define S_REFRESH_FILENAME "refresh_filename"

/* Media Source Settings */
#define S_FFMPEG_LOCAL_FILE "local_file"
#define S_FFMPEG_INPUT "input"
#define S_FFMPEG_IS_LOCAL_FILE "is_local_file"
#define S_FFMPEG_HW_DECODE "hw_decode"
#define S_FFMPEG_CLOSE_WHEN_INACTIVE "close_when_inactive"
#define S_FFMPEG_RESTART_ON_ACTIVATE "restart_on_activate"
#define S_FFMPEG_CLEAR_ON_MEDIA_END "clear_on_media_end"
#define S_FFMPEG_IS_STINGER "is_stinger"

#define T_(text) obs_module_text(text)
#define T_PLAYLIST T_("Playlist")
#define T_LOOP T_("LoopPlaylist")
#define T_SHUFFLE T_("Shuffle")
#define T_SHUFFLE_RESET_BEHAVIOR T_("ShuffleResetBehavior")
#define T_SHUFFLE_RESET_PRESERVE T_("ShuffleResetBehavior.Preserve")
#define T_SHUFFLE_RESET_ON_ACTIVATE T_("ShuffleResetBehavior.OnActivate")
#define T_VISIBILITY_BEHAVIOR T_("VisibilityBehavior")
#define T_VISIBILITY_BEHAVIOR_STOP_RESTART T_("VisibilityBehavior.StopRestart")
#define T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE T_("VisibilityBehavior.PauseUnpause")
#define T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY T_("VisibilityBehavior.AlwaysPlay")
#define T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT T_("VisibilityBehavior.StopPlayNext")
#define T_RESTART_BEHAVIOR T_("RestartBehavior")
#define T_RESTART_BEHAVIOR_CURRENT_FILE T_("RestartBehavior.CurrentFile")
#define T_RESTART_BEHAVIOR_FIRST_FILE T_("RestartBehavior.FirstFile")
#define T_CURRENT_FILE_NAME T_("CurrentFileName")
#define T_SELECT_FILE T_("SelectFile")
#define T_NO_FILE_SELECTED T_("NoFileSelected")
#define T_USE_HARDWARE_DECODING T_("UseHardwareDecoding")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE T_("CloseFileWhenInactive")
#define T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP T_("CloseFileWhenInactive.Tooltip")
#define T_SPEED T_("Speed")
#define T_SPEED_WARNING T_("SpeedWarning")
#define T_REFRESH_FILENAME T_("RefreshFilename")

/* A snapshot needs the state before and after the decoder clock read to match;
 * a cut racing the read is retried this many times. */
#define MPS_TELEMETRY_READ_ATTEMPTS 2
/* Bound read-only enumeration/copying and cache lookups per observation. */
#define MPS_TELEMETRY_MAX_DURATION_LOOKUPS MPS_DURATION_INVENTORY_MAX_ENTRIES

#define T_PLAY_PAUSE T_("PlayPause")
#define T_RESTART T_("Restart")
#define T_STOP T_("Stop")
#define T_PLAYLIST_NEXT T_("Next")
#define T_PLAYLIST_PREV T_("Previous")

struct mps_registry_entry {
	obs_source_t *source;
	struct media_playlist_source *mps;
};

struct mps_change_listener {
	mps_playlist_change_callback_t callback;
	void *data;
	size_t references;
	size_t active_callbacks;
	bool removed;
};

static DARRAY(struct mps_registry_entry) mps_registry;
static DARRAY(struct mps_change_listener *) mps_change_listeners;
static pthread_mutex_t mps_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mps_change_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mps_change_condition = PTHREAD_COND_INITIALIZER;

struct mps_change_callback_frame {
	struct mps_change_listener *listener;
	struct mps_change_callback_frame *previous;
};

static _Thread_local struct mps_change_callback_frame *mps_change_callback_frame;

static bool playlist_has_next(struct media_playlist_source *mps);
static bool playlist_has_previous(struct media_playlist_source *mps);
static void prepare_next_media(struct media_playlist_source *mps);

static void mps_playlist_change_notify(struct media_playlist_source *mps)
{
	pthread_mutex_lock(&mps->lifecycle_mutex);
	++mps->telemetry_discontinuity;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	char *source_uuid = NULL;
	DARRAY(struct mps_change_listener *) listeners;
	da_init(listeners);

	pthread_mutex_lock(&mps_registry_mutex);
	for (size_t i = 0; i < mps_registry.num; i++) {
		if (mps_registry.array[i].mps == mps) {
			if (mps->source_uuid)
				source_uuid = bstrdup(mps->source_uuid);
			break;
		}
	}
	pthread_mutex_unlock(&mps_registry_mutex);
	if (!source_uuid) {
		da_free(listeners);
		return;
	}

	pthread_mutex_lock(&mps_change_mutex);
	for (size_t i = 0; i < mps_change_listeners.num; i++) {
		struct mps_change_listener *listener = mps_change_listeners.array[i];
		if (listener->removed)
			continue;
		++listener->references;
		da_push_back(listeners, &listener);
	}
	pthread_mutex_unlock(&mps_change_mutex);

	for (size_t i = 0; i < listeners.num; i++) {
		struct mps_change_listener *listener = listeners.array[i];
		mps_playlist_change_callback_t callback;
		void *data;

		pthread_mutex_lock(&mps_change_mutex);
		callback = listener->removed ? NULL : listener->callback;
		data = listener->data;
		if (callback)
			++listener->active_callbacks;
		pthread_mutex_unlock(&mps_change_mutex);
		if (callback) {
			struct mps_change_callback_frame frame = {
				.listener = listener,
				.previous = mps_change_callback_frame,
			};
			mps_change_callback_frame = &frame;
			callback(data, source_uuid);
			mps_change_callback_frame = frame.previous;
		}

		pthread_mutex_lock(&mps_change_mutex);
		if (callback)
			--listener->active_callbacks;
		--listener->references;
		if (listener->removed && !listener->references)
			bfree(listener);
		pthread_cond_broadcast(&mps_change_condition);
		pthread_mutex_unlock(&mps_change_mutex);
	}
	da_free(listeners);
	bfree(source_uuid);
}

static bool clear_logical_next_path_locked(struct media_playlist_source *mps)
{
	bool changed = mps->logical_next_path != NULL;
	bfree(mps->logical_next_path);
	mps->logical_next_path = NULL;
	return changed;
}

static bool invalidate_logical_next_path(struct media_playlist_source *mps)
{
	bool changed;
	pthread_mutex_lock(&mps->mutex);
	changed = clear_logical_next_path_locked(mps);
	pthread_mutex_unlock(&mps->mutex);
	if (changed)
		mps_playlist_change_notify(mps);
	return changed;
}

void mps_playlist_change_add_listener(mps_playlist_change_callback_t callback, void *data)
{
	struct mps_change_listener *listener;

	if (!callback)
		return;

	listener = bzalloc(sizeof(*listener));
	if (!listener)
		return;
	listener->callback = callback;
	listener->data = data;

	pthread_mutex_lock(&mps_change_mutex);
	for (size_t i = 0; i < mps_change_listeners.num; i++) {
		if (mps_change_listeners.array[i]->callback == callback &&
		    mps_change_listeners.array[i]->data == data) {
			pthread_mutex_unlock(&mps_change_mutex);
			bfree(listener);
			return;
		}
	}
	da_push_back(mps_change_listeners, &listener);
	pthread_mutex_unlock(&mps_change_mutex);
}

void mps_playlist_change_remove_listener(mps_playlist_change_callback_t callback, void *data)
{
	struct mps_change_listener *listener = NULL;
	size_t current_thread_callbacks = 0;

	pthread_mutex_lock(&mps_change_mutex);
	for (size_t i = 0; i < mps_change_listeners.num; i++) {
		if (mps_change_listeners.array[i]->callback == callback &&
		    mps_change_listeners.array[i]->data == data) {
			listener = mps_change_listeners.array[i];
			da_erase(mps_change_listeners, i);
			break;
		}
	}
	if (listener) {
		++listener->references;
		listener->removed = true;
		for (struct mps_change_callback_frame *frame = mps_change_callback_frame; frame;
		     frame = frame->previous) {
			if (frame->listener == listener)
				++current_thread_callbacks;
		}
		while (listener->active_callbacks > current_thread_callbacks)
			pthread_cond_wait(&mps_change_condition, &mps_change_mutex);
		--listener->references;
		if (!listener->references)
			bfree(listener);
	}
	pthread_mutex_unlock(&mps_change_mutex);
}

static void mps_registry_add(struct media_playlist_source *mps)
{
	struct mps_registry_entry entry = {mps->source, mps};
	pthread_mutex_lock(&mps_registry_mutex);
	da_push_back(mps_registry, &entry);
	pthread_mutex_unlock(&mps_registry_mutex);
}

static void mps_registry_remove(struct media_playlist_source *mps)
{
	pthread_mutex_lock(&mps_registry_mutex);
	for (size_t i = 0; i < mps_registry.num; i++) {
		if (mps_registry.array[i].mps == mps) {
			da_erase(mps_registry, i);
			break;
		}
	}
	pthread_mutex_unlock(&mps_registry_mutex);
}

static char *copy_media_path(const struct media_file_data *media)
{
	return media && media->path ? bstrdup(media->path) : NULL;
}

static struct media_playlist_source *mps_registry_lookup(obs_source_t *source)
{
	struct media_playlist_source *mps = NULL;

	pthread_mutex_lock(&mps_registry_mutex);
	for (size_t i = 0; i < mps_registry.num; i++) {
		if (mps_registry.array[i].source == source) {
			mps = mps_registry.array[i].mps;
			break;
		}
	}
	pthread_mutex_unlock(&mps_registry_mutex);
	return mps;
}

static void invalidate_shuffle_cycle_exhausted(struct media_playlist_source *mps)
{
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->telemetry_cycle_exhausted = false;
	mps->telemetry_cycle_exhausted_generation = 0;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

/* Called after a cut publishes slot and before prepare_next_media() can
 * auto-reshuffle an exhausted cycle. Keeping the fact together with the
 * generation lets observers report a known empty current pass afterwards. */
static void record_shuffle_cycle_exhausted(struct media_playlist_source *mps, struct media_source_slot *slot)
{
	bool exhausted;

	pthread_mutex_lock(&mps->mutex);
	exhausted = mps->shuffle && mps->shuffler.shuffled_files.num > 0 &&
		    mps->shuffler.next == mps->shuffler.shuffled_files.num;
	pthread_mutex_unlock(&mps->mutex);

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->active_slot == slot && slot->source) {
		mps->telemetry_cycle_exhausted = exhausted;
		mps->telemetry_cycle_exhausted_generation = slot->source_generation;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

bool mps_playlist_context_snapshot_get(obs_source_t *source, struct mps_playlist_context_snapshot *snapshot)
{
	struct media_playlist_source *mps;

	if (!source || !snapshot)
		return false;
	*snapshot = (struct mps_playlist_context_snapshot){0};

	mps = mps_registry_lookup(source);
	if (!mps)
		return false;

	pthread_mutex_lock(&mps->mutex);
	struct mps_playlist_context_input input = {
		.files = &mps->files.da,
		.shuffler = &mps->shuffler,
		.shuffle = mps->shuffle,
		.loop = mps->loop,
		.current_media = mps->current_media,
		.actual_media = mps->actual_media,
		.current_media_index = mps->current_media_index,
		.current_folder_item_index = mps->current_folder_item_index,
		.logical_next_path = mps->logical_next_path,
	};
	struct mps_playlist_context context;
	mps_playlist_context_resolve(&input, &context);
	snapshot->previous = copy_media_path(context.previous);
	snapshot->current = copy_media_path(context.current);
	snapshot->next = context.next_path ? bstrdup(context.next_path) : NULL;
	snapshot->shuffle = mps->shuffle;
	bool strings_copied = (!context.previous || snapshot->previous) && (!context.current || snapshot->current) &&
			      (!context.next_path || snapshot->next);
	pthread_mutex_unlock(&mps->mutex);
	if (!strings_copied) {
		mps_playlist_context_snapshot_free(snapshot);
		return false;
	}
	return true;
}

void mps_playlist_context_snapshot_free(struct mps_playlist_context_snapshot *snapshot)
{
	if (!snapshot)
		return;
	bfree(snapshot->previous);
	bfree(snapshot->current);
	bfree(snapshot->next);
	*snapshot = (struct mps_playlist_context_snapshot){0};
}

bool mps_playlist_entries_snapshot_get(obs_source_t *source, struct mps_playlist_entries_snapshot *snapshot)
{
	return mps_playlist_entries_page_get(source, 0, SIZE_MAX, snapshot);
}

bool mps_playlist_entries_page_get(obs_source_t *source, size_t offset, size_t limit,
				   struct mps_playlist_entries_snapshot *snapshot)
{
	struct media_playlist_source *mps;
	bool copied;

	if (!source || !snapshot)
		return false;
	*snapshot = (struct mps_playlist_entries_snapshot){0};
	if (obs_source_removed(source))
		return false;

	mps = mps_registry_lookup(source);
	if (!mps)
		return false;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	bool unavailable = mps->destroying || mps->resetting;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (unavailable)
		return false;

	pthread_mutex_lock(&mps->mutex);
	snapshot->shuffle = mps->shuffle;
	snapshot->loop = mps->loop;
	snapshot->offset = offset;
	snapshot->playlist_revision = mps->playlist_revision;
	copied = mps_playlist_entries_copy_range(&mps->files.da, mps->actual_media, offset, limit, &snapshot->items,
						 &snapshot->item_count, &snapshot->total_count);
	pthread_mutex_unlock(&mps->mutex);
	if (!copied) {
		mps_playlist_entries_snapshot_free(snapshot);
		return false;
	}
	return true;
}

void mps_playlist_entries_snapshot_free(struct mps_playlist_entries_snapshot *snapshot)
{
	if (!snapshot)
		return;
	mps_playlist_entries_free(snapshot->items, snapshot->item_count);
	*snapshot = (struct mps_playlist_entries_snapshot){0};
}

bool mps_playlist_timing_get(obs_source_t *source, int64_t *time_ms, int64_t *duration_ms)
{
	if (!source || !time_ms || !duration_ms || !mps_registry_lookup(source))
		return false;
	*time_ms = obs_source_media_get_time(source);
	*duration_ms = obs_source_media_get_duration(source);
	return true;
}

bool mps_playlist_standby_duration_get(obs_source_t *source, const char *expected_path, int64_t *duration_ms)
{
	struct media_playlist_source *mps;
	obs_source_t *standby_source = NULL;

	if (!source || !expected_path || !duration_ms)
		return false;
	mps = mps_registry_lookup(source);
	if (!mps)
		return false;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	struct media_source_slot *standby = mps->standby_slot;
	if (standby && standby->source && !standby->failed && standby->path &&
	    strcmp(standby->path, expected_path) == 0)
		standby_source = obs_source_get_ref(standby->source);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!standby_source)
		return false;

	*duration_ms = obs_source_media_get_duration(standby_source);
	obs_source_release(standby_source);
	return *duration_ms > 0;
}

static const char *telemetry_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');

	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	return slash ? slash + 1 : path;
}

static char *telemetry_item_id(const char *id, const char *filename)
{
	if (!id || !*id)
		return NULL;
	if (!filename)
		return bstrdup(id);
	struct dstr value = {0};
	dstr_cat(&value, id);
	dstr_cat_ch(&value, '/');
	dstr_cat(&value, filename);
	return value.array;
}

static struct media_file_data *telemetry_current_locked(struct media_playlist_source *mps, const char *id,
							const char *filename, const char *path)
{
	if (!id || !path)
		return NULL;
	for (size_t i = 0; i < mps->files.num && i < MPS_TELEMETRY_MAX_DURATION_LOOKUPS; ++i) {
		struct media_file_data *media = &mps->files.array[i];
		if (!media->id || strcmp(media->id, id) != 0)
			continue;
		if (!filename)
			return !media->is_folder && strcmp(media->path, path) == 0 ? media : NULL;
		for (size_t j = 0; j < media->folder_items.num && j < MPS_TELEMETRY_MAX_DURATION_LOOKUPS; ++j) {
			struct media_file_data *child = &media->folder_items.array[j];
			if (strcmp(child->filename, filename) == 0 && strcmp(child->path, path) == 0)
				return child;
		}
	}
	return NULL;
}

/* Bound copying while holding the playlist lock, not just subsequent cache
 * lookups. Oversized inventories are unknown rather than stalling playback. */
static bool telemetry_pass_bounded(const struct media_playlist_source *mps)
{
	if (mps->files.num > MPS_TELEMETRY_MAX_DURATION_LOOKUPS)
		return false;
	size_t count = 0;
	for (size_t i = 0; i < mps->files.num; ++i) {
		const struct media_file_data *media = &mps->files.array[i];
		size_t added = media->is_folder ? media->folder_items.num : 1;
		if (added > MPS_TELEMETRY_MAX_DURATION_LOOKUPS - count)
			return false;
		count += added;
	}
	return true;
}

static bool telemetry_string_equal(const char *left, const char *right)
{
	if (left == right)
		return true;
	if (!left || !right)
		return false;
	return strcmp(left, right) == 0;
}

/* True when the current clock and speed make the current remaining value
 * computable. Only the fact is part of the fingerprint, never the value. */
static bool telemetry_current_remaining_known(const struct mps_playlist_telemetry *snapshot)
{
	return snapshot->current_path != NULL && snapshot->duration_media_ms > 0 && snapshot->elapsed_media_ms >= 0 &&
	       snapshot->speed_percent > 0;
}

static void telemetry_meaning_store(struct mps_telemetry_meaning *meaning,
				    const struct mps_playlist_telemetry *snapshot, bool future_known,
				    int64_t future_media_ms)
{
	char *item_id = snapshot->current_item_id ? bstrdup(snapshot->current_item_id) : NULL;
	char *file = snapshot->current_file ? bstrdup(snapshot->current_file) : NULL;
	char *source_name = snapshot->source_name ? bstrdup(snapshot->source_name) : NULL;

	bfree(meaning->source_name);
	meaning->source_name = source_name;
	bfree(meaning->item_id);
	bfree(meaning->file);
	meaning->item_id = item_id;
	meaning->file = file;
	meaning->state = snapshot->state;
	meaning->speed_percent = snapshot->speed_percent;
	meaning->duration_ms = snapshot->duration_media_ms;
	meaning->future_media_ms = future_media_ms;
	meaning->playlist_index = snapshot->playlist_index;
	meaning->folder_item_index = snapshot->folder_item_index;
	meaning->items_remaining = snapshot->items_remaining;
	meaning->future_known = future_known;
	meaning->current_remaining_known = telemetry_current_remaining_known(snapshot);
	meaning->playlist_remaining_known = snapshot->playlist_remaining_known;
	meaning->shuffle = snapshot->shuffle;
	meaning->loop = snapshot->loop;
	meaning->has_next = snapshot->has_next;
	meaning->has_previous = snapshot->has_previous;
}

static bool telemetry_meaning_matches(const struct mps_telemetry_meaning *meaning,
				      const struct mps_playlist_telemetry *snapshot, bool future_known,
				      int64_t future_media_ms)
{
	return meaning->state == snapshot->state && meaning->speed_percent == snapshot->speed_percent &&
	       meaning->duration_ms == snapshot->duration_media_ms && meaning->future_media_ms == future_media_ms &&
	       meaning->future_known == future_known && meaning->playlist_index == snapshot->playlist_index &&
	       meaning->folder_item_index == snapshot->folder_item_index &&
	       meaning->items_remaining == snapshot->items_remaining &&
	       meaning->current_remaining_known == telemetry_current_remaining_known(snapshot) &&
	       meaning->playlist_remaining_known == snapshot->playlist_remaining_known &&
	       meaning->shuffle == snapshot->shuffle && meaning->loop == snapshot->loop &&
	       meaning->has_next == snapshot->has_next && meaning->has_previous == snapshot->has_previous &&
	       telemetry_string_equal(meaning->item_id, snapshot->current_item_id) &&
	       telemetry_string_equal(meaning->file, snapshot->current_file) &&
	       telemetry_string_equal(meaning->source_name, snapshot->source_name);
}

/* Caller holds telemetry_mutex. A changed fingerprint advances the revision, and
 * so does an event serial observed since the previous read: several
 * notifications between two reads coalesce into one advance, while a read that
 * observed nothing new never advances it. */
static uint64_t telemetry_observe(struct media_playlist_source *mps, const struct mps_playlist_telemetry *snapshot,
				  bool future_known, int64_t future_media_ms, uint64_t discontinuity)
{
	bool changed = !mps->telemetry_meaning_valid ||
		       !telemetry_meaning_matches(&mps->telemetry_meaning, snapshot, future_known, future_media_ms);

	if (changed || discontinuity != mps->telemetry_observed_discontinuity) {
		++mps->telemetry_revision;
		if (changed) {
			telemetry_meaning_store(&mps->telemetry_meaning, snapshot, future_known, future_media_ms);
			mps->telemetry_meaning_valid = true;
		}
		mps->telemetry_observed_discontinuity = discontinuity;
	}
	return mps->telemetry_revision;
}

/* Must be called with the playlist lock held. The pass snapshot does not carry
 * is_url, so resolve it from the configured entry. An unresolvable entry is
 * treated as a URL: its duration is never trusted from the file inventory. */
static bool telemetry_item_is_url_locked(const struct media_playlist_source *mps,
					 const struct mps_playlist_item_snapshot *item)
{
	const struct media_file_data *top;

	if (!item || item->media_index >= mps->files.num)
		return true;
	top = &mps->files.array[item->media_index];
	if (!item->is_folder_child)
		return top->is_url;
	if (!top->is_folder || item->folder_item_index >= top->folder_items.num)
		return true;
	return top->folder_items.array[item->folder_item_index].is_url;
}

/* Reads state that the active slot committed, never a request that is still
 * being prepared. Decoder clocks come from a retained reference to that same
 * child; the caller validates the generation after the read. */
static bool telemetry_read_once(struct media_playlist_source *mps, obs_source_t *source,
				struct mps_playlist_telemetry *snapshot, uint64_t *discontinuity_out,
				bool *future_known_out, int64_t *future_media_ms_out)
{
	struct mps_playlist_item_snapshot *remaining = NULL;
	size_t count = 0;
	bool pass_known = false;
	obs_source_t *clock_source = NULL;
	uint64_t generation = 0;
	uint64_t discontinuity;
	enum obs_media_state cached_state;
	enum obs_media_state clock_state = OBS_MEDIA_STATE_NONE;
	char *media_id = NULL;
	char *folder_filename = NULL;
	char *path = NULL;
	bool *future_url = NULL;
	bool is_url = false;
	bool resetting = false;
	bool cycle_exhausted = false;
	int64_t configured_speed;
	int64_t elapsed = -1;
	int64_t duration = -1;
	struct media_source_slot *slot;

	*discontinuity_out = 0;
	*future_known_out = false;
	*future_media_ms_out = -1;
	if (mps->source_uuid && *mps->source_uuid)
		snapshot->source_uuid = bstrdup(mps->source_uuid);
	const char *source_name = obs_source_get_name(source);
	if (source_name && *source_name)
		snapshot->source_name = bstrdup(source_name);

	pthread_mutex_lock(&mps->mutex);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	discontinuity = mps->telemetry_discontinuity;
	cached_state = mps->state;
	snapshot->shuffle = mps->shuffle;
	snapshot->loop = mps->loop;
	resetting = mps->resetting;
	slot = mps->active_slot;
	if (slot && slot->path && *slot->path) {
		path = bstrdup(slot->path);
		media_id = slot->media_id ? bstrdup(slot->media_id) : NULL;
		folder_filename = slot->folder_item_filename ? bstrdup(slot->folder_item_filename) : NULL;
		is_url = slot->is_url;
		if (slot->source && !slot->failed) {
			clock_source = obs_source_get_ref(slot->source);
			generation = slot->source_generation;
		}
		cycle_exhausted = mps->telemetry_cycle_exhausted &&
				  mps->telemetry_cycle_exhausted_generation == slot->source_generation;
	}
	configured_speed = mps->speed >= 1 && mps->speed <= 200 ? mps->speed : 100;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	struct media_file_data *current = path ? telemetry_current_locked(mps, media_id, folder_filename, path) : NULL;
	if (current && !resetting) {
		struct media_file_data *parent = current->parent ? current->parent : current;
		size_t folder_index = current->parent ? current->index : 0;
		snapshot->playlist_index = parent->index;
		snapshot->folder_item_index = current->parent ? current->index : SIZE_MAX;
		if (mps->shuffle && cycle_exhausted) {
			/* The active item ended its cycle and the next-item prefetch
			 * already rolled the queue into a new cycle; the current pass
			 * therefore has no remaining entries. */
			pass_known = true;
		} else {
			struct mps_playlist_context_input input = {
				.files = &mps->files.da,
				.shuffler = &mps->shuffler,
				.shuffle = mps->shuffle,
				.loop = mps->loop,
				.current_media = parent,
				.actual_media = current,
				.current_media_index = parent->index,
				.current_folder_item_index = folder_index,
			};
			/* A pending manual selection moves the shuffle cursor away
			 * from the committed item; the pass copy then reports unknown
			 * instead of guessing. */
			pass_known = telemetry_pass_bounded(mps) && mps_playlist_pass_copy(&input, &remaining, &count);
			if (pass_known && count > 0 && count <= MPS_TELEMETRY_MAX_DURATION_LOOKUPS) {
				future_url = bmalloc(count * sizeof(*future_url));
				if (future_url) {
					for (size_t i = 0; i < count; ++i)
						future_url[i] = telemetry_item_is_url_locked(mps, &remaining[i]);
				}
			}
		}
		snapshot->has_next = mps->shuffle ? shuffler_has_next(&mps->shuffler)
						  : mps_next_sequential_media(&mps->files.da, parent, parent->index,
									      folder_index, mps->loop) != NULL;
		snapshot->has_previous = mps->shuffle
						 ? shuffler_has_prev(&mps->shuffler)
						 : mps_previous_sequential_media(&mps->files.da, parent, parent->index,
										 folder_index, mps->loop) != NULL;
	}
	pthread_mutex_unlock(&mps->mutex);

	if (path) {
		snapshot->current_path = path;
		path = NULL;
		snapshot->current_item_id = telemetry_item_id(media_id, folder_filename);
		const char *current_file = telemetry_basename(snapshot->current_path);
		if (*current_file)
			snapshot->current_file = bstrdup(current_file);
		snapshot->speed_percent = is_url ? 100 : configured_speed;
	}
	bfree(media_id);
	bfree(folder_filename);

	if (clock_source) {
		elapsed = obs_source_media_get_time(clock_source);
		duration = obs_source_media_get_duration(clock_source);
		clock_state = obs_source_media_get_state(clock_source);
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	bool coherent = discontinuity == mps->telemetry_discontinuity &&
			(!clock_source || (mps->active_slot && mps->active_slot->source == clock_source &&
					   mps->active_slot->source_generation == generation));
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (clock_source)
		obs_source_release(clock_source);
	if (!coherent) {
		bfree(future_url);
		mps_playlist_entries_free(remaining, count);
		return false;
	}

	/* The child clock is authoritative: OBS freezes it while paused and a seek
	 * updates it immediately, so no wall-clock cache is layered on top. */
	snapshot->state = clock_state != OBS_MEDIA_STATE_NONE ? clock_state : cached_state;
	if (!clock_source &&
	    (snapshot->state == OBS_MEDIA_STATE_PLAYING || snapshot->state == OBS_MEDIA_STATE_OPENING ||
	     snapshot->state == OBS_MEDIA_STATE_BUFFERING))
		/* No committed decoder: do not present a playing clock. */
		snapshot->state = OBS_MEDIA_STATE_NONE;
	if (snapshot->current_path) {
		snapshot->elapsed_media_ms = elapsed >= 0 ? elapsed : -1;
		snapshot->duration_media_ms = duration > 0 ? duration : -1;
	}

	int64_t *durations = NULL;
	int64_t *future_speeds = NULL;
	/* Cache lookups do no I/O. Without resolved URL flags the durations
	 * cannot be trusted, even when an entry shares a cached local path. */
	bool lookup_durations = count > 0 && count <= MPS_TELEMETRY_MAX_DURATION_LOOKUPS && future_url != NULL;
	if (lookup_durations) {
		durations = bmalloc(count * sizeof(*durations));
		future_speeds = bmalloc(count * sizeof(*future_speeds));
		if (!durations || !future_speeds) {
			bfree(durations);
			bfree(future_speeds);
			durations = NULL;
			future_speeds = NULL;
			lookup_durations = false;
		}
	}
	if (lookup_durations) {
		for (size_t i = 0; i < count; ++i) {
			durations[i] = -1;
			future_speeds[i] = future_url[i] ? 100 : configured_speed;
			/* A URL or stream has no authoritative file duration even if a
			 * cache entry shares its path. */
			if (!future_url[i])
				mps_duration_inventory_get(mps->duration_inventory, remaining[i].path, &durations[i]);
		}
	}
	mps_playlist_telemetry_calculate_at_speeds(snapshot, lookup_durations ? durations : NULL,
						   lookup_durations ? future_speeds : NULL, count, pass_known);
	bfree(durations);
	bfree(future_speeds);
	bfree(future_url);
	mps_playlist_entries_free(remaining, count);

	/* The fingerprint uses the effective total of the entries after the current
	 * one, never the playlist total that also contains current-clock drift. It
	 * is derivable exactly when the pass and both totals are known. */
	if (snapshot->playlist_remaining_known && snapshot->remaining_ms >= 0) {
		*future_known_out = true;
		*future_media_ms_out = snapshot->playlist_remaining_ms - snapshot->remaining_ms;
	}
	*discontinuity_out = discontinuity;
	return true;
}

/* Caller owns a source reference. No filesystem access, duration probe or
 * shuffle draw happens in this query: the inventory is read from its cache and
 * the pass is copied read-only. */
bool mps_playlist_telemetry_get(obs_source_t *source, struct mps_playlist_telemetry *snapshot)
{
	if (!source || !snapshot)
		return false;
	mps_playlist_telemetry_init(snapshot);
	struct media_playlist_source *mps = mps_registry_lookup(source);
	if (!mps)
		return false;

	pthread_mutex_lock(&mps->telemetry_mutex);
	bool read = false;
	uint64_t discontinuity = 0;
	bool future_known = false;
	int64_t future_media_ms = -1;
	for (unsigned attempt = 0; attempt < MPS_TELEMETRY_READ_ATTEMPTS && !read; ++attempt) {
		if (attempt) {
			mps_playlist_telemetry_free(snapshot);
			mps_playlist_telemetry_init(snapshot);
		}
		read = telemetry_read_once(mps, source, snapshot, &discontinuity, &future_known, &future_media_ms);
	}
	if (read)
		snapshot->snapshot_revision =
			telemetry_observe(mps, snapshot, future_known, future_media_ms, discontinuity);
	else {
		/* Never publish a half-observed item: after a racing event the caller
		 * gets no snapshot rather than stale partial timing. */
		mps_playlist_telemetry_free(snapshot);
		mps_playlist_telemetry_init(snapshot);
	}
	pthread_mutex_unlock(&mps->telemetry_mutex);
	return read;
}

struct media_source_callback_context {
	struct media_source_slot *slot;
	uint64_t source_generation;
	bool ended_before_publish;
};

static inline void set_current_media_index(struct media_playlist_source *mps, size_t index)
{
	if (get_total_file_count(mps) > 0) {
		if (index >= mps->files.num) {
			index = 0;
		}
		mps->current_media_index = index;
		mps->current_media = &mps->files.array[index];
	} else {
		mps->current_media_index = 0;
		mps->current_media = NULL;
		mps->actual_media = NULL;
		mps->current_folder_item_index = 0;
		bfree(mps->current_media_filename);
		mps->current_media_filename = NULL;
	}
}

static size_t get_total_file_count(struct media_playlist_source *mps)
{
	size_t count = 0;
	struct media_file_data item;

	for (size_t i = 0; i < mps->files.num; i++) {
		item = mps->files.array[i];
		if (item.is_folder) {
			count += item.folder_items.num;
		} else {
			count += 1;
		}
	}

	return count;
}

/* Requires setting current media index first
 */
static inline void set_current_folder_item_index(struct media_playlist_source *mps, size_t index)
{
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
	if (mps->current_media) {
		if (!mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;
			mps->actual_media = mps->current_media;
			return;
		}

		if (index < mps->current_media->folder_items.num) {
			mps->current_folder_item_index = index;
		} else {
			mps->current_folder_item_index = index = 0;
		}
		mps->actual_media = &mps->current_media->folder_items.array[index];
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = NULL;
	}
}

static inline void reset_folder_item_index(struct media_playlist_source *mps)
{
	mps->current_folder_item_index = 0;
	bfree(mps->current_media_filename);
	mps->current_media_filename = NULL;
}

static bool valid_extension(const char *ext)
{
	struct dstr haystack = {0};
	struct dstr needle = {0};
	bool valid = false;

	if (!ext || !*ext)
		return false;

	dstr_copy(&haystack, media_filter);
	dstr_cat(&haystack, video_filter);
	dstr_cat(&haystack, audio_filter);

	dstr_cat_ch(&needle, '*');
	dstr_cat(&needle, ext);

	valid = dstr_find_i(&haystack, needle.array);

	dstr_free(&haystack);
	dstr_free(&needle);
	return valid;
}

static bool is_local_video(const struct media_file_data *media)
{
	const char *extension;
	struct dstr haystack = {0};
	struct dstr needle = {0};
	bool is_video;

	if (!media || media->is_url)
		return false;

	extension = os_get_path_extension(media->path);
	if (!extension || !*extension)
		return false;

	dstr_cat_ch(&needle, '*');
	dstr_cat(&needle, extension);
	dstr_copy(&haystack, video_filter);
	is_video = dstr_find_i(&haystack, needle.array);
	dstr_free(&haystack);
	dstr_free(&needle);
	return is_video;
}

static void clear_audio_queues_locked(struct media_playlist_source *mps)
{
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		deque_free(&mps->audio_data[i]);
	deque_free(&mps->audio_frames);
	deque_free(&mps->audio_timestamps);
}

static void mute_active_audio(struct media_playlist_source *mps)
{
	pthread_mutex_lock(&mps->lifecycle_mutex);
	pthread_mutex_lock(&mps->audio_mutex);
	if (mps->active_slot)
		mps->active_slot->audio_enabled = false;
	clear_audio_queues_locked(mps);
	pthread_mutex_unlock(&mps->audio_mutex);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

static obs_source_t *detach_active_source(struct media_playlist_source *mps)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	pthread_mutex_lock(&mps->audio_mutex);
	if (mps->active_slot && mps->active_slot->source) {
		source = obs_source_get_ref(mps->active_slot->source);
		mps->active_slot->expected_stop = true;
		mps->active_slot->audio_enabled = false;
		mps->active_slot = NULL;
	}
	clear_audio_queues_locked(mps);
	pthread_mutex_unlock(&mps->audio_mutex);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return source;
}

static obs_source_t *get_active_source_ref(struct media_playlist_source *mps)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->active_slot && mps->active_slot->source)
		source = obs_source_get_ref(mps->active_slot->source);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return source;
}

static obs_source_t *get_slot_source_ref(struct media_playlist_source *mps, struct media_source_slot *slot)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (slot && slot->source)
		source = obs_source_get_ref(slot->source);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return source;
}

static void destroy_media_slot(struct media_source_slot *slot)
{
	struct media_playlist_source *mps = slot->owner;
	struct media_source_callback_context *callback_context;
	obs_source_t *source;
	char *path;
	char *media_id;
	char *folder_item_filename;
	bool child_added;
	bool manual_showing;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	source = slot->source;
	callback_context = slot->callback_context;
	path = slot->path;
	media_id = slot->media_id;
	folder_item_filename = slot->folder_item_filename;
	child_added = slot->child_added;
	manual_showing = slot->manual_showing;
	slot->source = NULL;
	slot->callback_context = NULL;
	slot->path = NULL;
	slot->media_id = NULL;
	slot->folder_item_filename = NULL;
	slot->request_generation = 0;
	slot->failed = false;
	slot->ended = false;
	slot->ended_handled = false;
	slot->expected_stop = true;
	slot->produced_output = false;
	slot->audio_enabled = false;
	slot->is_url = false;
	slot->child_added = false;
	slot->manual_showing = false;
	slot->start_requested = false;
	slot->started = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (source) {
		obs_source_remove_audio_capture_callback(source, mps_audio_callback, callback_context);
		signal_handler_disconnect(obs_source_get_signal_handler(source), "media_ended", media_source_ended,
					  callback_context);
		signal_handler_disconnect(obs_source_get_signal_handler(source), "media_started", media_source_started,
					  callback_context);
		if (manual_showing)
			obs_source_dec_showing(source);
		if (child_added)
			obs_source_remove_active_child(mps->source, source);
		obs_source_release(source);
	}
	bfree(callback_context);

	bfree(path);
	bfree(media_id);
	bfree(folder_item_filename);
}

static bool cancel_pending_transition(struct media_playlist_source *mps, bool discard_prefetch,
				      bool require_active_source)
{
	struct media_source_slot *standby = NULL;
	bool pending;
	bool request_pending;
	bool active_source_present = false;
	uint64_t pending_generation = 0;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	request_pending = mps->coordinator.request_pending;
	if (require_active_source) {
		active_source_present = mps->active_slot && mps->active_slot->source;
		pending_generation = mps->coordinator.generation;
		pending = mps_coordinator_cancel_for_restart(&mps->coordinator, active_source_present);
	} else {
		pending = request_pending;
		if (pending)
			mps_coordinator_cancel(&mps->coordinator);
	}
	if (pending || discard_prefetch)
		standby = mps->standby_slot;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (standby)
		destroy_media_slot(standby);
	if (require_active_source && request_pending) {
		obs_log(LOG_DEBUG, "Restart Current: pending transition generation %llu %s; active source=%s%s",
			(unsigned long long)pending_generation, pending ? "cancelled" : "preserved",
			active_source_present ? "present" : "absent",
			active_source_present ? "" : " (bootstrap preserved)");
	}
	return pending;
}

static void restore_active_playlist_position(struct media_playlist_source *mps)
{
	size_t media_index;
	size_t folder_item_index;
	bool folder_item;
	bool restore = false;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->active_slot && mps->active_slot->source) {
		media_index = mps->active_slot->media_index;
		folder_item_index = mps->active_slot->folder_item_index;
		folder_item = mps->active_slot->folder_item;
		restore = true;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!restore)
		return;

	pthread_mutex_lock(&mps->mutex);
	set_current_media_index(mps, media_index);
	set_current_folder_item_index(mps, folder_item ? folder_item_index : 0);
	pthread_mutex_unlock(&mps->mutex);
}

static void rebind_active_slot_metadata(struct media_playlist_source *mps)
{
	struct media_source_slot *slot;
	char *media_id = NULL;
	char *folder_item_filename = NULL;
	char *path = NULL;
	uint64_t source_generation = 0;
	size_t media_index = 0;
	size_t folder_item_index = 0;
	bool folder_item = false;
	bool is_url = false;
	bool found = false;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	slot = mps->active_slot;
	if (slot && slot->source && slot->media_id && slot->path) {
		media_id = bstrdup(slot->media_id);
		if (slot->folder_item_filename)
			folder_item_filename = bstrdup(slot->folder_item_filename);
		path = bstrdup(slot->path);
		source_generation = slot->source_generation;
		folder_item = slot->folder_item;
		is_url = slot->is_url;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!media_id || !path)
		goto cleanup;

	pthread_mutex_lock(&mps->mutex);
	for (size_t i = 0; i < mps->files.num; i++) {
		struct media_file_data *media = &mps->files.array[i];
		if (strcmp(media->id, media_id) != 0)
			continue;
		if (!folder_item) {
			if (!media->is_folder && media->is_url == is_url && strcmp(media->path, path) == 0) {
				media_index = i;
				folder_item_index = 0;
				found = true;
			}
			break;
		}
		if (!media->is_folder || !folder_item_filename)
			break;
		for (size_t j = 0; j < media->folder_items.num; j++) {
			struct media_file_data *item = &media->folder_items.array[j];
			if (item->is_url == is_url && strcmp(item->filename, folder_item_filename) == 0 &&
			    strcmp(item->path, path) == 0) {
				media_index = i;
				folder_item_index = j;
				found = true;
				break;
			}
		}
		break;
	}
	pthread_mutex_unlock(&mps->mutex);

	if (found) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		if (mps->active_slot == slot && slot->source && slot->source_generation == source_generation) {
			slot->media_index = media_index;
			slot->folder_item_index = folder_item_index;
			slot->folder_item = folder_item;
		}
		pthread_mutex_unlock(&mps->lifecycle_mutex);
	}

cleanup:
	bfree(media_id);
	bfree(folder_item_filename);
	bfree(path);
}

static obs_data_t *create_media_settings(struct media_playlist_source *mps, const struct media_file_data *media,
					 bool local_video)
{
	obs_data_t *settings = obs_data_create();
	const char *path_setting = media->is_url ? S_FFMPEG_INPUT : S_FFMPEG_LOCAL_FILE;
	bool restart_on_activate = local_video || (mps->visibility_behavior != VISIBILITY_BEHAVIOR_ALWAYS_PLAY &&
						   mps->visibility_behavior != VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE);

	obs_data_set_bool(settings, "log_changes", false);
	obs_data_set_bool(settings, S_FFMPEG_IS_LOCAL_FILE, !media->is_url);
	obs_data_set_string(settings, path_setting, media->path);
	obs_data_set_bool(settings, S_FFMPEG_HW_DECODE, mps->use_hw_decoding);
	obs_data_set_bool(settings, S_FFMPEG_CLOSE_WHEN_INACTIVE, local_video ? false : mps->close_when_inactive);
	obs_data_set_bool(settings, S_FFMPEG_RESTART_ON_ACTIVATE, restart_on_activate);
	obs_data_set_bool(settings, S_FFMPEG_CLEAR_ON_MEDIA_END, true);
	obs_data_set_bool(settings, S_FFMPEG_IS_STINGER, local_video);
	obs_data_set_int(settings, S_SPEED, mps->speed);
	return settings;
}

static bool create_media_slot(struct media_playlist_source *mps, struct media_source_slot *slot,
			      const struct media_file_data *media, uint64_t request_generation)
{
	obs_data_t *settings;
	obs_source_t *source;
	struct media_source_callback_context *callback_context;
	char *path;
	uint64_t source_generation;
	bool publish;
	bool local_video = is_local_video(media);

	destroy_media_slot(slot);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->destroying || mps->resetting || slot != mps->standby_slot) {
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		return false;
	}
	source_generation = ++mps->next_source_generation;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	settings = create_media_settings(mps, media, local_video);
	source = obs_source_create_private("ffmpeg_source", "media_playlist_slot", settings);
	obs_data_release(settings);
	if (!source)
		return false;
	/* The private source is a decoder; its audio is re-emitted by the parent. */
	obs_source_set_muted(source, true);
	callback_context = bzalloc(sizeof(*callback_context));
	path = bstrdup(media->path);
	callback_context->slot = slot;
	callback_context->source_generation = source_generation;

	obs_source_add_audio_capture_callback(source, mps_audio_callback, callback_context);
	signal_handler_connect(obs_source_get_signal_handler(source), "media_ended", media_source_ended,
			       callback_context);
	signal_handler_connect(obs_source_get_signal_handler(source), "media_started", media_source_started,
			       callback_context);

	if (local_video) {
		calldata_t cd = {0};
		proc_handler_call(obs_source_get_proc_handler(source), "preload_first_frame", &cd);
		calldata_free(&cd);
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	publish = !mps->destroying && !mps->resetting && slot == mps->standby_slot &&
		  (request_generation ? mps_coordinator_matches(&mps->coordinator, request_generation)
				      : !mps->coordinator.request_pending);
	if (publish) {
		slot->source = source;
		slot->callback_context = callback_context;
		slot->path = path;
		slot->source_generation = source_generation;
		slot->request_generation = request_generation;
		slot->is_url = media->is_url;
		slot->local_video = local_video;
		slot->child_added = false;
		slot->manual_showing = false;
		slot->start_requested = false;
		slot->started = false;
		slot->failed = callback_context->ended_before_publish;
		slot->ended = false;
		slot->ended_handled = false;
		slot->expected_stop = false;
		slot->produced_output = false;
		slot->audio_enabled = false;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!publish) {
		obs_source_remove_audio_capture_callback(source, mps_audio_callback, callback_context);
		signal_handler_disconnect(obs_source_get_signal_handler(source), "media_ended", media_source_ended,
					  callback_context);
		signal_handler_disconnect(obs_source_get_signal_handler(source), "media_started", media_source_started,
					  callback_context);
		obs_source_release(source);
		bfree(callback_context);
		bfree(path);
	}
	return publish;
}

static void configure_active_media_source(struct media_playlist_source *mps, obs_source_t *source)
{
	obs_data_t *settings = obs_source_get_settings(source);
	bool restart_on_activate = mps->visibility_behavior != VISIBILITY_BEHAVIOR_ALWAYS_PLAY &&
				   mps->visibility_behavior != VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE;

	obs_data_set_bool(settings, S_FFMPEG_CLEAR_ON_MEDIA_END, false);
	obs_data_set_bool(settings, S_FFMPEG_CLOSE_WHEN_INACTIVE, mps->close_when_inactive);
	obs_data_set_bool(settings, S_FFMPEG_RESTART_ON_ACTIVATE, restart_on_activate);
	obs_source_update(source, settings);
	obs_data_release(settings);
}

static void disable_restart_on_activate(obs_source_t *source)
{
	obs_data_t *settings = obs_source_get_settings(source);

	obs_data_set_bool(settings, S_FFMPEG_RESTART_ON_ACTIVATE, false);
	obs_source_update(source, settings);
	obs_data_release(settings);
}

static void update_current_filename_setting(struct media_playlist_source *mps, obs_data_t *data)
{
	struct dstr long_desc = {0};
	if (!mps || !data)
		return;
	if (!mps->actual_media) {
		obs_data_set_string(data, S_CURRENT_FILE_NAME, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_data_set_string(data, S_CURRENT_FILE_NAME, long_desc.array);
	dstr_free(&long_desc);
}

/* Empties path selected in media source,
 * for when there is no item in the list
 */
static void clear_media_source(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *active_source;
	bool was_resetting;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	was_resetting = mps->resetting;
	mps->resetting = true;
	mps_coordinator_cancel(&mps->coordinator);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	active_source = detach_active_source(mps);

	if (active_source) {
		obs_source_release(active_source);
	}

	destroy_media_slot(&mps->media_slots[0]);
	destroy_media_slot(&mps->media_slots[1]);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->active_slot = NULL;
	mps->standby_slot = &mps->media_slots[0];
	mps->resetting = was_resetting;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	obs_source_media_stop(mps->source);
}

/* Checks if the media source has to be updated, because updating its
 * settings causes it to restart. Can also force update it.
 * Should first call set_current_media_index before calling this
 *
 * Forced updates:
 * * Using play_folder_item_at_index
 * * Using play_media_at_index
 * * during mps_update (files/folders can be changed)
 */
static void update_media_source(void *data, bool forced)
{
	struct media_playlist_source *mps = data;

	if (!forced || !mps->current_media)
		return;

	if (mps->current_media->is_folder) {
		assert(mps->current_folder_item_index < mps->current_media->folder_items.num);
		mps->actual_media = &mps->current_media->folder_items.array[mps->current_folder_item_index];
	} else {
		mps->current_folder_item_index = 0;
		mps->actual_media = mps->current_media;
	}
}

static void request_media_switch(struct media_playlist_source *mps, enum mps_switch_request request,
				 bool preserve_failures)
{
	size_t candidate_limit = get_total_file_count(mps);
	if (!candidate_limit)
		return;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps_coordinator_request(&mps->coordinator, request, candidate_limit, preserve_failures);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

static void select_index_proc_(struct media_playlist_source *mps, size_t media_index, size_t folder_item_index)
{
	if (media_index < mps->files.num) {
		invalidate_logical_next_path(mps);
		obs_source_t *active_source = get_active_source_ref(mps);
		if (active_source) {
			obs_source_media_play_pause(active_source, true);
			obs_source_release(active_source);
		}
		pthread_mutex_lock(&mps->mutex);
		set_current_media_index(mps, media_index);
		set_current_folder_item_index(mps, folder_item_index);
		if (mps->actual_media) {
			update_media_source(mps, true);
			if (mps->shuffle) {
				invalidate_shuffle_cycle_exhausted(mps);
				shuffler_select(&mps->shuffler, mps->actual_media);
			}
			pthread_mutex_lock(&mps->lifecycle_mutex);
			mps_coordinator_request(&mps->coordinator, MPS_SWITCH_SELECT, 1, false);
			pthread_mutex_unlock(&mps->lifecycle_mutex);
		}
		pthread_mutex_unlock(&mps->mutex);
		mps_playlist_change_notify(mps);
	}
}

static bool media_has_stable_id(const struct media_file_data *media, const char *stable_id)
{
	if (media->id)
		return strcmp(media->id, stable_id) == 0;
	if (!media->parent_id || !media->filename)
		return false;

	size_t parent_length = strlen(media->parent_id);
	return strncmp(stable_id, media->parent_id, parent_length) == 0 && stable_id[parent_length] == '/' &&
	       strcmp(stable_id + parent_length + 1, media->filename) == 0;
}

bool mps_playlist_select_item(obs_source_t *source, const char *stable_id, uint64_t expected_revision)
{
	if (!source || !stable_id || !*stable_id || obs_source_removed(source))
		return false;
	struct media_playlist_source *mps = mps_registry_lookup(source);
	if (!mps)
		return false;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	bool unavailable = mps->destroying || mps->resetting;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (unavailable)
		return false;

	size_t media_index = 0;
	size_t folder_item_index = 0;
	size_t matches = 0;
	bool selectable = false;
	pthread_mutex_lock(&mps->mutex);
	if (expected_revision && expected_revision != mps->playlist_revision) {
		pthread_mutex_unlock(&mps->mutex);
		return false;
	}
	for (size_t i = 0; i < mps->files.num; i++) {
		const struct media_file_data *media = &mps->files.array[i];
		if (media_has_stable_id(media, stable_id)) {
			media_index = i;
			folder_item_index = 0;
			selectable = !media->is_folder;
			++matches;
		}
		for (size_t j = 0; media->is_folder && j < media->folder_items.num; j++) {
			if (media_has_stable_id(&media->folder_items.array[j], stable_id)) {
				media_index = i;
				folder_item_index = j;
				selectable = true;
				++matches;
			}
		}
	}
	pthread_mutex_unlock(&mps->mutex);
	if (matches != 1 || !selectable || obs_source_removed(source))
		return false;

	/* The graphics thread serializes this lookup and dispatch with playlist updates. */
	select_index_proc_(mps, media_index, folder_item_index);
	return true;
}

static void select_index_proc(void *data, calldata_t *cd)
{
	struct media_playlist_source *mps = data;
	long long media_index = 0;
	long long folder_item_index = 0;
	calldata_get_int(cd, "media_index", &media_index);
	calldata_get_int(cd, "folder_item_index", &folder_item_index);
	select_index_proc_(mps, media_index, folder_item_index);
}

static void play_folder_item_at_index(void *data, size_t index)
{
	struct media_playlist_source *mps = data;
	if (mps->current_media->is_folder && index < mps->current_media->folder_items.num) {
		mps->current_folder_item_index = index;
		mps->actual_media = &mps->current_media->folder_items.array[index];
		bfree(mps->current_media_filename);
		mps->current_media_filename = bstrdup(mps->actual_media->filename);
		update_media_source(mps, true);
		obs_source_save(mps->source);
	}
}

static void play_media_at_index(void *data, size_t index, bool play_last_folder_item)
{
	struct media_playlist_source *mps = data;
	set_current_media_index(mps, index);
	if (index >= mps->files.num || !mps->actual_media) {
		clear_media_source(mps);
		return;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_media->folder_items.num) {
			// when Previous Item is clicked to go back to a folder
			if (play_last_folder_item) {
				mps->current_folder_item_index = mps->current_media->folder_items.num - 1;
			} else {
				mps->current_folder_item_index = 0;
			}

			play_folder_item_at_index(mps, mps->current_folder_item_index);
		} else {
			if (play_last_folder_item) {
				obs_source_media_previous(mps->source);
			} else {
				obs_source_media_next(mps->source);
			}
		}
		return;
	}
	update_media_source(mps, true);
	obs_source_save(mps->source);
}

static size_t find_folder_item_index(struct darray *array, const char *filename)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		if (strcmp(files.array[i].filename, filename) == 0) {
			return i;
		}
	}

	return DARRAY_INVALID;
}

/* ------------------------------------------------------------------------- */

static void set_media_state(void *data, enum obs_media_state state)
{
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	bool changed = mps->state != state;
	mps->state = state;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (changed)
		mps_playlist_change_notify(mps);
}

static enum obs_media_state get_media_state(struct media_playlist_source *mps)
{
	enum obs_media_state state;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	state = mps->state;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return state;
}

static enum obs_media_state mps_get_state(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	enum obs_media_state media_state = source ? obs_source_media_get_state(source) : OBS_MEDIA_STATE_NONE;
	if (source)
		obs_source_release(source);
	return media_state;
}

static void mps_end_reached(void *data)
{
	struct media_playlist_source *mps = data;
	set_media_state(mps, OBS_MEDIA_STATE_ENDED);
	obs_source_media_ended(mps->source);
	invalidate_logical_next_path(mps);
	pthread_mutex_lock(&mps->mutex);
	set_current_media_index(mps, 0);
	pthread_mutex_unlock(&mps->mutex);
	obs_source_save(mps->source);
	mps_playlist_change_notify(mps);
}

static void media_source_ended(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct media_source_callback_context *context = data;
	struct media_source_slot *slot = context->slot;
	struct media_playlist_source *mps = slot->owner;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (slot->source && slot->source_generation == context->source_generation && !slot->expected_stop) {
		if (slot == mps->active_slot)
			slot->ended = true;
		else
			slot->failed = true;
	} else if (!slot->source) {
		context->ended_before_publish = true;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

static void media_source_started(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct media_source_callback_context *context = data;
	struct media_source_slot *slot = context->slot;
	struct media_playlist_source *mps = slot->owner;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (slot->source && slot->source_generation == context->source_generation)
		slot->started = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
}

void mps_audio_callback(void *data, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	struct media_source_callback_context *context = data;
	struct media_source_slot *slot = context->slot;
	struct media_playlist_source *mps = slot->owner;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (slot != mps->active_slot || source != slot->source || !slot->audio_enabled || slot->expected_stop ||
	    context->source_generation != slot->source_generation) {
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		return;
	}
	pthread_mutex_lock(&mps->audio_mutex);
	slot->produced_output = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	size_t size = audio_data->frames * sizeof(float);
	for (size_t i = 0; i < mps->num_channels; i++) {
		deque_push_back(&mps->audio_data[i], audio_data->data[i], size);
	}
	deque_push_back(&mps->audio_frames, &audio_data->frames, sizeof(audio_data->frames));
	deque_push_back(&mps->audio_timestamps, &audio_data->timestamp, sizeof(audio_data->timestamp));
	pthread_mutex_unlock(&mps->audio_mutex);
}

static bool play_selected_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	signal_handler_t *sh = obs_source_get_signal_handler(mps->source);
	obs_data_t *settings = obs_source_get_settings(mps->source);
	size_t media_index = 0;
	size_t folder_item_index = 0;
	const char *index_str = obs_data_get_string(settings, S_SELECT_FILE);
	if (strcmp(index_str, "0") != 0) {
		char **indexes = strlist_split(index_str, '-', false);

		media_index = strtol(indexes[0], NULL, 10) - 1;
		if (indexes[1]) {
			folder_item_index = strtol(indexes[1], NULL, 10) - 1;
		}
		select_index_proc_(mps, media_index, folder_item_index);

		strlist_free(indexes);
	}
	obs_data_release(settings);
	/*update_current_filename_property(
		mps, obs_properties_get(props, S_CURRENT_FILE_NAME));*/
	update_current_filename_setting(mps, settings);
	//obs_source_update_properties(mps->source);

	signal_handler_signal(sh, "media_next", NULL);
	return true;
}

static bool refresh_filename_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	update_current_filename_setting(mps, settings);
	obs_source_update_properties(mps->source);
	obs_data_release(settings);
	return true;
}

/* ------------------------------------------------------------------------- */

static const char *mps_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("MediaPlaylistSource");
}

static void free_files(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;

	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *file = &files.array[i];
		bfree(file->filename);
		bfree(file->path);
		bfree(file->id);
		if (file->is_folder) {
			for (size_t j = 0; j < file->folder_items.num; j++) {
				struct media_file_data *folder_item = &file->folder_items.array[j];
				bfree(folder_item->filename);
				bfree(folder_item->path);
				bfree(folder_item->id);
			}
		}
		da_free(file->folder_items);
	}

	da_free(files);
}

static int64_t mps_get_duration(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	int64_t duration = source ? obs_source_media_get_duration(source) : 0;
	if (source)
		obs_source_release(source);
	return duration;
}

static int64_t mps_get_time(void *data)
{
	struct media_playlist_source *mps = data;

	obs_source_t *source = get_active_source_ref(mps);
	int64_t time_ms = source ? obs_source_media_get_time(source) : 0;
	if (source)
		obs_source_release(source);
	return time_ms;
}

static void mps_set_time(void *data, int64_t ms)
{
	struct media_playlist_source *mps = data;

	obs_source_t *source = get_active_source_ref(mps);
	if (!source)
		return;
	obs_source_media_set_time(source, ms);
	obs_source_release(source);
	/* OBS queues the child action. Notify now, then the existing lifecycle
	 * tick also observes its applied state/paused-clock discontinuity. */
	mps_playlist_change_notify(mps);
}

static void play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_play_pause(mps->source, !mps->paused);
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_restart(mps->source);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_stop(mps->source);
}

static void next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_next(mps->source);
}

static void previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct media_playlist_source *mps = data;

	if (pressed && obs_source_showing(mps->source))
		obs_source_media_previous(mps->source);
}

static void mps_play_pause(void *data, bool pause)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);

	if (source) {
		if (!pause) {
			pthread_mutex_lock(&mps->lifecycle_mutex);
			if (mps->active_slot) {
				mps->active_slot->expected_stop = false;
				mps->active_slot->audio_enabled = true;
			}
			pthread_mutex_unlock(&mps->lifecycle_mutex);
		}
		obs_source_media_play_pause(source, pause);
		obs_source_release(source);
	}
	mps->paused = pause;

	if (pause)
		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	else
		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
}

static void mps_restart(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source;
	bool restart_current = mps->restart_behavior == RESTART_BEHAVIOR_CURRENT_FILE;

	if (restart_current && cancel_pending_transition(mps, false, true))
		restore_active_playlist_position(mps);
	invalidate_logical_next_path(mps);

	if (mps->restart_behavior == RESTART_BEHAVIOR_FIRST_FILE) {
		bool clear_source;
		bool empty_folder;

		pthread_mutex_lock(&mps->mutex);
		set_current_media_index(mps, 0);
		clear_source = !mps->current_media || !mps->actual_media;
		empty_folder = !clear_source && mps->current_media->is_folder && !mps->current_media->folder_items.num;
		if (!clear_source && !empty_folder) {
			if (mps->current_media->is_folder)
				set_current_folder_item_index(mps, 0);
			else
				update_media_source(mps, true);
			if (mps->shuffle) {
				invalidate_shuffle_cycle_exhausted(mps);
				shuffler_select(&mps->shuffler, mps->actual_media);
			}
		}
		pthread_mutex_unlock(&mps->mutex);
		if (clear_source)
			clear_media_source(mps);
		else if (empty_folder)
			obs_source_media_next(mps->source);
		else
			obs_source_save(mps->source);
		request_media_switch(mps, MPS_SWITCH_NEXT, false);
	} else if (mps->restart_behavior == RESTART_BEHAVIOR_CURRENT_FILE) {
		if (get_media_state(mps) == OBS_MEDIA_STATE_ENDED) {
			pthread_mutex_lock(&mps->mutex);
			update_media_source(mps, true);
			pthread_mutex_unlock(&mps->mutex);
			request_media_switch(mps, MPS_SWITCH_NEXT, false);
			mps_playlist_change_notify(mps);
			return;
		}
		source = get_active_source_ref(mps);
		if (source) {
			pthread_mutex_lock(&mps->lifecycle_mutex);
			if (mps->active_slot) {
				mps->active_slot->expected_stop = false;
				mps->active_slot->audio_enabled = true;
			}
			pthread_mutex_unlock(&mps->lifecycle_mutex);
			obs_source_media_restart(source);
			obs_source_show_preloaded_video(source);
			obs_source_release(source);
		}
		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
	}
	prepare_next_media(mps);
	mps_playlist_change_notify(mps);
}

static void mps_stop(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source;

	if (cancel_pending_transition(mps, false, false))
		restore_active_playlist_position(mps);
	invalidate_logical_next_path(mps);
	source = get_active_source_ref(mps);

	pthread_mutex_lock(&mps->lifecycle_mutex);
	pthread_mutex_lock(&mps->audio_mutex);
	if (mps->active_slot) {
		mps->active_slot->expected_stop = true;
		mps->active_slot->audio_enabled = false;
	}
	clear_audio_queues_locked(mps);
	pthread_mutex_unlock(&mps->audio_mutex);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (source) {
		obs_source_media_stop(source);
		obs_source_release(source);
	}
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
	mps_playlist_change_notify(mps);
}

static void mps_playlist_next(void *data)
{
	struct media_playlist_source *mps = data;
	bool last_folder_item_reached = false;
	bool switched = false;
	bool next_invalidated;
	bool automatic;
	bool retrying;

	pthread_mutex_lock(&mps->mutex);
	next_invalidated = clear_logical_next_path_locked(mps);
	pthread_mutex_unlock(&mps->mutex);
	if (!get_total_file_count(mps))
		goto notify;
	if (!playlist_has_next(mps))
		goto notify;
	invalidate_shuffle_cycle_exhausted(mps);

	pthread_mutex_lock(&mps->lifecycle_mutex);
	automatic = mps->automatic_next_pending;
	retrying = mps->retrying_switch;
	mps->automatic_next_pending = false;
	mps->retrying_switch = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (!automatic && !retrying) {
		obs_source_t *active_source = get_active_source_ref(mps);
		if (active_source) {
			obs_source_media_play_pause(active_source, true);
			obs_source_release(active_source);
		}

		mute_active_audio(mps);
	}

	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		if (shuffler_has_next(&mps->shuffler)) {
			mps->actual_media = shuffler_next(&mps->shuffler);
			bfree(mps->current_media_filename);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_media_filename = bstrdup(mps->actual_media->filename);
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
				mps->current_media_filename = NULL;
				mps->current_folder_item_index = 0;
			}
			mps->current_media_index = mps->current_media->index;
			update_media_source(mps, true);
			obs_source_save(mps->source);
			switched = true;
		}
		goto end;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_media->folder_items.num > 0 &&
		    mps->current_folder_item_index < mps->current_media->folder_items.num - 1) {
			++mps->current_folder_item_index;
			play_folder_item_at_index(mps, mps->current_folder_item_index);
			switched = true;
			goto end;
		} else {
			last_folder_item_reached = true;
			mps->current_folder_item_index = 0;
		}
	}

	if (!mps->current_media->is_folder || last_folder_item_reached) {
		if (mps->current_media_index < mps->files.num - 1) {
			++mps->current_media_index;
		} else if (mps->loop) {
			mps->current_media_index = 0;
		} else {
			goto end;
		}
		play_media_at_index(mps, mps->current_media_index, false);
		switched = true;
	}

end:
	if (switched) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps_coordinator_request(&mps->coordinator, MPS_SWITCH_NEXT, get_total_file_count(mps), retrying);
		pthread_mutex_unlock(&mps->lifecycle_mutex);
	}
	pthread_mutex_unlock(&mps->mutex);

notify:
	if (switched || next_invalidated)
		mps_playlist_change_notify(mps);
	return;
}

static void mps_playlist_prev(void *data)
{
	struct media_playlist_source *mps = data;
	bool is_first_folder_item = false;
	bool switched = false;
	bool next_invalidated;
	bool retrying;

	pthread_mutex_lock(&mps->mutex);
	next_invalidated = clear_logical_next_path_locked(mps);
	pthread_mutex_unlock(&mps->mutex);
	if (!get_total_file_count(mps))
		goto notify;
	if (!playlist_has_previous(mps))
		goto notify;
	invalidate_shuffle_cycle_exhausted(mps);

	pthread_mutex_lock(&mps->lifecycle_mutex);
	retrying = mps->retrying_switch;
	mps->retrying_switch = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (!retrying) {
		obs_source_t *active_source = get_active_source_ref(mps);
		if (active_source) {
			obs_source_media_play_pause(active_source, true);
			obs_source_release(active_source);
		}

		mute_active_audio(mps);
	}

	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		if (shuffler_has_prev(&mps->shuffler)) {
			mps->actual_media = shuffler_prev(&mps->shuffler);
			bfree(mps->current_media_filename);
			if (mps->actual_media->parent_id) {
				mps->current_media = mps->actual_media->parent;
				mps->current_media_filename = bstrdup(mps->actual_media->filename);
				mps->current_folder_item_index = mps->actual_media->index;
			} else {
				mps->current_media = mps->actual_media;
				mps->current_media_filename = NULL;
				mps->current_folder_item_index = 0;
			}
			mps->current_media_index = mps->current_media->index;
			update_media_source(mps, true);
			obs_source_save(mps->source);
			switched = true;
		}
		goto end;
	}

	if (mps->current_media->is_folder) {
		if (mps->current_folder_item_index > 0) {
			--mps->current_folder_item_index;
			play_folder_item_at_index(mps, mps->current_folder_item_index);
			switched = true;
			goto end;
		} else {
			is_first_folder_item = true;
		}
	}

	if (!mps->current_media->is_folder || is_first_folder_item) {
		if (mps->current_media_index > 0) {
			--mps->current_media_index;
		} else if (mps->loop) {
			mps->current_media_index = mps->files.num - 1;
		} else {
			goto end;
		}
		play_media_at_index(mps, mps->current_media_index, is_first_folder_item);
		switched = true;
	}

end:
	if (switched) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps_coordinator_request(&mps->coordinator, MPS_SWITCH_PREVIOUS, get_total_file_count(mps), retrying);
		pthread_mutex_unlock(&mps->lifecycle_mutex);
	}
	pthread_mutex_unlock(&mps->mutex);

notify:
	if (switched || next_invalidated)
		mps_playlist_change_notify(mps);
}

/* Runs on the video lifecycle thread, before processing slot events. */
static void activate_fresh_shuffle(struct media_playlist_source *mps)
{
	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (!mps->fresh_shuffle_pending || !mps->source_active || mps->resetting || mps->destroying) {
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		return;
	}
	mps->fresh_shuffle_pending = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	pthread_mutex_lock(&mps->mutex);
	bool fresh = mps->shuffle && mps->shuffle_reset_behavior == SHUFFLE_RESET_ON_ACTIVATE &&
		     mps->shuffler.shuffled_files.num > 0;
	pthread_mutex_unlock(&mps->mutex);
	if (!fresh)
		return;

	/* Do not enqueue a parent Stop: it would cancel the new request next tick. */
	mps_stop(mps);
	cancel_pending_transition(mps, true, false);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->automatic_next_pending = false;
	mps->retrying_switch = false;
	if (mps->active_slot) {
		mps->active_slot->ended = false;
		mps->active_slot->ended_handled = true;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	pthread_mutex_lock(&mps->mutex);
	struct media_file_data *avoid = NULL;
	if (mps->activation_first_id) {
		struct media_file_data previous = {
			.id = mps->activation_first_id,
			.parent_id = mps->activation_first_filename ? mps->activation_first_id : NULL,
			.filename = mps->activation_first_filename,
		};
		size_t index = find_media_index(&mps->shuffler.shuffled_files.da, &previous, 0);
		if (index != DARRAY_INVALID)
			avoid = mps->shuffler.shuffled_files.array[index];
	}
	shuffler_reshuffle_avoid(&mps->shuffler, avoid);
	invalidate_shuffle_cycle_exhausted(mps);
	pthread_mutex_unlock(&mps->mutex);

	/* Consume exactly once, using the normal folder/index and switch handling. */
	mps_playlist_next(mps);
	pthread_mutex_lock(&mps->mutex);
	bfree(mps->activation_first_id);
	bfree(mps->activation_first_filename);
	mps->activation_first_id = NULL;
	mps->activation_first_filename = NULL;
	if (mps->actual_media) {
		struct media_file_data *first = mps->actual_media;
		mps->activation_first_id = bstrdup(first->parent_id ? first->parent_id : first->id);
		if (first->parent_id)
			mps->activation_first_filename = bstrdup(first->filename);
	}
	pthread_mutex_unlock(&mps->mutex);
}

static void mps_activate(void *data)
{
	struct media_playlist_source *mps = data;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	bool was_active = mps->source_active;
	mps->source_active = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (was_active)
		return;

	pthread_mutex_lock(&mps->mutex);
	bool has_files = get_total_file_count(mps) > 0;
	bool fresh = mps->shuffle && mps->shuffle_reset_behavior == SHUFFLE_RESET_ON_ACTIVATE;
	pthread_mutex_unlock(&mps->mutex);
	if (!has_files)
		return;
	if (fresh) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps->fresh_shuffle_pending = true;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		activate_fresh_shuffle(mps);
		return;
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->active_slot) {
		mps->active_slot->expected_stop = false;
		mps->active_slot->audio_enabled = true;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		obs_source_media_restart(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		obs_source_media_play_pause(mps->source, false);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		// we only play next when the source is deactivated so we don't do anything here
	}
}

static void mps_deactivate(void *data)
{
	struct media_playlist_source *mps = data;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->source_active = false;
	mps->fresh_shuffle_pending = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_RESTART) {
		obs_source_media_stop(mps->source);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		obs_source_media_play_pause(mps->source, true);
	} else if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT) {
		obs_source_media_stop(mps->source);
		obs_source_media_next(mps->source);
	}
}

static void mps_destroy(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *active_source;

	if (!mps)
		return;

	mps_registry_remove(mps);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->destroying = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	active_source = detach_active_source(mps);
	if (active_source) {
		obs_source_release(active_source);
	}

	destroy_media_slot(&mps->media_slots[0]);
	destroy_media_slot(&mps->media_slots[1]);
	shuffler_destroy(&mps->shuffler);
	mps_duration_inventory_destroy(mps->duration_inventory);
	bfree(mps->telemetry_meaning.item_id);
	bfree(mps->telemetry_meaning.file);
	bfree(mps->telemetry_meaning.source_name);
	free_files(&mps->files.da);
	bfree(mps->logical_next_path);
	bfree(mps->activation_first_id);
	bfree(mps->activation_first_filename);
	bfree(mps->source_uuid);
	pthread_mutex_destroy(&mps->telemetry_mutex);
	pthread_mutex_destroy(&mps->mutex);
	pthread_mutex_destroy(&mps->audio_mutex);
	pthread_mutex_destroy(&mps->lifecycle_mutex);
	bfree(mps->current_media_filename);
	bfree(mps);
}

static void *mps_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	const char *source_uuid = obs_source_get_uuid(source);

	struct media_playlist_source *mps = bzalloc(sizeof(*mps));

	mps->first_update = true;
	mps->source = source;
	mps->source_uuid = source_uuid ? bstrdup(source_uuid) : NULL;
	pthread_mutex_init_value(&mps->mutex);
	pthread_mutex_init_value(&mps->audio_mutex);
	pthread_mutex_init_value(&mps->lifecycle_mutex);
	pthread_mutex_init_value(&mps->telemetry_mutex);
	if (pthread_mutex_init(&mps->lifecycle_mutex, NULL) != 0)
		goto allocation_error;
	if (pthread_mutex_init(&mps->audio_mutex, NULL) != 0)
		goto audio_mutex_error;
	if (pthread_mutex_init(&mps->mutex, NULL) != 0)
		goto mutex_error;
	if (pthread_mutex_init(&mps->telemetry_mutex, NULL) != 0)
		goto telemetry_mutex_error;

	shuffler_init(&mps->shuffler);
	mps->duration_inventory = mps_duration_inventory_create();
	mps_coordinator_init(&mps->coordinator);
	for (size_t i = 0; i < 2; i++)
		mps->media_slots[i].owner = mps;
	mps->standby_slot = &mps->media_slots[0];

	mps->paused = false;

	mps->play_pause_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlayPause",
							    obs_module_text("PlayPause"), play_pause_hotkey, mps);

	mps->restart_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.Restart",
							 obs_module_text("Restart"), restart_hotkey, mps);

	mps->stop_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.Stop", obs_module_text("Stop"),
						      stop_hotkey, mps);

	mps->next_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlaylistNext",
						      obs_module_text("PlaylistNext"), next_hotkey, mps);

	mps->prev_hotkey = obs_hotkey_register_source(source, "MediaPlaylistSource.PlaylistPrev",
						      obs_module_text("PlaylistPrev"), previous_hotkey, mps);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void select_index(int media_index, int folder_item_index)", select_index_proc, mps);

	obs_source_update(source, NULL);
	mps_registry_add(mps);
	return mps;

telemetry_mutex_error:
	pthread_mutex_destroy(&mps->mutex);
mutex_error:
	pthread_mutex_destroy(&mps->audio_mutex);
audio_mutex_error:
	pthread_mutex_destroy(&mps->lifecycle_mutex);
allocation_error:
	bfree(mps->source_uuid);
	bfree(mps);
	return NULL;
}

static void mps_video_render(void *data, gs_effect_t *effect)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	if (source) {
		obs_source_video_render(source);
		obs_source_release(source);
	} else {
		obs_source_video_render(NULL);
	}

	UNUSED_PARAMETER(effect);
}

static bool mps_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	if (!source)
		return false;

	struct obs_source_audio_mix child_audio;
	uint64_t source_ts;

	/*if (obs_source_audio_pending(source))
		return false;*/

	source_ts = obs_source_get_audio_timestamp(source);
	if (!source_ts) {
		obs_source_release(source);
		return false;
	}

	obs_source_get_audio_mix(source, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			memcpy(out, in, AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS * sizeof(float));
		}
	}

	*ts_out = source_ts;
	obs_source_release(source);

	UNUSED_PARAMETER(sample_rate);
	return true;
}

struct media_target {
	char *path;
	bool is_url;
};

static void free_media_target(struct media_target *target)
{
	bfree(target->path);
	target->path = NULL;
}

static bool copy_media_target(const struct media_file_data *media, struct media_target *target)
{
	if (!media || !media->path)
		return false;
	target->path = bstrdup(media->path);
	target->is_url = media->is_url;
	return true;
}

static bool copy_current_target(struct media_playlist_source *mps, struct media_target *target)
{
	bool copied;
	pthread_mutex_lock(&mps->mutex);
	copied = copy_media_target(mps->actual_media, target);
	pthread_mutex_unlock(&mps->mutex);
	return copied;
}

static struct media_file_data *next_sequential_media(struct media_playlist_source *mps)
{
	return mps_next_sequential_media(&mps->files.da, mps->current_media, mps->current_media_index,
					 mps->current_folder_item_index, mps->loop);
}

static bool copy_next_target(struct media_playlist_source *mps, struct media_target *target)
{
	struct media_file_data *media = NULL;
	bool copied = false;
	bool next_changed;

	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		if (shuffler_has_next(&mps->shuffler))
			media = shuffler_peek_next(&mps->shuffler);
	} else {
		media = next_sequential_media(mps);
	}
	if (media)
		copied = copy_media_target(media, target);
	char *next_path = media && media->path ? bstrdup(media->path) : NULL;
	next_changed = (mps->logical_next_path || next_path) &&
		       (!mps->logical_next_path || !next_path || strcmp(mps->logical_next_path, next_path) != 0);
	if (next_changed) {
		bfree(mps->logical_next_path);
		mps->logical_next_path = next_path;
	} else {
		bfree(next_path);
	}
	pthread_mutex_unlock(&mps->mutex);
	if (next_changed)
		mps_playlist_change_notify(mps);
	return copied;
}

static bool target_is_local_video(const struct media_target *target)
{
	struct media_file_data media = {0};
	media.path = target->path;
	media.is_url = target->is_url;
	return is_local_video(&media);
}

static bool slot_matches_target(struct media_playlist_source *mps, struct media_source_slot *slot,
				const struct media_target *target)
{
	bool matches;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	matches = slot && slot->source && !slot->failed && slot->path && strcmp(slot->path, target->path) == 0;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return matches;
}

static obs_source_t *get_slot_source_ref_and_active(struct media_playlist_source *mps, struct media_source_slot *slot,
						    bool *active)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	*active = slot && slot == mps->active_slot;
	if (slot && slot->source)
		source = obs_source_get_ref(slot->source);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return source;
}

static bool slot_has_failed(struct media_playlist_source *mps, struct media_source_slot *slot)
{
	bool failed;
	pthread_mutex_lock(&mps->lifecycle_mutex);
	failed = !slot || slot->failed;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	return failed;
}

static void prepare_next_media(struct media_playlist_source *mps)
{
	struct media_target target = {0};
	struct media_file_data media = {0};
	struct media_source_slot *standby;
	bool request_pending;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	request_pending = mps->coordinator.request_pending;
	standby = mps->standby_slot;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (request_pending || !standby)
		return;
	if (!copy_next_target(mps, &target)) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps->prefetch_dirty = false;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		return;
	}
	if (!target_is_local_video(&target)) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps->prefetch_dirty = false;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		free_media_target(&target);
		return;
	}

	media.path = target.path;
	media.is_url = target.is_url;
	create_media_slot(mps, standby, &media, 0);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->prefetch_dirty = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	free_media_target(&target);
}

static bool activate_and_cut(struct media_playlist_source *mps, struct media_source_slot *slot,
			     uint64_t request_generation)
{
	obs_source_t *new_source = get_slot_source_ref(mps, slot);
	obs_source_t *old_source = get_active_source_ref(mps);
	struct media_source_slot *old_slot;
	char *media_id = NULL;
	char *folder_item_filename = NULL;
	size_t media_index;
	size_t folder_item_index;
	bool folder_item;
	bool child_added;
	bool start_requested;
	bool started;
	bool start_now;
	bool request_start;
	bool manual_showing_needed;
	bool valid;

	if (!new_source) {
		if (old_source)
			obs_source_release(old_source);
		return false;
	}
	pthread_mutex_lock(&mps->mutex);
	media_index = mps->current_media_index;
	folder_item_index = mps->current_folder_item_index;
	folder_item = mps->current_media && mps->current_media->is_folder;
	if (mps->current_media)
		media_id = bstrdup(mps->current_media->id);
	if (folder_item && mps->actual_media)
		folder_item_filename = bstrdup(mps->actual_media->filename);
	pthread_mutex_unlock(&mps->mutex);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	valid = slot->source == new_source && mps_coordinator_matches(&mps->coordinator, request_generation);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!valid) {
		obs_source_release(new_source);
		if (old_source)
			obs_source_release(old_source);
		bfree(media_id);
		bfree(folder_item_filename);
		return false;
	}

	if (slot->local_video) {
		obs_source_show_preloaded_video(new_source);
		if (!obs_source_get_width(new_source) || !obs_source_get_height(new_source)) {
			obs_source_release(new_source);
			if (old_source)
				obs_source_release(old_source);
			bfree(media_id);
			bfree(folder_item_filename);
			return false;
		}

		pthread_mutex_lock(&mps->lifecycle_mutex);
		child_added = slot->child_added;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		if (!child_added) {
			obs_source_add_active_child(mps->source, new_source);
			pthread_mutex_lock(&mps->lifecycle_mutex);
			if (slot->source == new_source)
				slot->child_added = true;
			pthread_mutex_unlock(&mps->lifecycle_mutex);
		}

		if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_ALWAYS_PLAY && !obs_source_showing(mps->source)) {
			pthread_mutex_lock(&mps->lifecycle_mutex);
			manual_showing_needed = slot->source == new_source && !slot->manual_showing;
			if (manual_showing_needed)
				slot->manual_showing = true;
			pthread_mutex_unlock(&mps->lifecycle_mutex);
			if (manual_showing_needed)
				obs_source_inc_showing(new_source);
		}

		start_now = mps->visibility_behavior == VISIBILITY_BEHAVIOR_ALWAYS_PLAY ||
			    (mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE &&
			     obs_source_showing(mps->source));
		pthread_mutex_lock(&mps->lifecycle_mutex);
		start_requested = slot->start_requested;
		started = slot->started;
		request_start = start_now && !start_requested && slot->source == new_source;
		if (request_start)
			slot->start_requested = true;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		if (request_start) {
			/* Keep clear_on_media_end enabled until ffmpeg_source_start has shown B's preload. */
			disable_restart_on_activate(new_source);
			obs_source_media_restart(new_source);
			obs_source_release(new_source);
			if (old_source)
				obs_source_release(old_source);
			bfree(media_id);
			bfree(folder_item_filename);
			return false;
		}
		if (!started) {
			obs_source_release(new_source);
			if (old_source)
				obs_source_release(old_source);
			bfree(media_id);
			bfree(folder_item_filename);
			return false;
		}

		configure_active_media_source(mps, new_source);
		obs_source_show_preloaded_video(new_source);
		if (!obs_source_get_width(new_source) || !obs_source_get_height(new_source)) {
			obs_source_release(new_source);
			if (old_source)
				obs_source_release(old_source);
			destroy_media_slot(slot);
			bfree(media_id);
			bfree(folder_item_filename);
			return false;
		}
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	valid = slot->source == new_source && !slot->failed &&
		mps_coordinator_matches(&mps->coordinator, request_generation);
	old_slot = mps->active_slot;
	if (valid) {
		pthread_mutex_lock(&mps->audio_mutex);
		clear_audio_queues_locked(mps);
		if (old_slot)
			old_slot->expected_stop = true;
		slot->expected_stop = false;
		slot->audio_enabled = true;
		slot->media_index = media_index;
		slot->folder_item_index = folder_item_index;
		slot->folder_item = folder_item;
		bfree(slot->media_id);
		bfree(slot->folder_item_filename);
		slot->media_id = media_id;
		slot->folder_item_filename = folder_item_filename;
		media_id = NULL;
		folder_item_filename = NULL;
		mps->active_slot = slot;
		mps->standby_slot =
			old_slot ? old_slot
				 : (slot == &mps->media_slots[0] ? &mps->media_slots[1] : &mps->media_slots[0]);
		mps_coordinator_complete(&mps->coordinator, request_generation);
		pthread_mutex_unlock(&mps->audio_mutex);
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (!valid) {
		obs_source_release(new_source);
		if (old_source)
			obs_source_release(old_source);
		destroy_media_slot(slot);
		bfree(media_id);
		bfree(folder_item_filename);
		return false;
	}
	if (!slot->local_video) {
		obs_source_add_active_child(mps->source, new_source);
		pthread_mutex_lock(&mps->lifecycle_mutex);
		if (slot->source == new_source)
			slot->child_added = true;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
	}

	if (old_source) {
		obs_source_release(old_source);
	}
	obs_source_release(new_source);
	bfree(media_id);
	bfree(folder_item_filename);

	if (old_slot)
		destroy_media_slot(old_slot);
	mps->paused = false;
	set_media_state(mps, OBS_MEDIA_STATE_PLAYING);
	/* The prefetch below may auto-reshuffle a just-finished shuffle cycle, so
	 * remember that this generation ended its cycle while the cursor is still
	 * at the end. */
	record_shuffle_cycle_exhausted(mps, slot);
	prepare_next_media(mps);
	mps_playlist_change_notify(mps);
	return true;
}

static void queue_retry(struct media_playlist_source *mps, enum mps_switch_request request)
{
	bool can_retry = request == MPS_SWITCH_PREVIOUS ? playlist_has_previous(mps) : playlist_has_next(mps);
	if (!can_retry) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps_coordinator_cancel(&mps->coordinator);
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		mps_end_reached(mps);
		return;
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->retrying_switch = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (request == MPS_SWITCH_PREVIOUS)
		obs_source_media_previous(mps->source);
	else
		obs_source_media_next(mps->source);
}

static void handle_preload_failure(struct media_playlist_source *mps, struct media_source_slot *slot,
				   uint64_t request_generation)
{
	enum mps_switch_request request;
	enum mps_switch_failure failure;
	uint64_t generation;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	generation = request_generation ? request_generation : (slot ? slot->request_generation : 0);
	request = mps->coordinator.request;
	failure = mps_coordinator_fail(&mps->coordinator, generation);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (slot)
		destroy_media_slot(slot);

	if (failure == MPS_SWITCH_FAILURE_RETRY && (request == MPS_SWITCH_NEXT || request == MPS_SWITCH_PREVIOUS)) {
		queue_retry(mps, request);
	} else if (failure == MPS_SWITCH_FAILURE_EXHAUSTED) {
		mps_end_reached(mps);
	}
}

static void prepare_requested_media(struct media_playlist_source *mps)
{
	struct media_target target = {0};
	struct media_file_data media = {0};
	struct media_source_slot *standby;
	uint64_t generation;
	enum mps_switch_request request;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (!mps_coordinator_take_request(&mps->coordinator, &generation, &request)) {
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		return;
	}
	standby = mps->standby_slot;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (!standby || !copy_current_target(mps, &target)) {
		handle_preload_failure(mps, standby, generation);
		return;
	}

	if (slot_matches_target(mps, standby, &target)) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		standby->request_generation = generation;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		free_media_target(&target);
		return;
	}

	media.path = target.path;
	media.is_url = target.is_url;
	if (!create_media_slot(mps, standby, &media, generation)) {
		free_media_target(&target);
		handle_preload_failure(mps, standby, generation);
		return;
	}
	free_media_target(&target);

	if (slot_has_failed(mps, standby)) {
		handle_preload_failure(mps, standby, generation);
	} else if (!standby->local_video) {
		activate_and_cut(mps, standby, generation);
	}
}

static bool playlist_has_next(struct media_playlist_source *mps)
{
	bool has_next;
	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		has_next = shuffler_has_next(&mps->shuffler);
	} else {
		has_next = next_sequential_media(mps) != NULL;
	}
	pthread_mutex_unlock(&mps->mutex);
	return has_next;
}

static bool playlist_has_previous(struct media_playlist_source *mps)
{
	bool has_previous;
	pthread_mutex_lock(&mps->mutex);
	if (mps->shuffle) {
		has_previous = shuffler_has_prev(&mps->shuffler);
	} else {
		has_previous = mps->current_media &&
			       ((mps->current_media->is_folder && mps->current_folder_item_index > 0) ||
				mps->current_media_index > 0 || (mps->loop && mps->files.num > 0));
	}
	pthread_mutex_unlock(&mps->mutex);
	return has_previous;
}

static void process_slot_events(struct media_playlist_source *mps)
{
	struct media_source_slot *active;
	struct media_source_slot *standby;
	bool active_ended = false;
	bool active_produced = false;
	bool standby_failed = false;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	active = mps->active_slot;
	standby = mps->standby_slot;
	if (active && active->ended && !active->ended_handled) {
		active->ended_handled = true;
		active_ended = true;
		active_produced = active->produced_output;
	}
	if (standby && standby->failed)
		standby_failed = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	if (standby_failed) {
		handle_preload_failure(mps, standby, 0);
		return;
	}

	if (!active_ended)
		return;
	if (active_produced) {
		mps->consecutive_failures = 0;
	} else if (++mps->consecutive_failures >= get_total_file_count(mps)) {
		mps_end_reached(mps);
		return;
	}

	if (playlist_has_next(mps)) {
		pthread_mutex_lock(&mps->lifecycle_mutex);
		mps->automatic_next_pending = true;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		obs_source_media_next(mps->source);
	} else {
		mps_end_reached(mps);
	}
}

static void mps_video_tick(void *data, float seconds)
{
	struct media_playlist_source *mps = data;
	struct media_source_slot *standby;
	obs_source_t *standby_source;
	uint64_t request_generation;
	bool failed;
	bool local_video;
	bool refresh_prefetch;
	bool request_pending;
	bool resetting;
	UNUSED_PARAMETER(seconds);
	obs_source_t *active_source = get_active_source_ref(mps);
	if (active_source) {
		/* Child media actions are queued by OBS. In particular there is no
		 * seek-completed signal: observe applied idle-clock changes in this
		 * existing lifecycle tick, without a second timer or guessed cursor.
		 * Playing-clock progress never wakes the publisher here. */
		enum obs_media_state clock_state = obs_source_media_get_state(active_source);
		bool idle_clock = clock_state == OBS_MEDIA_STATE_PAUSED || clock_state == OBS_MEDIA_STATE_STOPPED ||
				  clock_state == OBS_MEDIA_STATE_ENDED;
		int64_t idle_ms = idle_clock ? obs_source_media_get_time(active_source) : -1;
		bool changed = false;
		pthread_mutex_lock(&mps->lifecycle_mutex);
		if (mps->active_slot && mps->active_slot->source == active_source) {
			uint64_t generation = mps->active_slot->source_generation;
			changed = mps->telemetry_clock_generation != generation ||
				  mps->telemetry_clock_state != clock_state ||
				  (idle_clock && mps->telemetry_idle_clock_ms != idle_ms);
			mps->telemetry_clock_generation = generation;
			mps->telemetry_clock_state = clock_state;
			mps->telemetry_idle_clock_ms = idle_ms;
		}
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		if (changed)
			mps_playlist_change_notify(mps);
		if (obs_source_get_width(active_source) && obs_source_get_height(active_source)) {
			pthread_mutex_lock(&mps->lifecycle_mutex);
			if (mps->active_slot && mps->active_slot->source == active_source)
				mps->active_slot->produced_output = true;
			pthread_mutex_unlock(&mps->lifecycle_mutex);
		}
		obs_source_release(active_source);
	}

	pthread_mutex_lock(&mps->lifecycle_mutex);
	resetting = mps->resetting;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	if (!resetting) {
		activate_fresh_shuffle(mps);
		process_slot_events(mps);
		prepare_requested_media(mps);
		pthread_mutex_lock(&mps->lifecycle_mutex);
		refresh_prefetch = mps->prefetch_dirty;
		request_pending = mps->coordinator.request_pending;
		standby = mps->standby_slot;
		pthread_mutex_unlock(&mps->lifecycle_mutex);
		if (refresh_prefetch && !request_pending) {
			destroy_media_slot(standby);
			prepare_next_media(mps);
		}

		pthread_mutex_lock(&mps->lifecycle_mutex);
		standby = mps->standby_slot;
		request_generation = standby ? standby->request_generation : 0;
		failed = standby && standby->failed;
		local_video = standby && standby->local_video;
		pthread_mutex_unlock(&mps->lifecycle_mutex);

		standby_source = get_slot_source_ref(mps, standby);
		if (standby_source && local_video && !failed) {
			obs_source_show_preloaded_video(standby_source);
			if (obs_source_get_width(standby_source) && obs_source_get_height(standby_source)) {
				pthread_mutex_lock(&mps->lifecycle_mutex);
				if (standby->source == standby_source)
					standby->produced_output = true;
				pthread_mutex_unlock(&mps->lifecycle_mutex);
				if (request_generation)
					activate_and_cut(mps, standby, request_generation);
			}
			obs_source_release(standby_source);
		}
	}

	const audio_t *a = obs_get_audio();
	const struct audio_output_info *aoi = audio_output_get_info(a);
	pthread_mutex_lock(&mps->audio_mutex);
	while (mps->audio_frames.size > 0) {
		struct obs_source_audio audio;
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		deque_pop_front(&mps->audio_frames, &audio.frames, sizeof(audio.frames));
		deque_pop_front(&mps->audio_timestamps, &audio.timestamp, sizeof(audio.timestamp));
		for (size_t i = 0; i < mps->num_channels; i++) {
			audio.data[i] = (uint8_t *)mps->audio_data[i].data + mps->audio_data[i].start_pos;
		}
		obs_source_output_audio(mps->source, &audio);
		for (size_t i = 0; i < mps->num_channels; i++) {
			deque_pop_front(&mps->audio_data[i], NULL, audio.frames * sizeof(float));
		}
	}
	mps->num_channels = audio_output_get_channels(a);
	pthread_mutex_unlock(&mps->audio_mutex);

	//if (mps->restart_on_activate && mps->use_cut) {
	//	mps->elapsed = 0.0f;
	//	mps->cur_item = mps->randomize ? random_file(mps) : 0;
	//	do_transition(mps, false);
	//	mps->restart_on_activate = false;
	//	mps->use_cut = false;
	//	mps->stop = false;
	//	return;
	//}

	//if (mps->pause_on_deactivate || mps->manual || mps->stop || mps->paused)
	//	return;

	///* ----------------------------------------------------- */
	///* do transition when slide time reached                 */
	//mps->elapsed += seconds;

	//if (mps->elapsed > mps->slide_time) {
	//	mps->elapsed -= mps->slide_time;

	//	if (!mps->loop && mps->cur_item == mps->files.num - 1) {
	//		if (mps->hide)
	//			do_transition(mps, true);
	//		else
	//			do_transition(mps, false);

	//		return;
	//	}

	//	if (mps->randomize) {
	//		size_t next = mps->cur_item;
	//		if (mps->files.num > 1) {
	//			while (next == mps->cur_item)
	//				next = random_file(mps);
	//		}
	//		mps->cur_item = next;

	//	} else if (++mps->cur_item >= mps->files.num) {
	//		mps->cur_item = 0;
	//	}

	//	if (mps->files.num)
	//		do_transition(mps, false);
	//}
}

static void mps_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct media_playlist_source *mps = data;
	obs_source_t *sources[2] = {0};
	size_t source_count = 0;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	for (size_t i = 0; i < 2; i++) {
		struct media_source_slot *slot = &mps->media_slots[i];
		bool active_slot = slot == mps->active_slot;
		bool standby_slot = slot == mps->standby_slot;
		bool duplicate = false;
		obs_source_t *source;

		if ((!active_slot && !standby_slot) || !slot->source ||
		    !mps_source_is_active_child(active_slot, slot->child_added))
			continue;

		for (size_t j = 0; j < source_count; j++) {
			if (sources[j] == slot->source) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		source = obs_source_get_ref(slot->source);
		if (source)
			sources[source_count++] = source;
	}
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	for (size_t i = 0; i < source_count; i++) {
		cb(mps->source, sources[i], param);
		obs_source_release(sources[i]);
	}
}

static uint32_t mps_width(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	uint32_t width = source ? obs_source_get_width(source) : 0;
	if (source)
		obs_source_release(source);
	return width;
}

static uint32_t mps_height(void *data)
{
	struct media_playlist_source *mps = data;
	obs_source_t *source = get_active_source_ref(mps);
	uint32_t height = source ? obs_source_get_height(source) : 0;
	if (source)
		obs_source_release(source);
	return height;
}

static void mps_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_int(settings, S_SHUFFLE_RESET_BEHAVIOR, SHUFFLE_RESET_PRESERVE);
	obs_data_set_default_int(settings, S_VISIBILITY_BEHAVIOR, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_data_set_default_int(settings, S_RESTART_BEHAVIOR, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_data_set_default_string(settings, S_CURRENT_FILE_NAME, " ");
	obs_data_set_default_int(settings, S_SPEED, 100);
}

static void add_media_to_selection(obs_property_t *list, struct media_file_data *data)
{
	struct dstr key = {0};
	struct dstr name = {0};

	if (data->parent) {
		dstr_catf(&key, "%zu-%zu", data->parent->index + 1, data->index + 1);
	} else if (data->folder_items.num) {
		for (size_t i = 0; i < data->folder_items.num; i++) {
			add_media_to_selection(list, &data->folder_items.array[i]);
		}
		return;
	} else {
		dstr_catf(&key, "%zu", data->index + 1);
	}
	dstr_copy_dstr(&name, &key);
	dstr_cat(&name, ": ");
	dstr_cat(&name, data->path);
	obs_property_list_add_string(list, name.array, key.array);
	dstr_free(&name);
	dstr_free(&key);
}

static void update_current_filename_property(struct media_playlist_source *mps, obs_property_t *p)
{
	struct dstr long_desc = {0};
	if (!mps || !p)
		return;
	if (!mps->actual_media) {
		obs_property_set_long_description(p, " ");
		return;
	} else if (mps->actual_media->parent) {
		dstr_catf(&long_desc, "%zu-%zu", mps->actual_media->parent->index + 1, mps->actual_media->index + 1);
	} else {
		dstr_catf(&long_desc, "%zu", mps->actual_media->index + 1);
	}
	dstr_catf(&long_desc, ": %s", mps->actual_media->path);
	obs_property_set_long_description(p, long_desc.array);
	dstr_free(&long_desc);
}

static obs_properties_t *mps_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct media_playlist_source *mps = data;
	obs_data_t *settings = obs_source_get_settings(mps->source);
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	struct dstr filter = {0};
	struct dstr exts = {0};
	struct dstr path = {0};
	obs_property_t *p;

	obs_properties_add_bool(props, S_LOOP, T_LOOP);
	obs_properties_add_bool(props, S_SHUFFLE, T_SHUFFLE);
	p = obs_properties_add_list(props, S_SHUFFLE_RESET_BEHAVIOR, T_SHUFFLE_RESET_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_SHUFFLE_RESET_PRESERVE, SHUFFLE_RESET_PRESERVE);
	obs_property_list_add_int(p, T_SHUFFLE_RESET_ON_ACTIVATE, SHUFFLE_RESET_ON_ACTIVATE);

	// get last directory opened for editable list
	if (mps) {
		pthread_mutex_lock(&mps->mutex);
		if (mps->files.num) {
			struct media_file_data *last = da_end(mps->files);
			const char *slash;

			dstr_copy(&path, last->path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&mps->mutex);
	}

	p = obs_properties_add_list(props, S_VISIBILITY_BEHAVIOR, T_VISIBILITY_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_RESTART, VISIBILITY_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT, VISIBILITY_BEHAVIOR_STOP_PLAY_NEXT);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE, VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE);
	obs_property_list_add_int(p, T_VISIBILITY_BEHAVIOR_ALWAYS_PLAY, VISIBILITY_BEHAVIOR_ALWAYS_PLAY);

	p = obs_properties_add_list(props, S_RESTART_BEHAVIOR, T_RESTART_BEHAVIOR, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_CURRENT_FILE, RESTART_BEHAVIOR_CURRENT_FILE);
	obs_property_list_add_int(p, T_RESTART_BEHAVIOR_FIRST_FILE, RESTART_BEHAVIOR_FIRST_FILE);

	obs_properties_add_bool(props, S_FFMPEG_HW_DECODE, T_USE_HARDWARE_DECODING);

	p = obs_properties_add_bool(props, S_FFMPEG_CLOSE_WHEN_INACTIVE, T_FFMPEG_CLOSE_WHEN_INACTIVE);
	obs_property_set_long_description(p, T_FFMPEG_CLOSE_WHEN_INACTIVE_TOOLTIP);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	p = obs_properties_add_editable_list(props, S_PLAYLIST, T_PLAYLIST, OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					     filter.array, path.array);
	dstr_free(&path);
	dstr_free(&filter);
	dstr_free(&exts);

	p = obs_properties_add_text(props, S_CURRENT_FILE_NAME, T_CURRENT_FILE_NAME, OBS_TEXT_INFO);
	obs_property_set_long_description(p, "Due to OBS limitations, this will only update if any settings"
					     " are changed, the selected file is played, or the Properties "
					     "window is reopened. It will not update when the video ends.");
	obs_properties_add_button2(props, S_REFRESH_FILENAME, T_REFRESH_FILENAME, refresh_filename_clicked, NULL);
	//update_current_filename_property(mps, p);

	p = obs_properties_add_list(props, S_SELECT_FILE, T_SELECT_FILE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_NO_FILE_SELECTED, "0");
	if (mps) {
		for (size_t i = 0; i < mps->files.num; i++) {
			add_media_to_selection(p, &mps->files.array[i]);
		}
	}

	obs_properties_add_button2(props, "play_selected", "Play Selected File", play_selected_clicked, NULL);

	p = obs_properties_add_int_slider(props, S_SPEED, T_SPEED, 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_text(props, "", T_SPEED_WARNING, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	obs_data_array_release(array);
	obs_data_release(settings);

	return props;
}

/* Sets the parent field of each media_file_data. This is necessary to be
 * called AFTER the darray is size is modified (because of array reallocation)
 */
static void set_parents(struct darray *array)
{
	DARRAY(struct media_file_data) files;
	files.da = *array;
	for (size_t i = 0; i < files.num; i++) {
		struct media_file_data *item = &files.array[i];
		for (size_t j = 0; j < item->folder_items.num; j++) {
			struct media_file_data *folder_item = &item->folder_items.array[j];
			folder_item->parent = item;
		}
	}
}

static void add_file(struct darray *array, const char *path, const char *id)
{
	DARRAY(struct media_file_data) new_files;
	new_files.da = *array;
	struct media_file_data *data = da_push_back_new(new_files);

	data->id = bstrdup(id);
	data->index = new_files.num - 1;
	data->path = bstrdup(path);
	data->is_url = strstr(path, "://") != NULL;
	da_init(data->folder_items);

	os_dir_t *dir = os_opendir(path);

	if (dir) {
		struct dstr dir_path = {0};
		struct os_dirent *ent;

		data->is_folder = true;

		while (true) {
			const char *ext;

			ent = os_readdir(dir);
			if (!ent)
				break;
			if (ent->directory)
				continue;

			ext = os_get_path_extension(ent->d_name);
			if (!valid_extension(ext))
				continue;

			struct media_file_data folder_item = {0};
			folder_item.filename = bstrdup(ent->d_name);
			folder_item.parent_id = data->id;
			folder_item.index = data->folder_items.num;
			dstr_copy(&dir_path, path);
			dstr_cat_ch(&dir_path, '/');
			dstr_cat(&dir_path, ent->d_name);
			folder_item.path = bstrdup(dir_path.array);

			da_push_back(data->folder_items, &folder_item);
		}

		dstr_free(&dir_path);
		os_closedir(dir);
	}

	*array = new_files.da;
}

/* mps_duration_inventory_set_paths() copies the paths itself, so the playlist
 * lock is released first while the temporary copies keep the pointers valid.
 * Copying stops at the inventory's per-source entry bound. */
static char *copy_inventory_path(const char *path)
{
	if (!path || !*path || !memchr(path, '\0', MPS_DURATION_INVENTORY_MAX_PATH_BYTES))
		return NULL;
	return bstrdup(path);
}

static void update_duration_inventory(struct media_playlist_source *mps)
{
	size_t visited = 0;
	DARRAY(char *) paths;
	da_init(paths);

	pthread_mutex_lock(&mps->mutex);
	for (size_t i = 0; i < mps->files.num && i < MPS_DURATION_INVENTORY_MAX_ENTRIES &&
			   visited < MPS_DURATION_INVENTORY_MAX_ENTRIES;
	     ++i) {
		struct media_file_data *media = &mps->files.array[i];
		if (media->is_folder) {
			for (size_t j = 0; j < media->folder_items.num && visited < MPS_DURATION_INVENTORY_MAX_ENTRIES;
			     ++j) {
				++visited;
				const char *path = media->folder_items.array[j].path;
				char *copy = copy_inventory_path(path);
				if (copy)
					da_push_back(paths, &copy);
				if (paths.num >= MPS_DURATION_INVENTORY_MAX_ENTRIES)
					break;
			}
		} else {
			++visited;
			char *copy = copy_inventory_path(media->path);
			if (copy)
				da_push_back(paths, &copy);
		}
	}
	pthread_mutex_unlock(&mps->mutex);

	mps_duration_inventory_set_paths(mps->duration_inventory, (const char *const *)paths.array, paths.num);
	for (size_t i = 0; i < paths.num; ++i)
		bfree(paths.array[i]);
	da_free(paths);
}

static void mps_update(void *data, obs_data_t *settings)
{
	DARRAY(struct media_file_data) new_files;
	DARRAY(struct media_file_data) old_files;
	struct media_playlist_source *mps = data;
	obs_data_array_t *array;
	size_t count;
	bool shuffle = false;
	bool shuffle_changed = false;
	enum visibility_behavior visibility_behavior = mps->visibility_behavior;
	bool visibility_behavior_changed = false;
	bool item_edited = false;
	bool transition_pending;
	bool restart_on_activate = true;
	const char *old_media_path = NULL;
	long long new_speed;
	//const char *mode;

	/* ------------------------------------- */
	/* get settings data */

	da_init(new_files);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->resetting = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	transition_pending = cancel_pending_transition(mps, true, false);
	invalidate_logical_next_path(mps);

	mps->visibility_behavior = obs_data_get_int(settings, S_VISIBILITY_BEHAVIOR);
	if (mps->visibility_behavior != visibility_behavior) {
		visibility_behavior_changed = true;
	}
	mps->restart_behavior = obs_data_get_int(settings, S_RESTART_BEHAVIOR);
	shuffle = obs_data_get_bool(settings, S_SHUFFLE);
	new_speed = obs_data_get_int(settings, S_SPEED);
	pthread_mutex_lock(&mps->mutex);
	mps->speed = new_speed;
	pthread_mutex_unlock(&mps->mutex);

	/* Internal media source settings */
	mps->use_hw_decoding = obs_data_get_bool(settings, S_FFMPEG_HW_DECODE);
	mps->close_when_inactive = obs_data_get_bool(settings, S_FFMPEG_CLOSE_WHEN_INACTIVE);
	if (mps->visibility_behavior == VISIBILITY_BEHAVIOR_ALWAYS_PLAY ||
	    mps->visibility_behavior == VISIBILITY_BEHAVIOR_PAUSE_UNPAUSE) {
		restart_on_activate = false;
	}
	for (size_t i = 0; i < 2; i++) {
		bool active;
		obs_source_t *media_source = get_slot_source_ref_and_active(mps, &mps->media_slots[i], &active);
		if (!media_source)
			continue;

		obs_data_t *media_source_settings = obs_data_create();
		obs_data_set_bool(media_source_settings, S_FFMPEG_RESTART_ON_ACTIVATE, restart_on_activate);
		obs_data_set_bool(media_source_settings, S_FFMPEG_HW_DECODE, mps->use_hw_decoding);
		obs_data_set_bool(media_source_settings, S_FFMPEG_CLOSE_WHEN_INACTIVE,
				  active && mps->close_when_inactive);
		obs_data_set_int(media_source_settings, S_SPEED, mps->speed);
		obs_source_update(media_source, media_source_settings);
		obs_data_release(media_source_settings);
		obs_source_release(media_source);
	}
	/* mps_get_state() reports NONE when no child is committed; do not erase a
	 * meaningful cached state in that window. */
	enum obs_media_state observed_state = mps_get_state(mps);
	if (observed_state != OBS_MEDIA_STATE_NONE)
		set_media_state(mps, observed_state);
	if (visibility_behavior_changed && !obs_source_active(mps->source) &&
	    (observed_state == OBS_MEDIA_STATE_PLAYING || observed_state == OBS_MEDIA_STATE_PAUSED)) {
		mps_deactivate(mps);
	}

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	bool found = false;
	bool has_files;
	bool empty_folder = false;
	bool should_switch;
	pthread_mutex_lock(&mps->mutex);
	shuffle_changed = mps->shuffle != shuffle;
	mps->shuffle = shuffle;
	mps->shuffle_reset_behavior = obs_data_get_int(settings, S_SHUFFLE_RESET_BEHAVIOR);
	mps->loop = obs_data_get_bool(settings, S_LOOP);
	shuffler_set_loop(&mps->shuffler, mps->loop);
	if (!mps->first_update && mps->current_media)
		old_media_path = mps->current_media->path;
	if (mps->first_update) {
		mps->current_media_filename = bstrdup(obs_data_get_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME));
		mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	}
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		const char *id = obs_data_get_string(item, S_ID);

		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}

		if (!mps->first_update && mps->current_media && strcmp(id, mps->current_media->id) == 0) {
			// check for current_media->id only if media isn't changed, allowing scripts to set the index.
			mps->current_media_index = i;
			found = true;
			if (old_media_path)
				item_edited = strcmp(old_media_path, path) != 0;
		}
		add_file(&new_files.da, path, id);
		obs_data_release(item);
	}
	set_parents(&new_files.da);

	if (mps->shuffle) {
		if (shuffle_changed) {
			shuffler_reshuffle(&mps->shuffler);
		}
		shuffler_update_files(&mps->shuffler, &new_files.da);
	} else if (shuffle_changed) {
		bfree(mps->current_media_filename);
		if (mps->actual_media && mps->actual_media->parent_id)
			mps->current_media_filename = bstrdup(mps->actual_media->filename);
		else
			mps->current_media_filename = NULL;
	}
	old_files.da = mps->files.da;
	mps->files.da = new_files.da;
	++mps->playlist_revision;
	free_files(&old_files.da);
	if (found || mps->first_update) {
		set_current_media_index(mps, mps->current_media_index);
	} else {
		set_current_media_index(mps, 0);
	}
	if (mps->current_media && mps->current_media->is_folder)
		mps->actual_media = NULL;
	else
		mps->actual_media = mps->current_media;
	has_files = get_total_file_count(mps) > 0;
	pthread_mutex_unlock(&mps->mutex);

	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->prefetch_dirty = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	pthread_mutex_lock(&mps->mutex);
	if (has_files) {
		if (item_edited) {
			mps->current_folder_item_index = 0;
		} else if (mps->current_media && mps->current_media->is_folder) {
			mps->current_folder_item_index = 0;

			/* Find that file in the folder */
			if (mps->current_media_filename) {
				// some files may have been added/deleted so current file index is changed
				mps->current_folder_item_index = find_folder_item_index(
					&mps->current_media->folder_items.da, mps->current_media_filename);
				if (mps->current_folder_item_index == DARRAY_INVALID) {
					mps->current_folder_item_index = 0;
					found = false;
				}
			}

			if (mps->current_media->folder_items.num == 0) {
				empty_folder = true;
			} else {
				set_current_folder_item_index(mps, mps->current_folder_item_index);
			}
		} else {
			mps->actual_media = mps->current_media;
		}

		if (mps->shuffle && !empty_folder && !item_edited) {
			struct shuffler *s = &mps->shuffler;
			size_t current = s->next ? s->next - 1 : s->shuffled_files.num - 1;
			/* Rebinding an unchanged current item is not a manual selection:
			 * moving it past a peeked next item would silently skip that item. */
			if (!(s->next || s->head) || s->shuffled_files.array[current] != mps->actual_media)
				shuffler_select(s, mps->actual_media);
		}

		should_switch = mps->first_update || !found || item_edited || transition_pending;
	} else {
		should_switch = false;
	}
	if (!has_files && !mps->first_update) {
		bfree(mps->current_media_filename);
		mps->current_media_filename = NULL;
	}
	pthread_mutex_unlock(&mps->mutex);

	if (has_files) {
		if (empty_folder)
			mps_playlist_next(mps);
		if (should_switch) {
			bool clear_source;
			pthread_mutex_lock(&mps->mutex);
			clear_source = mps->current_media && mps->current_media->is_folder &&
				       mps->current_media->folder_items.num == 0;
			if (!clear_source)
				update_media_source(mps, true);
			pthread_mutex_unlock(&mps->mutex);
			if (clear_source)
				clear_media_source(mps);
			else
				request_media_switch(mps, MPS_SWITCH_NEXT, false);
		}
	} else if (!mps->first_update) {
		clear_media_source(mps);
	}
	rebind_active_slot_metadata(mps);
	obs_source_save(mps->source);

	/* So Current File Name is updated */
	update_current_filename_setting(mps, settings);
	/* ------------------------- */

	//if (mps->files.num) {
	//	do_transition(mps, false);

	//	if (mps->manual)
	//		set_media_state(mps, OBS_MEDIA_STATE_PAUSED);
	//	else
	//		set_media_state(mps, OBS_MEDIA_STATE_PLAYING);

	//	obs_source_media_started(mps->source);
	//}
	obs_data_array_release(array);
	/* set_paths() copies the paths, so only the temporary copies below keep the
	 * call outside the playlist lock; probing stays on the inventory worker. */
	update_duration_inventory(mps);
	mps->first_update = false;
	invalidate_shuffle_cycle_exhausted(mps);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->resetting = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	mps_playlist_change_notify(mps);
}

static void mps_save(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	obs_data_set_int(settings, S_CURRENT_MEDIA_INDEX, mps->current_media_index);
	obs_data_set_string(settings, S_CURRENT_FOLDER_ITEM_FILENAME, mps->current_media_filename);
	update_current_filename_setting(mps, settings);
}

static void mps_load(void *data, obs_data_t *settings)
{
	struct media_playlist_source *mps = data;
	mps->current_media_index = obs_data_get_int(settings, S_CURRENT_MEDIA_INDEX);
	if (mps->files.num)
		obs_log(LOG_DEBUG, "%s", mps->files.array[mps->current_media_index].id);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct media_playlist_source *mps = src;
	const char *orig_path = data;

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *file = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);
}

static obs_missing_files_t *mps_missingfiles(void *data)
{
	struct media_playlist_source *mps = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();

	obs_source_t *source = mps->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (strcmp(path, "") != 0) {
			if (!os_file_exists(path) && strstr(path, "://") == NULL) {
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source, (void *)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}
