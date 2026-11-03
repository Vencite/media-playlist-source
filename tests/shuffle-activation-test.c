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

int main(void)
{
	assert(obs_startup("en-US", NULL, NULL));
	obs_register_source(&media_playlist_source_info);
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
	obs_shutdown();
	puts("shuffle activation tests passed");
	return 0;
}
