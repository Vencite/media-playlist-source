#ifdef NDEBUG
#undef NDEBUG
#endif

/* White-box lifecycle tests use the real callbacks and libobs, but deliberately
 * do not start graphics/audio or tick decoders. Rendering remains a manual check. */
#include "../src/media-playlist-source.c"

const char *obs_module_text(const char *text)
{
	return text;
}

void obs_log(int level, const char *format, ...)
{
	UNUSED_PARAMETER(level);
	UNUSED_PARAMETER(format);
}

static obs_source_t *create_playlist(size_t count, enum shuffle_reset_behavior behavior)
{
	obs_data_t *settings = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();
	mps_defaults(settings);
	obs_data_set_bool(settings, S_SHUFFLE, true);
	obs_data_set_bool(settings, S_LOOP, false);
	obs_data_set_int(settings, S_VISIBILITY_BEHAVIOR, VISIBILITY_BEHAVIOR_ALWAYS_PLAY);
	obs_data_set_int(settings, S_SHUFFLE_RESET_BEHAVIOR, behavior);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_create();
		char path[32];
		char id[32];
		snprintf(path, sizeof(path), "clip%zu.mp4", i);
		snprintf(id, sizeof(id), "item%zu", i);
		obs_data_set_string(item, "value", path);
		obs_data_set_string(item, S_ID, id);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, S_PLAYLIST, items);
	obs_data_array_release(items);
	obs_source_t *source = obs_source_create("media_playlist_source_codeyan", "shuffle test", settings, NULL);
	assert(source);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	assert(mps);
	/* Video-source updates are normally dispatched by OBS's video thread. */
	mps_update(mps, settings);
	shuffler_set_seed(&mps->shuffler, 42);
	obs_data_release(settings);
	return source;
}

static void test_compatibility_default(void)
{
	obs_data_t *settings = obs_data_create();
	mps_defaults(settings);
	assert(obs_data_get_int(settings, S_SHUFFLE_RESET_BEHAVIOR) == SHUFFLE_RESET_PRESERVE);
	obs_data_release(settings);

	obs_source_t *source = create_playlist(4, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps_playlist_next(mps);
	size_t head = mps->shuffler.head;
	size_t next = mps->shuffler.next;
	size_t history = mps->shuffler.history;
	uint64_t rng = mps->shuffler.rng_state;
	mps_activate(mps);
	mps_deactivate(mps);
	mps_activate(mps);
	assert(mps->shuffler.head == head);
	assert(mps->shuffler.next == next);
	assert(mps->shuffler.history == history);
	assert(mps->shuffler.rng_state == rng);
	obs_source_release(source);
}

static void test_fresh_activation_and_redundant_callback(void)
{
	obs_source_t *source = create_playlist(4, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps_activate(mps);
	struct media_file_data *first = mps->actual_media;
	uint64_t generation = mps->coordinator.generation;
	uint64_t rng = mps->shuffler.rng_state;
	assert(mps->shuffler.next == 1);
	assert(!shuffler_has_prev(&mps->shuffler));
	assert(mps->coordinator.request_pending);
	mps_activate(mps);
	assert(mps->actual_media == first);
	assert(mps->coordinator.generation == generation);
	assert(mps->shuffler.rng_state == rng);

	mps_deactivate(mps);
	mps_activate(mps);
	assert(mps->actual_media != first);
	assert(mps->shuffler.next == 1);
	assert(!shuffler_has_prev(&mps->shuffler));
	bool seen[4] = {0};
	seen[mps->actual_media->index] = true;
	for (size_t i = 1; i < 4; i++) {
		mps_playlist_next(mps);
		assert(!seen[mps->actual_media->index]);
		seen[mps->actual_media->index] = true;
	}
	assert(!shuffler_has_next(&mps->shuffler));
	obs_source_release(source);
}

static void test_empty_single_and_shuffle_disabled(void)
{
	for (size_t count = 0; count <= 1; count++) {
		obs_source_t *source = create_playlist(count, SHUFFLE_RESET_ON_ACTIVATE);
		struct media_playlist_source *mps = obs_obj_get_data(source);
		mps_activate(mps);
		mps_deactivate(mps);
		mps_activate(mps);
		assert(mps->shuffler.next == count);
		if (count)
			assert(mps->actual_media == &mps->files.array[0]);
		obs_source_release(source);
	}
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps->shuffle = false;
	uint64_t rng = mps->shuffler.rng_state;
	mps_activate(mps);
	assert(mps->shuffler.rng_state == rng);
	assert(!mps->activation_first_id);
	obs_source_release(source);
}

static void test_stale_end_and_pending_request_are_superseded(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps->active_slot = &mps->media_slots[0];
	mps->standby_slot = &mps->media_slots[1];
	mps->active_slot->ended = true;
	mps->automatic_next_pending = true;
	mps->retrying_switch = true;
	uint64_t old_generation = mps->coordinator.generation;
	mps_activate(mps);
	assert(!mps_coordinator_matches(&mps->coordinator, old_generation));
	assert(mps->active_slot->ended_handled);
	assert(!mps->automatic_next_pending);
	assert(!mps->retrying_switch);
	uint64_t generation = mps->coordinator.generation;
	process_slot_events(mps);
	assert(mps->coordinator.generation == generation);
	assert(mps->shuffler.next == 1);
	obs_source_release(source);
}

static void test_shared_active_references(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	obs_source_inc_active(source);
	mps_activate(mps);
	uint64_t generation = mps->coordinator.generation;
	obs_source_inc_active(source);
	obs_source_dec_active(source);
	assert(obs_source_active(source));
	/* OBS does not deactivate while another active scene reference remains. */
	mps_activate(mps);
	assert(mps->coordinator.generation == generation);
	obs_source_dec_active(source);
	assert(!obs_source_active(source));
	mps_deactivate(mps);
	obs_source_inc_active(source);
	mps_activate(mps);
	assert(mps->coordinator.generation != generation);
	obs_source_dec_active(source);
	mps_deactivate(mps);
	obs_source_release(source);
}

static void test_deferred_activation_during_update(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps->resetting = true;
	mps_activate(mps);
	assert(mps->fresh_shuffle_pending);
	assert(!mps->activation_first_id);
	mps->resetting = false;
	activate_fresh_shuffle(mps);
	assert(!mps->fresh_shuffle_pending);
	assert(mps->activation_first_id);
	uint64_t generation = mps->coordinator.generation;
	activate_fresh_shuffle(mps);
	assert(mps->coordinator.generation == generation);
	obs_source_release(source);
}

static void test_previous_first_survives_playlist_rebuild(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps_activate(mps);
	char *first_id = bstrdup(mps->activation_first_id);
	obs_data_t *settings = obs_source_get_settings(source);
	mps_update(mps, settings);
	obs_data_release(settings);
	mps_deactivate(mps);
	mps_activate(mps);
	assert(strcmp(mps->activation_first_id, first_id) != 0);
	bfree(first_id);
	obs_source_release(source);
}

static void test_playlist_update_does_not_consume_prefetched_next(void)
{
	obs_source_t *source = create_playlist(4, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	char *expected_id = bstrdup(shuffler_peek_next(&mps->shuffler)->id);
	size_t next = mps->shuffler.next;
	obs_data_t *settings = obs_source_get_settings(source);
	mps_update(mps, settings);
	obs_data_release(settings);
	assert(mps->shuffler.next == next);
	assert(strcmp(shuffler_peek_next(&mps->shuffler)->id, expected_id) == 0);
	mps_playlist_next(mps);
	assert(strcmp(mps->actual_media->id, expected_id) == 0);
	bfree(expected_id);
	obs_source_release(source);
}

static void test_folder_activation_and_restart(void)
{
	obs_source_t *source = create_playlist(2, SHUFFLE_RESET_ON_ACTIVATE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	struct media_file_data *folder = &mps->files.array[0];
	folder->is_folder = true;
	for (size_t i = 0; i < 2; i++) {
		struct media_file_data *child = da_push_back_new(folder->folder_items);
		child->filename = bstrdup(i ? "second.mp4" : "first.mp4");
		child->path = bstrdup(child->filename);
		child->parent_id = folder->id;
		child->parent = folder;
		child->index = i;
	}
	shuffler_update_files(&mps->shuffler, &mps->files.da);
	mps->activation_first_id = bstrdup(folder->id);
	mps->activation_first_filename = bstrdup("first.mp4");
	mps_activate(mps);
	assert(mps->actual_media != &folder->folder_items.array[0]);
	assert(!mps->actual_media->is_folder);
	if (mps->actual_media->parent_id) {
		assert(mps->current_media == folder);
		assert(mps->current_folder_item_index == mps->actual_media->index);
	}
	mps->restart_behavior = RESTART_BEHAVIOR_FIRST_FILE;
	mps_restart(mps);
	assert(mps->actual_media == &folder->folder_items.array[0]);
	assert(mps->shuffler.shuffled_files.array[mps->shuffler.next - 1] == mps->actual_media);
	obs_source_release(source);
}

static void test_restart_first_keeps_shuffle_history_coherent(void)
{
	obs_source_t *source = create_playlist(4, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	select_index_proc_(mps, 2, 0);
	mps->restart_behavior = RESTART_BEHAVIOR_FIRST_FILE;
	mps_restart(mps);
	assert(mps->actual_media == &mps->files.array[0]);
	assert(mps->shuffler.shuffled_files.array[mps->shuffler.next - 1] == mps->actual_media);
	assert(shuffler_prev(&mps->shuffler) == &mps->files.array[2]);
	obs_source_release(source);
}

static void test_external_stable_selection_and_revision(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	struct mps_playlist_entries_snapshot page = {0};
	assert(mps_playlist_entries_page_get(source, 1, 1, &page));
	assert(page.item_count == 1 && page.total_count == 3 && page.playlist_revision > 0);
	uint64_t revision = page.playlist_revision;
	char *selected_id = bstrdup(page.items[0].stable_id);
	mps_playlist_entries_snapshot_free(&page);
	assert(mps_playlist_select_item(source, selected_id, revision));
	assert(mps->actual_media == &mps->files.array[1]);
	assert(mps->coordinator.request == MPS_SWITCH_SELECT);
	uint64_t generation = mps->coordinator.generation;
	assert(!mps_playlist_select_item(source, "missing", revision));
	assert(mps->coordinator.generation == generation);
	obs_data_t *settings = obs_source_get_settings(source);
	mps_update(mps, settings);
	obs_data_release(settings);
	assert(mps->playlist_revision > revision);
	generation = mps->coordinator.generation;
	assert(!mps_playlist_select_item(source, selected_id, revision));
	assert(mps->coordinator.generation == generation);
	assert(mps_playlist_select_item(source, selected_id, mps->playlist_revision));
	bfree(selected_id);
	obs_source_release(source);
}

static void test_external_selection_rejects_folder_header(void)
{
	obs_source_t *source = create_playlist(2, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps->files.array[0].is_folder = true;
	uint64_t generation = mps->coordinator.generation;
	assert(!mps_playlist_select_item(source, mps->files.array[0].id, 0));
	assert(mps->coordinator.generation == generation);
	mps->files.array[0].is_folder = false;
	obs_source_release(source);
}

static struct media_file_data *test_folder_source(obs_source_t **source, bool shuffle)
{
	*source = create_playlist(2, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(*source);
	cancel_pending_transition(mps, true, false);
	mps->shuffle = shuffle;
	mps->folder_watch = true;
	mps->folder_wait = false;
	mps->folder_sort = MPS_FOLDER_KEEP;
	mps->folder_generation = 7;
	struct media_file_data *folder = &mps->files.array[0];
	folder->is_folder = true;
	for (size_t i = 0; i < 3; ++i) {
		struct media_file_data *child = da_push_back_new(folder->folder_items);
		const char *name = i == 0 ? "a.mp4" : i == 1 ? "b.mp4" : "c.mp4";
		child->filename = bstrdup(name);
		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", folder->path, name);
		child->path = path.array;
		child->parent_id = folder->id;
		child->parent = folder;
		child->index = i;
	}
	mps->current_media = folder;
	mps->current_media_index = 0;
	mps->actual_media = &folder->folder_items.array[0];
	mps->current_folder_item_index = 0;
	bfree(mps->current_media_filename);
	mps->current_media_filename = bstrdup("a.mp4");
	shuffler_update_files(&mps->shuffler, &mps->files.da);
	if (shuffle)
		shuffler_select(&mps->shuffler, mps->actual_media);
	return folder;
}

static void test_folder_reconciliation_lifecycle(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "b.mp4", .size = 20},
					     {.name = "c.mp4", .size = 30}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 3};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	uint64_t revision = mps->playlist_revision;
	assert(!folder_apply_result(mps, &result));
	assert(mps->playlist_revision == revision);
	mps->folder_sort = MPS_FOLDER_ZA;
	mps->telemetry_cycle_exhausted = true;
	mps->telemetry_cycle_exhausted_generation = 42;
	assert(folder_apply_result(mps, &result));
	assert(strcmp(mps->actual_media->filename, "a.mp4") == 0);
	assert(mps->current_folder_item_index == 2);
	assert(mps->telemetry_cycle_exhausted);
	assert(mps->telemetry_cycle_exhausted_generation == 42);
	assert(mps->playlist_revision == revision + 1);
	assert(strcmp(mps->files.array[1].id, "item1") == 0);
	assert(!folder_apply_result(mps, &result));
	assert(mps->playlist_revision == revision + 1);
	/* A failed scan retains everything. A successful empty scan removes children. */
	scan.success = false;
	assert(!folder_apply_result(mps, &result));
	assert(mps->files.array[0].folder_items.num == 3);
	scan.success = true;
	scan.count = 0;
	assert(folder_apply_result(mps, &result));
	assert(mps->files.array[0].folder_items.num == 0);
	assert(mps->actual_media == mps->folder_orphan);
	assert(strcmp(mps->actual_media->filename, "a.mp4") == 0);
	assert(mps->coordinator.request_pending == false);
	result.generation = 6;
	scan.count = 3;
	assert(!folder_apply_result(mps, &result));
	assert(mps->files.array[0].folder_items.num == 0);
	obs_source_release(source);
}

static void test_folder_missing_current_next_and_shuffle(void)
{
	for (int shuffle = 0; shuffle <= 1; ++shuffle) {
		obs_source_t *source;
		struct media_file_data *folder = test_folder_source(&source, shuffle != 0);
		struct media_playlist_source *mps = obs_obj_get_data(source);
		struct mps_folder_entry entries[] = {{.name = "b.mp4", .size = 20}, {.name = "c.mp4", .size = 30}};
		struct mps_folder_snapshot scan = {.id = folder->id,
						   .path = folder->path,
						   .success = true,
						   .entries = entries,
						   .count = 2};
		struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
		uint64_t rng = mps->shuffler.rng_state;
		assert(folder_apply_result(mps, &result));
		assert(mps->shuffler.rng_state == rng);
		assert(mps->actual_media == mps->folder_orphan);
		assert(mps->current_folder_item_index == SIZE_MAX);
		mps_playlist_next(mps);
		assert(mps->actual_media != mps->folder_orphan);
		assert(strcmp(mps->actual_media->filename, "a.mp4") != 0);
		if (!shuffle)
			assert(strcmp(mps->actual_media->filename, "b.mp4") == 0);
		obs_source_release(source);
	}
}

static void test_folder_monitor_settings_do_not_reset_playlist(void)
{
	obs_source_t *source = create_playlist(3, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	obs_data_t *settings = obs_source_get_settings(source);
	/* Establish the same settings representation used by OBS updates. */
	mps_update(mps, settings);
	struct media_file_data *files = mps->files.array;
	uint64_t revision = mps->playlist_revision;
	uint64_t generation = mps->coordinator.generation;
	obs_data_set_bool(settings, S_FOLDER_WATCH, true);
	obs_data_set_string(settings, S_SELECT_FILE, "0");
	obs_data_set_bool(settings, S_FFMPEG_HW_DECODE, obs_data_get_bool(settings, S_FFMPEG_HW_DECODE));
	mps_update(mps, settings);
	assert(mps->folder_watch);
	assert(mps->files.array == files);
	assert(mps->playlist_revision == revision);
	assert(mps->coordinator.generation == generation);
	obs_data_set_bool(settings, S_FOLDER_WATCH, false);
	mps_update(mps, settings);
	assert(!mps->folder_watch);
	assert(mps->files.array == files);
	assert(mps->playlist_revision == revision);
	assert(mps->coordinator.generation == generation);
	obs_data_release(settings);
	obs_source_release(source);
}

static void *folder_fake_decoder_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(source);
	return bzalloc(1);
}

static uint32_t folder_fake_decoder_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 64;
}

static struct obs_source_info folder_fake_decoder = {
	.id = "ffmpeg_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = mps_getname,
	.get_width = folder_fake_decoder_size,
	.get_height = folder_fake_decoder_size,
	.create = folder_fake_decoder_create,
	.destroy = bfree,
};

static void test_folder_preserves_unaffected_decoder_and_preload(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	assert(create_media_slot(mps, mps->standby_slot, mps->actual_media, 0));
	mps->active_slot = mps->standby_slot;
	mps->standby_slot = &mps->media_slots[1];
	mps->active_slot->media_index = 0;
	prepare_next_media(mps);
	obs_source_t *active = mps->active_slot->source;
	obs_source_t *preloaded = mps->standby_slot->source;
	assert(active && preloaded);
	assert(mps->standby_slot->media_id);
	assert(strcmp(mps->standby_slot->folder_item_filename, "b.mp4") == 0);
	uint64_t preload_generation = mps->standby_slot->source_generation;
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "b.mp4", .size = 20},
					     {.name = "c.mp4", .size = 30},
					     {.name = "d.mp4", .size = 40}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 3};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	assert(!folder_apply_result(mps, &result));
	scan.count = 4;
	assert(folder_apply_result(mps, &result));
	assert(mps->active_slot->source == active);
	assert(mps->standby_slot->source == preloaded);
	assert(mps->standby_slot->source_generation == preload_generation);
	assert(!mps->prefetch_dirty);
	entries[1].size = 21;
	assert(folder_apply_result(mps, &result));
	assert(mps->active_slot->source == active);
	assert(!mps->standby_slot->source);
	assert(mps->prefetch_dirty);
	prepare_next_media(mps);
	assert(mps->standby_slot->source);
	assert(mps->standby_slot->source_generation != preload_generation);
	/* Removing the prepared next file also invalidates it, without cutting active. */
	entries[1] = entries[2];
	entries[2] = entries[3];
	scan.count = 3;
	assert(folder_apply_result(mps, &result));
	assert(!mps->standby_slot->source);
	assert(mps->active_slot->source == active);
	obs_source_release(source);
}

static void test_folder_orphan_successor_survives_further_changes(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps->actual_media = &folder->folder_items.array[1];
	mps->current_folder_item_index = 1;
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "c.mp4", .size = 30},
					     {.name = "d.mp4", .size = 40}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 2};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	assert(folder_apply_result(mps, &result));
	assert(mps->current_folder_item_index == 0);
	scan.count = 3;
	assert(folder_apply_result(mps, &result));
	assert(mps->current_folder_item_index == 0);
	entries[2].size = 41;
	assert(folder_apply_result(mps, &result));
	assert(mps->current_folder_item_index == 0);
	mps_playlist_next(mps);
	assert(strcmp(mps->actual_media->filename, "c.mp4") == 0);
	obs_source_release(source);
}

static void test_folder_settings_rebuild_preserves_known_children(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	folder->folder_items.array[0].monitor_stamp = true;
	folder->folder_items.array[0].monitor_size = 123;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_bool(settings, S_FOLDER_WATCH, true);
	obs_data_set_bool(settings, S_SHUFFLE, true);
	mps_update(mps, settings);
	assert(mps->files.array[0].is_folder);
	assert(mps->files.array[0].folder_items.num == 3);
	assert(mps->files.array[0].folder_items.array[0].monitor_stamp);
	assert(mps->files.array[0].folder_items.array[0].monitor_size == 123);
	obs_data_release(settings);
	obs_source_release(source);
}

static void test_folder_properties_visibility(void)
{
	obs_source_t *source = create_playlist(1, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	obs_data_t *settings = obs_source_get_settings(source);
	obs_properties_t *props = mps_properties(mps);
	assert(!obs_data_get_bool(settings, S_FOLDER_WATCH));
	assert(!obs_property_visible(obs_properties_get(props, S_FOLDER_SORT)));
	obs_data_set_bool(settings, S_FOLDER_WATCH, true);
	folder_watch_modified(props, NULL, settings);
	assert(obs_property_visible(obs_properties_get(props, S_FOLDER_SORT)));
	assert(obs_property_visible(obs_properties_get(props, S_FOLDER_INTERVAL)));
	assert(obs_property_visible(obs_properties_get(props, S_FOLDER_WAIT)));
	obs_properties_destroy(props);
	obs_data_release(settings);
	obs_source_release(source);
}

static void test_empty_watched_folder_becomes_selectable_without_autoplay(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	mps_folder_children_free(&folder->folder_items.da);
	mps->actual_media = NULL;
	mps->current_media = NULL;
	set_media_state(mps, OBS_MEDIA_STATE_STOPPED);
	struct mps_folder_entry entry = {.name = "first.mp4", .size = 10};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = &entry,
					   .count = 1};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	assert(folder_apply_result(mps, &result));
	assert(mps->actual_media && !mps->actual_media->is_folder);
	assert(strcmp(mps->actual_media->filename, "first.mp4") == 0);
	assert(!mps->coordinator.request_pending);
	assert(mps->state == OBS_MEDIA_STATE_STOPPED);
	obs_source_release(source);
}

static void test_unavailable_saved_folder_keeps_type(void)
{
	obs_source_t *source = create_playlist(1, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *folders = obs_data_array_create();
	obs_data_t *folder = obs_data_create();
	obs_data_set_string(folder, "uuid", "item0");
	obs_data_set_string(folder, "path", "clip0.mp4");
	obs_data_array_push_back(folders, folder);
	obs_data_release(folder);
	obs_data_set_array(settings, "folder_entries", folders);
	obs_data_array_release(folders);
	obs_data_set_bool(settings, S_FOLDER_WATCH, true);
	/* Force an ordinary update, as on initial source load. */
	obs_data_set_int(settings, S_SPEED, 101);
	mps_update(mps, settings);
	assert(mps->files.array[0].is_folder);
	assert(mps->files.array[0].folder_items.num == 0);
	obs_data_release(settings);
	obs_source_release(source);
}

static void test_legacy_unavailable_folder_recovers_and_persists_type(void)
{
	obs_source_t *source = create_playlist(1, SHUFFLE_RESET_PRESERVE);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_bool(settings, S_FOLDER_WATCH, true);
	assert(!mps->folder_watch);
	assert(mps->files.array[0].monitor_probe);
	mps_update(mps, settings);
	assert(mps->files.array[0].monitor_probe);
	assert(!mps->files.array[0].is_folder);
	cancel_pending_transition(mps, true, false);
	mps->folder_wait = false;
	struct mps_folder_entry entry = {.name = "returned.mp4", .size = 10};
	struct mps_folder_snapshot scan = {.id = mps->files.array[0].id,
					   .path = mps->files.array[0].path,
					   .success = true,
					   .entries = &entry,
					   .count = 1};
	struct mps_folder_result result = {.generation = mps->folder_generation, .folders = &scan, .count = 1};
	assert(folder_apply_result(mps, &result));
	assert(mps->files.array[0].is_folder);
	assert(!mps->files.array[0].monitor_probe);
	assert(mps->actual_media && !mps->actual_media->is_folder);
	assert(strcmp(mps->actual_media->filename, "returned.mp4") == 0);
	obs_data_array_t *saved = obs_data_get_array(settings, S_FOLDER_ENTRIES);
	assert(obs_data_array_count(saved) == 1);
	obs_data_array_release(saved);
	obs_source_t *reloaded =
		obs_source_create("media_playlist_source_codeyan", "offline folder reload", settings, NULL);
	assert(reloaded);
	struct media_playlist_source *other = obs_obj_get_data(reloaded);
	mps_update(other, settings);
	assert(other->files.array[0].is_folder);
	assert(other->files.array[0].folder_items.num == 0);
	obs_source_release(reloaded);
	obs_data_release(settings);
	obs_source_release(source);
}

struct folder_ui_update_test {
	obs_source_t *source;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	bool start;
	bool read_snapshot;
};

static void *send_folder_ui_updates(void *data)
{
	struct folder_ui_update_test *test = data;
	pthread_mutex_lock(&test->mutex);
	while (!test->start)
		pthread_cond_wait(&test->condition, &test->mutex);
	pthread_mutex_unlock(&test->mutex);
	for (size_t i = 0; i < 1000; ++i) {
		obs_data_t *settings = obs_data_create();
		if (test->read_snapshot) {
			struct media_playlist_source *mps = obs_obj_get_data(test->source);
			mps_save(mps, settings);
			assert(strstr(obs_data_get_string(settings, S_CURRENT_FILE_NAME), "a.mp4"));
			obs_properties_t *props = mps_properties(mps);
			obs_properties_destroy(props);
		} else {
			obs_data_set_bool(settings, S_FOLDER_WATCH, i % 2 != 0);
			obs_source_update(test->source, settings);
		}
		obs_data_release(settings);
	}
	return NULL;
}

static void test_folder_ui_updates_are_deferred_during_reconciliation(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "b.mp4", .size = 20},
					     {.name = "c.mp4", .size = 30}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 3};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	struct folder_ui_update_test test = {.source = source,
					     .mutex = PTHREAD_MUTEX_INITIALIZER,
					     .condition = PTHREAD_COND_INITIALIZER};
	pthread_t ui;
	assert(pthread_create(&ui, NULL, send_folder_ui_updates, &test) == 0);
	pthread_mutex_lock(&test.mutex);
	test.start = true;
	pthread_cond_signal(&test.condition);
	pthread_mutex_unlock(&test.mutex);
	for (size_t i = 0; i < 1000; ++i) {
		assert(!folder_apply_result(mps, &result));
		assert(mps->folder_generation == 7);
		assert(mps->folder_watch);
	}
	pthread_join(ui, NULL);
	/* OBS_SOURCE_VIDEO queues UI updates; only the video-thread dispatch applies them. */
	assert(mps->folder_generation == 7);
	obs_data_t *settings = obs_source_get_settings(source);
	folder_monitor_configure(mps, settings);
	assert(mps->folder_generation == 8);
	obs_data_release(settings);
	pthread_cond_destroy(&test.condition);
	pthread_mutex_destroy(&test.mutex);
	obs_source_release(source);
}

static void test_folder_properties_and_save_during_reconciliation(void)
{
	obs_source_t *source;
	struct media_file_data *folder = test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "b.mp4", .size = 20},
					     {.name = "c.mp4", .size = 30}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 3};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	struct folder_ui_update_test test = {.source = source,
					     .mutex = PTHREAD_MUTEX_INITIALIZER,
					     .condition = PTHREAD_COND_INITIALIZER,
					     .read_snapshot = true};
	pthread_t ui;
	assert(pthread_create(&ui, NULL, send_folder_ui_updates, &test) == 0);
	pthread_mutex_lock(&test.mutex);
	test.start = true;
	pthread_cond_signal(&test.condition);
	pthread_mutex_unlock(&test.mutex);
	for (size_t i = 0; i < 1000; ++i) {
		mps->folder_sort = i % 2 ? MPS_FOLDER_AZ : MPS_FOLDER_ZA;
		assert(folder_apply_result(mps, &result));
	}
	pthread_join(ui, NULL);
	pthread_cond_destroy(&test.condition);
	pthread_mutex_destroy(&test.mutex);
	obs_source_release(source);
}

static void test_folder_recovery_preserves_current_in_later_folder(void)
{
	obs_source_t *source;
	test_folder_source(&source, false);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	da_swap(mps->files, 0, 1);
	mps->files.array[0].index = 0;
	mps->files.array[1].index = 1;
	set_parents(&mps->files.da);
	struct media_file_data *folder = &mps->files.array[1];
	mps->current_media = folder;
	mps->current_media_index = 1;
	mps->actual_media = &folder->folder_items.array[0];
	struct mps_folder_entry entries[] = {{.name = "a.mp4", .size = 10},
					     {.name = "b.mp4", .size = 20},
					     {.name = "c.mp4", .size = 30},
					     {.name = "d.mp4", .size = 40}};
	struct mps_folder_snapshot scan = {.id = folder->id,
					   .path = folder->path,
					   .success = true,
					   .entries = entries,
					   .count = 4};
	struct mps_folder_result result = {.generation = 7, .folders = &scan, .count = 1};
	assert(folder_apply_result(mps, &result));
	assert(mps->current_media_index == 1);
	assert(mps->current_media == &mps->files.array[1]);
	assert(mps->actual_media->parent == mps->current_media);
	assert(mps->actual_media != mps->folder_orphan);
	assert(strcmp(mps->actual_media->filename, "a.mp4") == 0);
	obs_source_release(source);
}

static void destroy_queue_barrier(void *unused)
{
	UNUSED_PARAMETER(unused);
}

int main(void)
{
	assert(obs_startup("en-US", NULL, NULL));
	obs_register_source(&media_playlist_source_info);
	obs_register_source(&folder_fake_decoder);
	test_folder_recovery_preserves_current_in_later_folder();
	test_folder_properties_and_save_during_reconciliation();
	test_folder_ui_updates_are_deferred_during_reconciliation();
	test_legacy_unavailable_folder_recovers_and_persists_type();
	test_unavailable_saved_folder_keeps_type();
	test_empty_watched_folder_becomes_selectable_without_autoplay();
	test_folder_properties_visibility();
	test_folder_preserves_unaffected_decoder_and_preload();
	test_folder_orphan_successor_survives_further_changes();
	test_folder_settings_rebuild_preserves_known_children();
	test_folder_reconciliation_lifecycle();
	test_folder_missing_current_next_and_shuffle();
	test_folder_monitor_settings_do_not_reset_playlist();
	test_external_stable_selection_and_revision();
	test_external_selection_rejects_folder_header();
	test_compatibility_default();
	test_fresh_activation_and_redundant_callback();
	test_empty_single_and_shuffle_disabled();
	test_stale_end_and_pending_request_are_superseded();
	test_shared_active_references();
	test_deferred_activation_during_update();
	test_previous_first_survives_playlist_rebuild();
	test_playlist_update_does_not_consume_prefetched_next();
	test_folder_activation_and_restart();
	test_restart_first_keeps_shuffle_history_coherent();
	obs_queue_task(OBS_TASK_DESTROY, destroy_queue_barrier, NULL, true);
	mps_folder_monitor_shutdown();
	obs_shutdown();
	puts("shuffle activation tests passed");
	return 0;
}
