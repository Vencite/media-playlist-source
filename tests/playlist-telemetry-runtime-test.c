#ifdef NDEBUG
#undef NDEBUG
#endif

/* Headless runtime tests for mps_playlist_telemetry_get(). The real MPS
 * callbacks are linked through the source file below; libobs is started without
 * graphics or an audio device. A fake decoder registered as "ffmpeg_source"
 * replaces obs-ffmpeg so decoder clocks and states are deterministic, and the
 * duration inventory is an in-test cache so no file system probing runs. */
#include "../src/media-playlist-source.c"

#include <util/threading.h>

const char *obs_module_text(const char *text)
{
	return text;
}

void obs_log(int level, const char *format, ...)
{
	UNUSED_PARAMETER(level);
	UNUSED_PARAMETER(format);
}

/* ------------------------------------------------------------------------- */
/* Duration inventory stub: cache lookups only, like the production getter. */

struct mps_duration_inventory {
	int unused;
};

#define FAKE_DURATION_MAX 32

struct fake_duration {
	char *path;
	int64_t media_ms;
};

static struct fake_duration fake_durations[FAKE_DURATION_MAX];
static size_t fake_duration_count;

static void fake_durations_reset(void)
{
	for (size_t i = 0; i < fake_duration_count; ++i)
		bfree(fake_durations[i].path);
	fake_duration_count = 0;
}

static void fake_duration_set(const char *path, int64_t media_ms)
{
	for (size_t i = 0; i < fake_duration_count; ++i) {
		if (strcmp(fake_durations[i].path, path) == 0) {
			fake_durations[i].media_ms = media_ms;
			return;
		}
	}
	assert(fake_duration_count < FAKE_DURATION_MAX);
	fake_durations[fake_duration_count].path = bstrdup(path);
	fake_durations[fake_duration_count].media_ms = media_ms;
	++fake_duration_count;
}

struct mps_duration_inventory *mps_duration_inventory_create(void)
{
	return bzalloc(sizeof(struct mps_duration_inventory));
}

void mps_duration_inventory_destroy(struct mps_duration_inventory *inventory)
{
	bfree(inventory);
}

void mps_duration_inventory_set_paths(struct mps_duration_inventory *inventory, const char *const *paths, size_t count)
{
	UNUSED_PARAMETER(inventory);
	UNUSED_PARAMETER(paths);
	UNUSED_PARAMETER(count);
}

bool mps_duration_inventory_get(struct mps_duration_inventory *inventory, const char *path, int64_t *media_ms)
{
	UNUSED_PARAMETER(inventory);
	if (!path || !media_ms)
		return false;
	for (size_t i = 0; i < fake_duration_count; ++i) {
		if (strcmp(fake_durations[i].path, path) == 0) {
			*media_ms = fake_durations[i].media_ms;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Fake decoder registered under the id MPS uses for its private media slots. */

struct fake_decoder {
	int64_t time_ms;
	int64_t duration_ms;
	int64_t speed_percent;
	enum obs_media_state state;
};

static void *fake_decoder_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct fake_decoder *decoder = bzalloc(sizeof(*decoder));
	decoder->duration_ms = 60000;
	decoder->speed_percent = 100;
	decoder->state = OBS_MEDIA_STATE_NONE;
	if (settings)
		decoder->speed_percent = obs_data_get_int(settings, S_SPEED);
	return decoder;
}

static void fake_decoder_destroy(void *data)
{
	bfree(data);
}

static void fake_decoder_update(void *data, obs_data_t *settings)
{
	struct fake_decoder *decoder = data;
	decoder->speed_percent = obs_data_get_int(settings, S_SPEED);
}

static const char *fake_decoder_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "mps fake decoder";
}

static uint32_t fake_decoder_width(void *data)
{
	UNUSED_PARAMETER(data);
	return 640;
}

static uint32_t fake_decoder_height(void *data)
{
	UNUSED_PARAMETER(data);
	return 360;
}

static void fake_decoder_play_pause(void *data, bool pause)
{
	struct fake_decoder *decoder = data;
	decoder->state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
}

static void fake_decoder_restart(void *data)
{
	struct fake_decoder *decoder = data;
	decoder->time_ms = 0;
	decoder->state = OBS_MEDIA_STATE_PLAYING;
}

static void fake_decoder_stop(void *data)
{
	struct fake_decoder *decoder = data;
	decoder->state = OBS_MEDIA_STATE_STOPPED;
}

static void fake_decoder_set_time(void *data, int64_t ms)
{
	struct fake_decoder *decoder = data;
	decoder->time_ms = ms;
}

static int64_t fake_decoder_get_time(void *data)
{
	return ((struct fake_decoder *)data)->time_ms;
}

static int64_t fake_decoder_get_duration(void *data)
{
	return ((struct fake_decoder *)data)->duration_ms;
}

static enum obs_media_state fake_decoder_get_state(void *data)
{
	return ((struct fake_decoder *)data)->state;
}

static const struct obs_source_info fake_decoder_info = {
	.id = "ffmpeg_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = fake_decoder_name,
	.create = fake_decoder_create,
	.destroy = fake_decoder_destroy,
	.update = fake_decoder_update,
	.get_width = fake_decoder_width,
	.get_height = fake_decoder_height,
	.media_play_pause = fake_decoder_play_pause,
	.media_restart = fake_decoder_restart,
	.media_stop = fake_decoder_stop,
	.media_set_time = fake_decoder_set_time,
	.media_get_time = fake_decoder_get_time,
	.media_get_duration = fake_decoder_get_duration,
	.media_get_state = fake_decoder_get_state,
};

/* ------------------------------------------------------------------------- */
/* Playlist fixture helpers. */

static obs_source_t *create_playlist(const char *const *paths, const char *const *ids, size_t count, bool shuffle,
				     bool loop, int64_t speed)
{
	obs_data_t *settings = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();

	mps_defaults(settings);
	obs_data_set_bool(settings, S_SHUFFLE, shuffle);
	obs_data_set_bool(settings, S_LOOP, loop);
	obs_data_set_int(settings, S_SPEED, speed);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", paths[i]);
		obs_data_set_string(item, S_ID, ids[i]);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, S_PLAYLIST, items);
	obs_data_array_release(items);

	obs_source_t *source = obs_source_create("media_playlist_source_codeyan", "telemetry test", settings, NULL);
	assert(source);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	assert(mps);
	/* Source updates are normally dispatched by the OBS video thread. */
	mps_update(mps, settings);
	obs_data_release(settings);
	return source;
}

/* Drives the source through a real cut the way the OBS video thread would. */
static void pump(struct media_playlist_source *mps)
{
	for (int i = 0; i < 3; ++i)
		mps_video_tick(mps, 0.0f);
}

static struct fake_decoder *active_decoder(struct media_playlist_source *mps)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&mps->lifecycle_mutex);
	if (mps->active_slot && mps->active_slot->source)
		source = obs_source_get_ref(mps->active_slot->source);
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	assert(source);

	struct fake_decoder *decoder = obs_obj_get_data(source);
	obs_source_release(source);
	assert(decoder);
	return decoder;
}

static void set_current_clock(struct media_playlist_source *mps, int64_t time_ms, int64_t duration_ms)
{
	struct fake_decoder *decoder = active_decoder(mps);
	decoder->time_ms = time_ms;
	decoder->duration_ms = duration_ms;
}

static void get_snapshot(obs_source_t *source, struct mps_playlist_telemetry *snapshot)
{
	assert(mps_playlist_telemetry_get(source, snapshot));
}

static void assert_current_item(const struct mps_playlist_telemetry *snapshot, const char *id, const char *path)
{
	assert(snapshot->current_item_id);
	assert(strcmp(snapshot->current_item_id, id) == 0);
	assert(snapshot->current_path);
	assert(strcmp(snapshot->current_path, path) == 0);
}

/* ------------------------------------------------------------------------- */
/* Tests. */

static void test_sequential_timing_and_revision(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};

	fake_durations_reset();
	fake_duration_set("b.mp3", 10000);
	fake_duration_set("c.mp3", 10000);

	obs_source_t *source = create_playlist(paths, ids, 3, false, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 2000, 10000);

	struct mps_playlist_telemetry first;
	get_snapshot(source, &first);
	assert_current_item(&first, "id-a", "a.mp3");
	assert(first.state == OBS_MEDIA_STATE_PLAYING);
	assert(first.speed_percent == 100);
	assert(first.elapsed_media_ms == 2000);
	assert(first.duration_media_ms == 10000);
	assert(first.remaining_ms == 8000);
	assert(first.playlist_index == 0);
	assert(first.folder_item_index == SIZE_MAX);
	assert(first.items_remaining == 2);
	assert(first.playlist_remaining_known);
	assert(first.playlist_remaining_ms == 28000);
	assert(first.shuffle == false);
	assert(first.loop == true);
	assert(first.has_next);
	assert(first.snapshot_revision > 0);
	uint64_t revision = first.snapshot_revision;
	mps_playlist_telemetry_free(&first);

	/* An unchanged observation must not advance the revision. */
	struct mps_playlist_telemetry repeated;
	get_snapshot(source, &repeated);
	assert(repeated.snapshot_revision == revision);
	mps_playlist_telemetry_free(&repeated);

	/* Continuous clock drift never advances the revision: consumers detect
	 * discontinuities, not playback progress, from it. */
	struct fake_decoder *decoder = active_decoder(mps);
	decoder->time_ms = 2500;
	struct mps_playlist_telemetry same_second;
	get_snapshot(source, &same_second);
	assert(same_second.snapshot_revision == revision);
	mps_playlist_telemetry_free(&same_second);

	decoder->time_ms = 3000;
	struct mps_playlist_telemetry later;
	get_snapshot(source, &later);
	assert(later.snapshot_revision == revision);
	assert(later.elapsed_media_ms == 3000);
	assert(later.remaining_ms == 7000);
	mps_playlist_telemetry_free(&later);

	/* A seek is an explicit discontinuity even though the raw duration and
	 * the future aggregate do not change. */
	mps_set_time(mps, 1000);
	/* libobs may apply media controls on the source's video thread; apply the
	 * requested seek to the deterministic decoder as that thread would. */
	fake_decoder_set_time(decoder, 1000);
	struct mps_playlist_telemetry seeked;
	get_snapshot(source, &seeked);
	assert(seeked.snapshot_revision > revision);
	assert(seeked.elapsed_media_ms == 1000);
	assert(seeked.remaining_ms == 9000);
	revision = seeked.snapshot_revision;
	mps_playlist_telemetry_free(&seeked);

	obs_source_set_name(source, "Renamed playlist");
	struct mps_playlist_telemetry renamed;
	get_snapshot(source, &renamed);
	assert(renamed.snapshot_revision > revision);
	assert(strcmp(renamed.source_name, "Renamed playlist") == 0);
	mps_playlist_telemetry_free(&renamed);
	obs_source_release(source);
}

static void test_telemetry_copy_bounds(void)
{
	struct media_playlist_source mps = {0};
	mps.files.num = MPS_TELEMETRY_MAX_DURATION_LOOKUPS + 1;
	assert(!telemetry_pass_bounded(&mps));
	struct media_file_data folder = {0};
	folder.is_folder = true;
	mps.files.array = &folder;
	mps.files.num = 1;
	folder.folder_items.num = MPS_TELEMETRY_MAX_DURATION_LOOKUPS + 1;
	assert(!telemetry_pass_bounded(&mps));
	folder.folder_items.num = MPS_TELEMETRY_MAX_DURATION_LOOKUPS;
	assert(telemetry_pass_bounded(&mps));
}

static void test_pause_resume_and_seek_while_paused(void)
{
	const char *paths[] = {"a.mp3", "b.mp3"};
	const char *ids[] = {"id-a", "id-b"};

	fake_durations_reset();
	obs_source_t *source = create_playlist(paths, ids, 2, false, false, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 5000, 20000);

	mps_play_pause(mps, true);
	/* OBS may dispatch media controls on the video thread; the fake decoder
	 * applies the same request deterministically. */
	fake_decoder_play_pause(active_decoder(mps), true);
	struct mps_playlist_telemetry paused;
	get_snapshot(source, &paused);
	assert(paused.state == OBS_MEDIA_STATE_PAUSED);
	assert(paused.elapsed_media_ms == 5000);
	assert(paused.remaining_ms == 15000);
	uint64_t paused_revision = paused.snapshot_revision;
	mps_playlist_telemetry_free(&paused);

	/* A seek while paused must be visible immediately, not after a stale
	 * frozen value is released. */
	mps_video_tick(mps, 0.0f); /* Establish the applied paused clock. */
	mps_set_time(mps, 12000);
	struct mps_playlist_telemetry queued;
	get_snapshot(source, &queued);
	assert(queued.elapsed_media_ms == 5000); /* Do not guess a queued cursor. */
	uint64_t queued_revision = queued.snapshot_revision;
	mps_playlist_telemetry_free(&queued);
	uint64_t notified = mps->telemetry_discontinuity;
	fake_decoder_set_time(active_decoder(mps), 12000);
	mps_video_tick(mps, 0.0f);
	assert(mps->telemetry_discontinuity > notified); /* Wake even while paused. */
	notified = mps->telemetry_discontinuity;
	mps_video_tick(mps, 0.0f);
	assert(mps->telemetry_discontinuity == notified); /* No identical-state spam. */
	struct mps_playlist_telemetry seeked;
	get_snapshot(source, &seeked);
	assert(seeked.state == OBS_MEDIA_STATE_PAUSED);
	assert(seeked.elapsed_media_ms == 12000);
	assert(seeked.remaining_ms == 8000);
	assert(seeked.snapshot_revision > paused_revision);
	assert(seeked.snapshot_revision > queued_revision);
	uint64_t seeked_revision = seeked.snapshot_revision;
	mps_playlist_telemetry_free(&seeked);

	struct mps_playlist_telemetry seeked_again;
	get_snapshot(source, &seeked_again);
	assert(seeked_again.elapsed_media_ms == 12000);
	assert(seeked_again.snapshot_revision == seeked_revision);
	mps_playlist_telemetry_free(&seeked_again);

	mps_play_pause(mps, false);
	fake_decoder_play_pause(active_decoder(mps), false);
	struct mps_playlist_telemetry resumed;
	get_snapshot(source, &resumed);
	assert(resumed.state == OBS_MEDIA_STATE_PLAYING);
	assert(resumed.elapsed_media_ms == 12000);
	mps_playlist_telemetry_free(&resumed);

	obs_source_release(source);
}

static void test_pending_selection_keeps_committed_identity(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};

	fake_durations_reset();
	fake_duration_set("b.mp3", 10000);
	fake_duration_set("c.mp3", 10000);

	obs_source_t *source = create_playlist(paths, ids, 3, false, false, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 4000, 20000);

	/* The request selects c, but the committed decoder is still a. */
	select_index_proc_(mps, 2, 0);
	/* The parent paused the committed child; OBS dispatches that on the video
	 * thread, so apply it to the fake decoder here. */
	fake_decoder_play_pause(active_decoder(mps), true);
	assert(mps->actual_media);
	assert(strcmp(mps->actual_media->id, "id-c") == 0);

	struct mps_playlist_telemetry pending;
	get_snapshot(source, &pending);
	assert_current_item(&pending, "id-a", "a.mp3");
	assert(pending.playlist_index == 0);
	assert(pending.elapsed_media_ms == 4000);
	assert(pending.state == OBS_MEDIA_STATE_PAUSED);
	mps_playlist_telemetry_free(&pending);

	pump(mps);
	set_current_clock(mps, 0, 30000);
	struct mps_playlist_telemetry cut;
	get_snapshot(source, &cut);
	assert_current_item(&cut, "id-c", "c.mp3");
	assert(cut.playlist_index == 2);
	assert(cut.elapsed_media_ms == 0);
	assert(cut.remaining_ms == 30000);
	assert(cut.items_remaining == 0);
	assert(cut.playlist_remaining_known);
	assert(cut.playlist_remaining_ms == 30000);
	mps_playlist_telemetry_free(&cut);

	obs_source_release(source);
}

static void create_temp_media_folder(char *folder, size_t folder_size, char *dup_path, size_t dup_size)
{
	const char *base = getenv("TMPDIR");
	char child_path[1200];

	if (!base || !*base)
		base = getenv("TEMP");
	if (!base || !*base)
		base = ".";
	snprintf(folder, folder_size, "%s/mps-telemetry-%llu", base, (unsigned long long)os_gettime_ns());
	assert(os_mkdir(folder) == 0);

	snprintf(child_path, sizeof(child_path), "%s/one.mp3", folder);
	FILE *file = fopen(child_path, "wb");
	assert(file);
	fclose(file);
	snprintf(child_path, sizeof(child_path), "%s/two.mp3", folder);
	file = fopen(child_path, "wb");
	assert(file);
	fclose(file);

	snprintf(dup_path, dup_size, "%s/dup.mp3", folder);
}

static void remove_temp_media_folder(const char *folder, const char *dup_path)
{
	char child_path[1200];

	snprintf(child_path, sizeof(child_path), "%s/one.mp3", folder);
	os_unlink(child_path);
	snprintf(child_path, sizeof(child_path), "%s/two.mp3", folder);
	os_unlink(child_path);
	os_unlink(dup_path);
	os_rmdir(folder);
}

static void test_sequential_folder_duplicates_and_loop(void)
{
	char folder[1024];
	char dup_path[1200];
	char child_one[1200];
	char child_two[1200];

	create_temp_media_folder(folder, sizeof(folder), dup_path, sizeof(dup_path));
	snprintf(child_one, sizeof(child_one), "%s/one.mp3", folder);
	snprintf(child_two, sizeof(child_two), "%s/two.mp3", folder);

	const char *paths[] = {folder, dup_path, dup_path};
	const char *ids[] = {"id-folder", "id-dup-one", "id-dup-two"};

	fake_durations_reset();
	fake_duration_set(child_two, 20000);
	fake_duration_set(dup_path, 5000);

	obs_source_t *source = create_playlist(paths, ids, 3, false, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 0, 10000);

	struct mps_playlist_telemetry snapshot;
	get_snapshot(source, &snapshot);
	assert(snapshot.current_item_id);
	assert(strcmp(snapshot.current_item_id, "id-folder/one.mp3") == 0);
	assert(snapshot.playlist_index == 0);
	assert(snapshot.folder_item_index == 0);
	assert(snapshot.items_remaining == 3);
	assert(snapshot.playlist_remaining_known);
	/* Current 10000 + two.mp3 20000 + dup 5000 + duplicate entry 5000. Loop
	 * on must not add another pass. */
	assert(snapshot.playlist_remaining_ms == 40000);
	assert(snapshot.remaining_ms == 10000);
	mps_playlist_telemetry_free(&snapshot);

	obs_source_release(source);
	remove_temp_media_folder(folder, dup_path);
}

static void test_shuffle_query_is_read_only(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3", "d.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c", "id-d"};

	fake_durations_reset();
	for (size_t i = 0; i < 4; ++i)
		fake_duration_set(paths[i], 10000);

	obs_source_t *source = create_playlist(paths, ids, 4, true, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 0, 10000);

	struct media_file_data *committed = mps->actual_media;
	size_t head = mps->shuffler.head;
	size_t next = mps->shuffler.next;
	size_t history = mps->shuffler.history;
	size_t count = mps->shuffler.shuffled_files.num;
	uint64_t rng_state = mps->shuffler.rng_state;
	assert(next > 0 && next <= count);
	char *expected_next_id = bstrdup(mps->shuffler.shuffled_files.array[next]->id);

	struct mps_playlist_telemetry snapshot;
	get_snapshot(source, &snapshot);
	assert_current_item(&snapshot, committed->id, committed->path);
	assert(snapshot.shuffle);
	assert(snapshot.items_remaining == 3);
	assert(snapshot.playlist_remaining_ms == 40000);
	mps_playlist_telemetry_free(&snapshot);

	get_snapshot(source, &snapshot);
	mps_playlist_telemetry_free(&snapshot);

	assert(mps->shuffler.head == head);
	assert(mps->shuffler.next == next);
	assert(mps->shuffler.history == history);
	assert(mps->shuffler.shuffled_files.num == count);
	assert(mps->shuffler.rng_state == rng_state);
	assert(mps->actual_media == committed);

	/* The prepared next item still plays next. */
	mps_playlist_next(mps);
	assert(mps->actual_media);
	assert(strcmp(mps->actual_media->id, expected_next_id) == 0);
	bfree(expected_next_id);

	obs_source_release(source);
}

static void test_shuffle_last_prepeek_reports_empty_pass(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};

	fake_durations_reset();
	for (size_t i = 0; i < 3; ++i)
		fake_duration_set(paths[i], 10000);

	obs_source_t *source = create_playlist(paths, ids, 3, true, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);

	for (int i = 0; i < 1; ++i) {
		mps_playlist_next(mps);
		pump(mps);
	}
	mps_playlist_next(mps);
	pump(mps);

	/* The successful cut of the last cycle item runs prepare_next_media(),
	 * which auto-reshuffles and moves the cursor to 0 for the next cycle. */
	assert(mps->shuffler.next == 0);
	set_current_clock(mps, 0, 10000);

	struct mps_playlist_telemetry snapshot;
	get_snapshot(source, &snapshot);
	assert(snapshot.shuffle);
	assert(snapshot.items_remaining == 0);
	assert(snapshot.playlist_remaining_known);
	assert(snapshot.playlist_remaining_ms == 10000);
	assert(snapshot.remaining_ms == 10000);
	mps_playlist_telemetry_free(&snapshot);

	obs_source_release(source);
}

static void test_no_active_item_and_unknown_future_durations(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};

	fake_durations_reset();
	obs_source_t *source = create_playlist(paths, ids, 3, false, false, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);

	/* No cut has happened yet: no committed item, no fabricated clock. */
	struct mps_playlist_telemetry inactive;
	get_snapshot(source, &inactive);
	assert(inactive.current_item_id == NULL);
	assert(inactive.current_path == NULL);
	assert(inactive.elapsed_media_ms == -1);
	assert(inactive.duration_media_ms == -1);
	assert(inactive.remaining_ms == -1);
	assert(inactive.playlist_remaining_known == false);
	assert(inactive.playlist_remaining_ms == -1);
	assert(inactive.playlist_index == SIZE_MAX);
	assert(inactive.items_remaining == SIZE_MAX);
	assert(inactive.state == OBS_MEDIA_STATE_NONE);
	mps_playlist_telemetry_free(&inactive);

	/* Current item is known, later durations are not. */
	pump(mps);
	set_current_clock(mps, 1000, 10000);
	struct mps_playlist_telemetry partial;
	get_snapshot(source, &partial);
	assert_current_item(&partial, "id-a", "a.mp3");
	assert(partial.remaining_ms == 9000);
	assert(partial.items_remaining == 2);
	assert(partial.playlist_remaining_known == false);
	assert(partial.playlist_remaining_ms == -1);
	uint64_t unknown_revision = partial.snapshot_revision;
	mps_playlist_telemetry_free(&partial);

	/* The duration inventory completing changes the future aggregate and must
	 * advance the revision even though the current clock did not move. */
	fake_duration_set("b.mp3", 10000);
	fake_duration_set("c.mp3", 10000);
	struct mps_playlist_telemetry completed;
	get_snapshot(source, &completed);
	assert(completed.playlist_remaining_known);
	assert(completed.playlist_remaining_ms == 29000);
	assert(completed.snapshot_revision > unknown_revision);
	uint64_t completed_revision = completed.snapshot_revision;
	mps_playlist_telemetry_free(&completed);

	struct mps_playlist_telemetry completed_again;
	get_snapshot(source, &completed_again);
	assert(completed_again.snapshot_revision == completed_revision);
	mps_playlist_telemetry_free(&completed_again);

	/* A revalidated duration changes the future aggregate and must advance the
	 * revision even with a still current clock. */
	fake_duration_set("b.mp3", 15000);
	struct mps_playlist_telemetry revalidated;
	get_snapshot(source, &revalidated);
	assert(revalidated.playlist_remaining_ms == 34000);
	assert(revalidated.snapshot_revision > completed_revision);
	mps_playlist_telemetry_free(&revalidated);

	/* A failed decoder keeps its committed identity but reports no playing
	 * clock and no fabricated timing. */
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->active_slot->failed = true;
	pthread_mutex_unlock(&mps->lifecycle_mutex);
	struct mps_playlist_telemetry failed;
	get_snapshot(source, &failed);
	assert_current_item(&failed, "id-a", "a.mp3");
	assert(failed.state == OBS_MEDIA_STATE_NONE);
	assert(failed.elapsed_media_ms == -1);
	assert(failed.duration_media_ms == -1);
	assert(failed.remaining_ms == -1);
	assert(failed.playlist_remaining_known == false);
	mps_playlist_telemetry_free(&failed);
	pthread_mutex_lock(&mps->lifecycle_mutex);
	mps->active_slot->failed = false;
	pthread_mutex_unlock(&mps->lifecycle_mutex);

	obs_source_release(source);
}

static void test_speed_units_and_url_entries(void)
{
	const char *paths[] = {"a.mp3", "https://example.com/live.mp3"};
	const char *ids[] = {"id-a", "id-url"};

	fake_durations_reset();
	fake_duration_set("a.mp3", 8000);
	fake_duration_set("https://example.com/live.mp3", 10000);

	obs_source_t *source = create_playlist(paths, ids, 2, false, false, 200);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 2000, 10000);

	struct mps_playlist_telemetry local;
	get_snapshot(source, &local);
	assert_current_item(&local, "id-a", "a.mp3");
	assert(local.speed_percent == 200);
	assert(local.remaining_ms == 4000);
	assert(local.items_remaining == 1);
	/* A URL has no authoritative file duration even if the inventory cache
	 * happens to hold an entry for its path. */
	assert(local.playlist_remaining_known == false);
	assert(local.playlist_remaining_ms == -1);
	mps_playlist_telemetry_free(&local);

	select_index_proc_(mps, 1, 0);
	pump(mps);
	set_current_clock(mps, 1000, 5000);
	struct mps_playlist_telemetry url;
	get_snapshot(source, &url);
	assert_current_item(&url, "id-url", "https://example.com/live.mp3");
	assert(url.speed_percent == 100);
	assert(url.remaining_ms == 4000);
	mps_playlist_telemetry_free(&url);

	obs_source_release(source);
}

static obs_data_t *settings_with_playlist(const char *const *paths, const char *const *ids, size_t count, bool loop)
{
	obs_data_t *settings = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();

	mps_defaults(settings);
	obs_data_set_bool(settings, S_SHUFFLE, false);
	obs_data_set_bool(settings, S_LOOP, loop);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", paths[i]);
		obs_data_set_string(item, S_ID, ids[i]);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, S_PLAYLIST, items);
	obs_data_array_release(items);
	return settings;
}

static void test_stop_and_absent_source_uuid(void)
{
	const char *paths[] = {"a.mp3", "b.mp3"};
	const char *ids[] = {"id-a", "id-b"};

	fake_durations_reset();
	fake_duration_set("b.mp3", 20000);
	obs_source_t *source = create_playlist(paths, ids, 2, false, false, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 5000, 20000);

	/* An absent or empty identifier must stay NULL instead of being
	 * fabricated as an empty string. */
	char *saved_uuid = mps->source_uuid;
	mps->source_uuid = NULL;
	struct mps_playlist_telemetry no_uuid;
	get_snapshot(source, &no_uuid);
	assert(no_uuid.source_uuid == NULL);
	assert(no_uuid.source_name && no_uuid.source_name[0]);
	mps_playlist_telemetry_free(&no_uuid);
	mps->source_uuid = bstrdup("");
	get_snapshot(source, &no_uuid);
	assert(no_uuid.source_uuid == NULL);
	mps_playlist_telemetry_free(&no_uuid);
	bfree(mps->source_uuid);
	mps->source_uuid = saved_uuid;

	mps_stop(mps);
	/* libobs may dispatch the stop on the video thread. */
	fake_decoder_stop(active_decoder(mps));
	struct mps_playlist_telemetry stopped;
	get_snapshot(source, &stopped);
	assert(stopped.state == OBS_MEDIA_STATE_STOPPED);
	assert(stopped.elapsed_media_ms == 5000);
	assert(stopped.remaining_ms == 15000);
	assert(stopped.playlist_remaining_known);
	assert(stopped.playlist_remaining_ms == 35000);
	mps_playlist_telemetry_free(&stopped);

	obs_source_release(source);
}

struct reader_thread {
	obs_source_t *source;
	int failures;
};

static void *reader_thread_main(void *data)
{
	struct reader_thread *reader = data;
	uint64_t previous_revision = 0;

	for (int i = 0; i < 200; ++i) {
		struct mps_playlist_telemetry snapshot;

		if (!mps_playlist_telemetry_get(reader->source, &snapshot)) {
			++reader->failures;
			continue;
		}
		if (snapshot.snapshot_revision < previous_revision)
			++reader->failures;
		previous_revision = snapshot.snapshot_revision;
		mps_playlist_telemetry_free(&snapshot);
	}
	return NULL;
}

static void test_cut_while_observing(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3", "d.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c", "id-d"};
	enum { READERS = 2, CUTS = 40 };

	fake_durations_reset();
	for (size_t i = 0; i < 4; ++i)
		fake_duration_set(paths[i], 10000);

	obs_source_t *source = create_playlist(paths, ids, 4, true, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 1000, 10000);

	pthread_t threads[READERS];
	struct reader_thread readers[READERS];
	for (size_t i = 0; i < READERS; ++i) {
		readers[i] = (struct reader_thread){.source = source, .failures = 0};
		assert(pthread_create(&threads[i], NULL, reader_thread_main, &readers[i]) == 0);
	}
	for (int i = 0; i < CUTS; ++i) {
		mps_playlist_next(mps);
		pump(mps);
	}
	for (size_t i = 0; i < READERS; ++i) {
		pthread_join(threads[i], NULL);
		/* A read racing a cut may legitimately return no snapshot; it must not
		 * publish a partial one or stay stuck. */
		assert(readers[i].failures < 150);
	}

	struct mps_playlist_telemetry snapshot;
	get_snapshot(source, &snapshot);
	assert(snapshot.current_path != NULL);
	mps_playlist_telemetry_free(&snapshot);

	obs_source_release(source);
}

static void test_source_edit_and_removal(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};
	const char *kept_paths[] = {"b.mp3", "c.mp3"};
	const char *kept_ids[] = {"id-b", "id-c"};

	fake_durations_reset();
	obs_source_t *source = create_playlist(paths, ids, 3, false, false, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 1000, 10000);

	/* Remove the actively playing entry from the playlist. */
	obs_data_t *settings = settings_with_playlist(kept_paths, kept_ids, 2, false);
	mps_update(mps, settings);
	obs_data_release(settings);

	struct mps_playlist_telemetry removed;
	get_snapshot(source, &removed);
	assert_current_item(&removed, "id-a", "a.mp3");
	assert(removed.elapsed_media_ms == 1000);
	assert(removed.playlist_index == SIZE_MAX);
	assert(removed.folder_item_index == SIZE_MAX);
	assert(removed.items_remaining == SIZE_MAX);
	assert(removed.playlist_remaining_known == false);
	mps_playlist_telemetry_free(&removed);

	/* The queued switch still lands on a valid entry. */
	pump(mps);
	fake_duration_set("c.mp3", 20000);
	set_current_clock(mps, 0, 20000);
	struct mps_playlist_telemetry rebound;
	get_snapshot(source, &rebound);
	assert_current_item(&rebound, "id-b", "b.mp3");
	assert(rebound.playlist_index == 0);
	assert(rebound.items_remaining == 1);
	assert(rebound.playlist_remaining_known);
	assert(rebound.playlist_remaining_ms == 40000);
	mps_playlist_telemetry_free(&rebound);

	/* Emptying the playlist removes the committed item without stale clocks. */
	obs_data_t *empty = settings_with_playlist(NULL, NULL, 0, false);
	mps_update(mps, empty);
	obs_data_release(empty);

	struct mps_playlist_telemetry empty_snapshot;
	get_snapshot(source, &empty_snapshot);
	assert(empty_snapshot.current_item_id == NULL);
	assert(empty_snapshot.current_path == NULL);
	assert(empty_snapshot.elapsed_media_ms == -1);
	assert(empty_snapshot.remaining_ms == -1);
	assert(empty_snapshot.playlist_remaining_known == false);
	mps_playlist_telemetry_free(&empty_snapshot);

	obs_source_release(source);
}

static void test_concurrent_observers(void)
{
	const char *paths[] = {"a.mp3", "b.mp3", "c.mp3"};
	const char *ids[] = {"id-a", "id-b", "id-c"};
	enum { READERS = 4 };

	fake_durations_reset();
	for (size_t i = 0; i < 3; ++i)
		fake_duration_set(paths[i], 10000);

	obs_source_t *source = create_playlist(paths, ids, 3, true, true, 100);
	struct media_playlist_source *mps = obs_obj_get_data(source);
	pump(mps);
	set_current_clock(mps, 1000, 10000);

	pthread_t threads[READERS];
	struct reader_thread readers[READERS];
	for (size_t i = 0; i < READERS; ++i) {
		readers[i] = (struct reader_thread){.source = source, .failures = 0};
		assert(pthread_create(&threads[i], NULL, reader_thread_main, &readers[i]) == 0);
	}
	for (size_t i = 0; i < READERS; ++i) {
		pthread_join(threads[i], NULL);
		assert(readers[i].failures == 0);
	}

	obs_source_release(source);
}

int main(void)
{
	assert(obs_startup("en-US", NULL, NULL));

	struct obs_audio_info audio = {0};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	assert(obs_reset_audio(&audio));

	obs_register_source(&media_playlist_source_info);
	obs_register_source(&fake_decoder_info);

	test_sequential_timing_and_revision();
	test_telemetry_copy_bounds();
	test_pause_resume_and_seek_while_paused();
	test_pending_selection_keeps_committed_identity();
	test_sequential_folder_duplicates_and_loop();
	test_shuffle_query_is_read_only();
	test_shuffle_last_prepeek_reports_empty_pass();
	test_no_active_item_and_unknown_future_durations();
	test_speed_units_and_url_entries();
	test_stop_and_absent_source_uuid();
	test_source_edit_and_removal();
	test_concurrent_observers();
	test_cut_while_observing();

	fake_durations_reset();
	obs_shutdown();
	puts("playlist telemetry runtime tests passed");
	return 0;
}
