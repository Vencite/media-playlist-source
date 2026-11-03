#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/bmem.h>

#include "playlist-telemetry.h"

#define CHECK(condition)                                                                                \
	do {                                                                                            \
		if (!(condition)) {                                                                     \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE);                                                             \
		}                                                                                       \
	} while (false)

static struct media_file_data make_file(const char *path, const char *id, size_t index)
{
	struct media_file_data media = {0};
	media.path = (char *)path;
	media.id = (char *)id;
	media.index = index;
	return media;
}

static void set_folder(struct media_file_data *folder, const char *path, const char *id, size_t index,
		       struct media_file_data *children, size_t child_count)
{
	folder->path = (char *)path;
	folder->id = (char *)id;
	folder->index = index;
	folder->is_folder = true;
	folder->folder_items.array = children;
	folder->folder_items.num = child_count;
	folder->folder_items.capacity = child_count;
}

static void set_folder_child(struct media_file_data *child, const char *path, const char *filename,
			     const char *parent_id, struct media_file_data *parent, size_t index)
{
	child->path = (char *)path;
	child->filename = (char *)filename;
	child->parent_id = parent_id;
	child->parent = parent;
	child->index = index;
}

static bool has_key(const char *json, const char *key)
{
	char pattern[128];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	return strstr(json, pattern) != NULL;
}

static void check_always_present_keys(const char *json)
{
	static const char *keys[] = {
		"schema_version",        "snapshot_revision", "state", "remaining_known", "playlist_remaining_known",
		"items_remaining_known", "shuffle",           "loop",  "has_next",        "has_previous",
	};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		CHECK(has_key(json, keys[i]));
}

static void check_no_unknown_wire_markers(const char *json)
{
	CHECK(strstr(json, "null") == NULL);
	CHECK(strstr(json, "{}") == NULL);
	CHECK(strstr(json, ":-1") == NULL);
}

static void test_init_defaults(void)
{
	struct mps_playlist_telemetry snapshot;

	mps_playlist_telemetry_init(&snapshot);
	CHECK(snapshot.source_uuid == NULL);
	CHECK(snapshot.source_name == NULL);
	CHECK(snapshot.current_item_id == NULL);
	CHECK(snapshot.current_path == NULL);
	CHECK(snapshot.current_file == NULL);
	CHECK(snapshot.snapshot_revision == 0);
	CHECK(snapshot.state == OBS_MEDIA_STATE_NONE);
	CHECK(snapshot.speed_percent == -1);
	CHECK(snapshot.elapsed_media_ms == -1);
	CHECK(snapshot.duration_media_ms == -1);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(!snapshot.shuffle);
	CHECK(!snapshot.loop);
	CHECK(!snapshot.has_next);
	CHECK(!snapshot.has_previous);
	CHECK(snapshot.playlist_index == SIZE_MAX);
	CHECK(snapshot.folder_item_index == SIZE_MAX);
	CHECK(snapshot.items_remaining == SIZE_MAX);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_json_empty_snapshot_omits_unknowns(void)
{
	struct mps_playlist_telemetry snapshot;
	obs_data_t *data;
	const char *json;

	mps_playlist_telemetry_init(&snapshot);
	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	check_always_present_keys(json);
	check_no_unknown_wire_markers(json);
	CHECK(strstr(json, "\"schema_version\":1") != NULL);
	CHECK(obs_data_get_int(data, "snapshot_revision") == 0);
	CHECK(strcmp(obs_data_get_string(data, "state"), "unknown") == 0);
	CHECK(!has_key(json, "source_uuid"));
	CHECK(!has_key(json, "source_name"));
	CHECK(!has_key(json, "current_item_id"));
	CHECK(!has_key(json, "current_path"));
	CHECK(!has_key(json, "current_file"));
	CHECK(!has_key(json, "playlist_index"));
	CHECK(!has_key(json, "folder_item_index"));
	CHECK(!has_key(json, "elapsed_media_ms"));
	CHECK(!has_key(json, "duration_media_ms"));
	CHECK(!has_key(json, "remaining_ms"));
	CHECK(!has_key(json, "playlist_remaining_ms"));
	CHECK(!has_key(json, "items_remaining"));
	CHECK(!has_key(json, "speed_percent"));
	CHECK(!obs_data_get_bool(data, "remaining_known"));
	CHECK(!obs_data_get_bool(data, "playlist_remaining_known"));
	CHECK(!obs_data_get_bool(data, "items_remaining_known"));
	obs_data_release(data);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_json_roundtrip_and_escaping(void)
{
	struct mps_playlist_telemetry snapshot;
	obs_data_t *data;
	obs_data_t *parsed;
	const char *json;
	const char *path = "dir/a\"b\\c\n.mp4";

	mps_playlist_telemetry_init(&snapshot);
	snapshot.source_uuid = bstrdup("uuid-1");
	snapshot.source_name = bstrdup("VT_PLAYLIST");
	snapshot.current_item_id = bstrdup("entry-uuid");
	snapshot.current_path = bstrdup(path);
	snapshot.current_file = bstrdup("a.mp4");
	snapshot.snapshot_revision = 28;
	snapshot.state = OBS_MEDIA_STATE_PLAYING;
	snapshot.speed_percent = 200;
	snapshot.elapsed_media_ms = 42000;
	snapshot.duration_media_ms = 180000;
	snapshot.remaining_ms = 69000;
	snapshot.playlist_remaining_ms = 100000;
	snapshot.playlist_remaining_known = true;
	snapshot.shuffle = true;
	snapshot.loop = true;
	snapshot.has_next = true;
	snapshot.has_previous = false;
	snapshot.playlist_index = 0;
	snapshot.folder_item_index = SIZE_MAX;
	snapshot.items_remaining = 3;

	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	check_always_present_keys(json);
	check_no_unknown_wire_markers(json);
	CHECK(has_key(json, "source_uuid"));
	CHECK(has_key(json, "source_name"));
	CHECK(has_key(json, "current_item_id"));
	CHECK(has_key(json, "current_path"));
	CHECK(has_key(json, "current_file"));
	CHECK(has_key(json, "playlist_index"));
	CHECK(!has_key(json, "folder_item_index"));
	CHECK(has_key(json, "elapsed_media_ms"));
	CHECK(has_key(json, "duration_media_ms"));
	CHECK(has_key(json, "remaining_ms"));
	CHECK(has_key(json, "playlist_remaining_ms"));
	CHECK(has_key(json, "items_remaining"));
	CHECK(has_key(json, "speed_percent"));
	CHECK(obs_data_get_bool(data, "remaining_known"));
	CHECK(obs_data_get_bool(data, "playlist_remaining_known"));
	CHECK(obs_data_get_bool(data, "items_remaining_known"));
	CHECK(strcmp(obs_data_get_string(data, "current_path"), path) == 0);
	CHECK(obs_data_get_int(data, "snapshot_revision") == 28);
	CHECK(obs_data_get_int(data, "elapsed_media_ms") == 42000);
	CHECK(obs_data_get_bool(data, "shuffle"));

	parsed = obs_data_create_from_json(json);
	CHECK(parsed != NULL);
	CHECK(strcmp(obs_data_get_string(parsed, "current_path"), path) == 0);
	CHECK(strcmp(obs_data_get_string(parsed, "source_uuid"), "uuid-1") == 0);
	CHECK(obs_data_get_int(parsed, "playlist_remaining_ms") == 100000);
	CHECK(obs_data_get_int(parsed, "speed_percent") == 200);
	CHECK(obs_data_get_bool(parsed, "remaining_known"));
	CHECK(obs_data_get_bool(parsed, "playlist_remaining_known"));
	CHECK(obs_data_get_bool(parsed, "items_remaining_known"));
	obs_data_release(parsed);
	obs_data_release(data);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_json_zero_values_are_known(void)
{
	struct mps_playlist_telemetry snapshot;
	obs_data_t *data;
	const char *json;

	mps_playlist_telemetry_init(&snapshot);
	snapshot.remaining_ms = 0;
	snapshot.playlist_remaining_known = true;
	snapshot.playlist_remaining_ms = 0;
	snapshot.items_remaining = 0;

	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	check_no_unknown_wire_markers(json);
	CHECK(obs_data_get_bool(data, "remaining_known"));
	CHECK(obs_data_get_bool(data, "playlist_remaining_known"));
	CHECK(obs_data_get_bool(data, "items_remaining_known"));
	CHECK(has_key(json, "remaining_ms"));
	CHECK(has_key(json, "playlist_remaining_ms"));
	CHECK(has_key(json, "items_remaining"));
	CHECK(obs_data_get_int(data, "remaining_ms") == 0);
	CHECK(obs_data_get_int(data, "playlist_remaining_ms") == 0);
	CHECK(obs_data_get_int(data, "items_remaining") == 0);
	obs_data_release(data);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_json_known_flags_guard_invalid_values(void)
{
	struct mps_playlist_telemetry snapshot;
	obs_data_t *data;
	const char *json;

	mps_playlist_telemetry_init(&snapshot);
	snapshot.remaining_ms = -500;
	snapshot.playlist_remaining_known = true;
	snapshot.playlist_remaining_ms = -1;
	snapshot.items_remaining = SIZE_MAX;

	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	check_always_present_keys(json);
	check_no_unknown_wire_markers(json);
	CHECK(!obs_data_get_bool(data, "remaining_known"));
	CHECK(!obs_data_get_bool(data, "playlist_remaining_known"));
	CHECK(!obs_data_get_bool(data, "items_remaining_known"));
	CHECK(!has_key(json, "remaining_ms"));
	CHECK(!has_key(json, "playlist_remaining_ms"));
	CHECK(!has_key(json, "items_remaining"));
	obs_data_release(data);

	snapshot.playlist_remaining_known = false;
	snapshot.playlist_remaining_ms = 100;
	snapshot.remaining_ms = 0;
	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	CHECK(obs_data_get_bool(data, "remaining_known"));
	CHECK(!obs_data_get_bool(data, "playlist_remaining_known"));
	CHECK(!has_key(json, "playlist_remaining_ms"));
	obs_data_release(data);

#if SIZE_MAX > INT64_MAX
	snapshot.items_remaining = (size_t)INT64_MAX + 1;
	data = mps_playlist_telemetry_to_data(&snapshot);
	CHECK(data != NULL);
	json = obs_data_get_json(data);
	CHECK(json != NULL);
	CHECK(!obs_data_get_bool(data, "items_remaining_known"));
	CHECK(!has_key(json, "items_remaining"));
	obs_data_release(data);
#endif

	mps_playlist_telemetry_free(&snapshot);
}

static void test_state_names(void)
{
	static const struct {
		enum obs_media_state state;
		const char *name;
	} cases[] = {
		{OBS_MEDIA_STATE_NONE, "unknown"},    {OBS_MEDIA_STATE_PLAYING, "playing"},
		{OBS_MEDIA_STATE_OPENING, "playing"}, {OBS_MEDIA_STATE_BUFFERING, "playing"},
		{OBS_MEDIA_STATE_PAUSED, "paused"},   {OBS_MEDIA_STATE_STOPPED, "stopped"},
		{OBS_MEDIA_STATE_ENDED, "ended"},     {OBS_MEDIA_STATE_ERROR, "unknown"},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct mps_playlist_telemetry snapshot;
		obs_data_t *data;

		mps_playlist_telemetry_init(&snapshot);
		snapshot.state = cases[i].state;
		data = mps_playlist_telemetry_to_data(&snapshot);
		CHECK(data != NULL);
		CHECK(strcmp(obs_data_get_string(data, "state"), cases[i].name) == 0);
		obs_data_release(data);
		mps_playlist_telemetry_free(&snapshot);
	}
}

static void test_calculate_speeds(void)
{
	struct mps_playlist_telemetry snapshot;
	int64_t futures[] = {120000, 60000};

	mps_playlist_telemetry_init(&snapshot);
	snapshot.current_item_id = bstrdup("item");
	snapshot.duration_media_ms = 120000;
	snapshot.elapsed_media_ms = 30000;
	snapshot.speed_percent = 200;
	mps_playlist_telemetry_calculate(&snapshot, futures, 2, true);
	CHECK(snapshot.remaining_ms == 45000);
	CHECK(snapshot.playlist_remaining_ms == 135000);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 2);

	snapshot.speed_percent = 50;
	mps_playlist_telemetry_calculate(&snapshot, futures, 2, true);
	CHECK(snapshot.remaining_ms == 180000);
	CHECK(snapshot.playlist_remaining_ms == 540000);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 2);

	snapshot.state = OBS_MEDIA_STATE_PAUSED;
	mps_playlist_telemetry_calculate(&snapshot, futures, 2, true);
	CHECK(snapshot.remaining_ms == 180000);
	CHECK(snapshot.playlist_remaining_ms == 540000);
	mps_playlist_telemetry_calculate(&snapshot, futures, 2, true);
	CHECK(snapshot.remaining_ms == 180000);
	CHECK(snapshot.playlist_remaining_ms == 540000);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_calculate_per_entry_speeds(void)
{
	struct mps_playlist_telemetry snapshot;
	int64_t futures[] = {120000, 60000};
	int64_t local_then_url_speeds[] = {100, 200};
	int64_t url_then_local_speeds[] = {200, 200};
	int64_t odd_futures[] = {1001, 1001};
	int64_t odd_speeds[] = {200, 50};
	int64_t short_futures[] = {1000, 1000};
	int64_t invalid_speeds[] = {0, 200};
	int64_t negative_speeds[] = {-1, 200};
	int64_t overflow_futures[] = {INT64_MAX / 100, 1};
	int64_t overflow_speeds[] = {1, 1};
	int64_t oversized_future[] = {INT64_MAX / 100 + 1};
	int64_t one_percent_speed[] = {1};

	mps_playlist_telemetry_init(&snapshot);
	snapshot.current_item_id = bstrdup("local-item");
	snapshot.duration_media_ms = 120000;
	snapshot.elapsed_media_ms = 30000;
	snapshot.speed_percent = 200;

	/* The current local entry plays at 200%, while a URL after it stays at
	 * 100% and a later local entry uses 200%. */
	mps_playlist_telemetry_calculate_at_speeds(&snapshot, futures, local_then_url_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 45000);
	CHECK(snapshot.playlist_remaining_ms == 195000);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 2);
	CHECK(snapshot.speed_percent == 200);

	/* A current URL at 100% must not have its speed overwritten by the 200%
	 * entries that follow it. */
	snapshot.duration_media_ms = 90000;
	snapshot.elapsed_media_ms = 30000;
	snapshot.speed_percent = 100;
	mps_playlist_telemetry_calculate_at_speeds(&snapshot, futures, url_then_local_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 60000);
	CHECK(snapshot.playlist_remaining_ms == 150000);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.speed_percent == 100);

	/* Ceiling applies independently to each entry and speed. */
	snapshot.duration_media_ms = 1001;
	snapshot.elapsed_media_ms = 500;
	snapshot.speed_percent = 200;
	mps_playlist_telemetry_calculate_at_speeds(&snapshot, odd_futures, odd_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 251);
	CHECK(snapshot.playlist_remaining_ms == 2754);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.speed_percent == 200);

	/* An invalid future speed makes only the aggregate unknown. */
	snapshot.duration_media_ms = 10000;
	snapshot.elapsed_media_ms = 0;
	snapshot.speed_percent = 100;
	mps_playlist_telemetry_calculate_at_speeds(&snapshot, short_futures, invalid_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 10000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 2);

	mps_playlist_telemetry_calculate_at_speeds(&snapshot, short_futures, negative_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 10000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	/* Per-entry duration scaling and accumulation both guard overflow. */
	snapshot.duration_media_ms = 2000;
	snapshot.elapsed_media_ms = 0;
	snapshot.speed_percent = 100;
	mps_playlist_telemetry_calculate_at_speeds(&snapshot, overflow_futures, overflow_speeds, 2, true);
	CHECK(snapshot.remaining_ms == 2000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	mps_playlist_telemetry_calculate_at_speeds(&snapshot, oversized_future, one_percent_speed, 1, true);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 1);

	mps_playlist_telemetry_free(&snapshot);
}

static void test_calculate_unknown_and_overflow(void)
{
	struct mps_playlist_telemetry snapshot;
	int64_t unknown_future[] = {-1};
	int64_t zero_future[] = {0};
	int64_t huge_future[] = {INT64_MAX / 100};

	mps_playlist_telemetry_init(&snapshot);
	snapshot.current_item_id = bstrdup("item");
	snapshot.duration_media_ms = 120000;
	snapshot.elapsed_media_ms = 30000;
	snapshot.speed_percent = 100;

	mps_playlist_telemetry_calculate(&snapshot, unknown_future, 1, true);
	CHECK(snapshot.remaining_ms == 90000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 1);

	mps_playlist_telemetry_calculate(&snapshot, zero_future, 1, true);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	mps_playlist_telemetry_calculate(&snapshot, NULL, 1, true);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, false);
	CHECK(snapshot.remaining_ms == 90000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == SIZE_MAX);

	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.playlist_remaining_ms == 90000);
	CHECK(snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 0);

	snapshot.duration_media_ms = INT64_MAX;
	snapshot.elapsed_media_ms = 0;
	snapshot.speed_percent = 1;
	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	snapshot.duration_media_ms = 1000;
	snapshot.speed_percent = 1;
	mps_playlist_telemetry_calculate(&snapshot, huge_future, 1, true);
	CHECK(snapshot.remaining_ms == 100000);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 1);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_calculate_invalid_current(void)
{
	struct mps_playlist_telemetry snapshot;
	int64_t future[] = {1000};

	mps_playlist_telemetry_init(&snapshot);
	snapshot.duration_media_ms = 120000;
	snapshot.elapsed_media_ms = 0;
	snapshot.speed_percent = 100;
	mps_playlist_telemetry_calculate(&snapshot, future, 1, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == SIZE_MAX);

	snapshot.current_item_id = bstrdup("item");
	snapshot.duration_media_ms = 0;
	mps_playlist_telemetry_calculate(&snapshot, future, 1, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 1);

	snapshot.duration_media_ms = -1;
	snapshot.speed_percent = 100;
	mps_playlist_telemetry_calculate(&snapshot, future, 1, true);
	CHECK(snapshot.remaining_ms == -1);

	snapshot.duration_media_ms = 10000;
	snapshot.speed_percent = 0;
	mps_playlist_telemetry_calculate(&snapshot, future, 1, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 1);

	snapshot.speed_percent = -1;
	mps_playlist_telemetry_calculate(&snapshot, future, 1, true);
	CHECK(snapshot.remaining_ms == -1);
	mps_playlist_telemetry_free(&snapshot);
}

static void test_calculate_elapsed_handling(void)
{
	struct mps_playlist_telemetry snapshot;

	mps_playlist_telemetry_init(&snapshot);
	snapshot.current_item_id = bstrdup("item");
	snapshot.duration_media_ms = 10000;
	snapshot.speed_percent = 100;

	/* Unknown elapsed values must not be treated as zero. */
	snapshot.elapsed_media_ms = -1;
	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);
	CHECK(snapshot.items_remaining == 0);

	snapshot.elapsed_media_ms = -500;
	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.remaining_ms == -1);
	CHECK(snapshot.playlist_remaining_ms == -1);
	CHECK(!snapshot.playlist_remaining_known);

	snapshot.elapsed_media_ms = 0;
	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.remaining_ms == 10000);

	snapshot.elapsed_media_ms = 15000;
	mps_playlist_telemetry_calculate(&snapshot, NULL, 0, true);
	CHECK(snapshot.remaining_ms == 0);
	CHECK(snapshot.playlist_remaining_ms == 0);
	CHECK(snapshot.playlist_remaining_known);

	mps_playlist_telemetry_free(&snapshot);
}

static void test_sequential_pass_with_folder(void)
{
	struct media_file_data children[3] = {0};
	struct media_file_data items[3] = {0};
	struct darray files = {0};
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	size_t count = 0;

	items[0] = make_file("first.mp4", "id-first", 0);
	set_folder(&items[1], "folder", "id-folder", 1, children, 3);
	set_folder_child(&children[0], "folder/a.mp4", "a.mp4", "id-folder", &items[1], 0);
	set_folder_child(&children[1], "folder/b.mp4", "b.mp4", "id-folder", &items[1], 1);
	set_folder_child(&children[2], "folder/c.mp4", "c.mp4", "id-folder", &items[1], 2);
	items[2] = make_file("last.mp4", "id-last", 2);

	files.array = items;
	files.num = 3;
	files.capacity = 3;
	input.files = &files;
	input.current_media = &items[1];
	input.actual_media = &children[1];
	input.current_media_index = 1;
	input.current_folder_item_index = 1;
	input.loop = true;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 2);
	CHECK(remaining != NULL);
	CHECK(strcmp(remaining[0].stable_id, "id-folder/c.mp4") == 0);
	CHECK(strcmp(remaining[0].path, "folder/c.mp4") == 0);
	CHECK(strcmp(remaining[0].filename, "c.mp4") == 0);
	CHECK(remaining[0].media_index == 1);
	CHECK(remaining[0].folder_item_index == 2);
	CHECK(remaining[0].is_folder_child);
	CHECK(!remaining[0].is_current);
	CHECK(strcmp(remaining[1].stable_id, "id-last") == 0);
	CHECK(remaining[1].media_index == 2);
	CHECK(remaining[1].folder_item_index == 0);
	CHECK(!remaining[1].is_folder_child);
	mps_playlist_entries_free(remaining, count);

	input.actual_media = &children[0];
	input.current_folder_item_index = 0;
	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 3);
	CHECK(strcmp(remaining[0].stable_id, "id-folder/b.mp4") == 0);
	CHECK(strcmp(remaining[2].stable_id, "id-last") == 0);
	mps_playlist_entries_free(remaining, count);

	input.current_media = &items[2];
	input.actual_media = &items[2];
	input.current_media_index = 2;
	input.current_folder_item_index = 0;
	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);
}

static void test_sequential_duplicates_and_validation(void)
{
	struct media_file_data items[2] = {0};
	struct darray files = {0};
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	size_t count = 0;

	items[0] = make_file("same.mp4", "dup-1", 0);
	items[1] = make_file("same.mp4", "dup-2", 1);
	files.array = items;
	files.num = 2;
	files.capacity = 2;
	input.files = &files;
	input.current_media = &items[0];
	input.actual_media = &items[0];
	input.current_media_index = 0;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 1);
	CHECK(strcmp(remaining[0].path, "same.mp4") == 0);
	CHECK(strcmp(remaining[0].stable_id, "dup-2") == 0);
	mps_playlist_entries_free(remaining, count);

	input.current_media_index = 1;
	CHECK(!mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(remaining == NULL);
	CHECK(count == 0);

	files.num = 0;
	files.array = NULL;
	input.current_media = NULL;
	input.actual_media = NULL;
	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);
}

static bool remaining_has_id(const struct mps_playlist_item_snapshot *items, size_t count, const char *stable_id)
{
	for (size_t i = 0; i < count; i++)
		if (items[i].stable_id && strcmp(items[i].stable_id, stable_id) == 0)
			return true;
	return false;
}

static void test_shuffle_pass_is_read_only(void)
{
#define SHUFFLE_COUNT 6
	char paths[SHUFFLE_COUNT][16];
	char ids[SHUFFLE_COUNT][16];
	struct media_file_data items[SHUFFLE_COUNT] = {0};
	struct darray files = {0};
	struct shuffler shuffler;
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	struct media_file_data *array_before[SHUFFLE_COUNT];
	struct media_file_data *current;
	size_t count = 0;
	size_t head;
	size_t next;
	size_t history;
	uint64_t rng;

	for (size_t i = 0; i < SHUFFLE_COUNT; i++) {
		snprintf(paths[i], sizeof(paths[i]), "file-%zu.mp4", i);
		snprintf(ids[i], sizeof(ids[i]), "id-%zu", i);
		items[i] = make_file(paths[i], ids[i], i);
	}
	files.array = items;
	files.num = SHUFFLE_COUNT;
	files.capacity = SHUFFLE_COUNT;

	shuffler_init(&shuffler);
	shuffler_set_seed(&shuffler, 0x123456789abcdef0ULL);
	shuffler_update_files(&shuffler, &files);
	shuffler_select(&shuffler, &items[3]);
	current = &items[3];

	head = shuffler.head;
	next = shuffler.next;
	history = shuffler.history;
	rng = shuffler.rng_state;
	memcpy(array_before, shuffler.shuffled_files.array, sizeof(array_before));

	input.files = &files;
	input.shuffler = &shuffler;
	input.shuffle = true;
	input.actual_media = current;
	input.current_media = current;
	input.current_media_index = current->index;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == SHUFFLE_COUNT - next);
	for (size_t i = 0; i < count; i++) {
		CHECK(remaining[i].stable_id != NULL);
		CHECK(strcmp(remaining[i].stable_id, array_before[next + i]->id) == 0);
	}
	mps_playlist_entries_free(remaining, count);

	CHECK(shuffler.head == head);
	CHECK(shuffler.next == next);
	CHECK(shuffler.history == history);
	CHECK(shuffler.rng_state == rng);
	CHECK(shuffler.shuffled_files.num == SHUFFLE_COUNT);
	for (size_t i = 0; i < SHUFFLE_COUNT; i++)
		CHECK(shuffler.shuffled_files.array[i] == array_before[i]);

	while (shuffler.next < shuffler.shuffled_files.num) {
		size_t expected_next = shuffler.next;
		current = shuffler_next(&shuffler);
		input.actual_media = current;
		CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
		CHECK(count == shuffler.shuffled_files.num - expected_next - 1);
		if (count)
			CHECK(strcmp(remaining[0].stable_id, shuffler.shuffled_files.array[expected_next + 1]->id) ==
			      0);
		mps_playlist_entries_free(remaining, count);
	}

	input.actual_media = current;
	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);
	shuffler_destroy(&shuffler);
#undef SHUFFLE_COUNT
}

static void test_shuffle_loop_and_history_ambiguity(void)
{
#define LOOP_COUNT 5
	char paths[LOOP_COUNT][16];
	char ids[LOOP_COUNT][16];
	struct media_file_data items[LOOP_COUNT] = {0};
	struct darray files = {0};
	struct shuffler shuffler;
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	struct media_file_data *current = NULL;
	size_t count = 0;
	size_t head;
	size_t next;
	size_t history;
	uint64_t rng;

	for (size_t i = 0; i < LOOP_COUNT; i++) {
		snprintf(paths[i], sizeof(paths[i]), "loop-%zu.mp4", i);
		snprintf(ids[i], sizeof(ids[i]), "loop-id-%zu", i);
		items[i] = make_file(paths[i], ids[i], i);
	}
	files.array = items;
	files.num = LOOP_COUNT;
	files.capacity = LOOP_COUNT;

	shuffler_init(&shuffler);
	shuffler_set_seed(&shuffler, 0x0f0e0d0c0b0a0908ULL);
	shuffler_set_loop(&shuffler, true);
	shuffler_update_files(&shuffler, &files);

	while (shuffler.next < shuffler.shuffled_files.num)
		current = shuffler_next(&shuffler);

	input.files = &files;
	input.shuffler = &shuffler;
	input.shuffle = true;
	input.loop = true;
	input.actual_media = current;
	input.current_media = current;
	input.current_media_index = current->index;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);

	/* Auto-prepeek starts the next cycle while the old pass is still current. */
	CHECK(shuffler_peek_next(&shuffler) != NULL);
	head = shuffler.head;
	next = shuffler.next;
	history = shuffler.history;
	rng = shuffler.rng_state;
	input.actual_media = current;
	CHECK(!mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(remaining == NULL);
	CHECK(count == 0);
	CHECK(shuffler.head == head);
	CHECK(shuffler.next == next);
	CHECK(shuffler.history == history);
	CHECK(shuffler.rng_state == rng);

	/* Previous from the first item of a new cycle traverses history. */
	shuffler_clear(&shuffler);
	shuffler_set_seed(&shuffler, 0x0f0e0d0c0b0a0908ULL);
	shuffler_set_loop(&shuffler, true);
	shuffler_update_files(&shuffler, &files);
	while (shuffler.next < shuffler.shuffled_files.num)
		shuffler_next(&shuffler);
	current = shuffler_next(&shuffler);
	CHECK(shuffler.next == 1);
	current = shuffler_prev(&shuffler);
	CHECK(shuffler.next == 0);
	input.actual_media = current;
	CHECK(!mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);

	shuffler_destroy(&shuffler);
#undef LOOP_COUNT
}

static void test_shuffle_end_of_pass_with_partial_head(void)
{
#define END_COUNT 3
	char paths[END_COUNT][16];
	char ids[END_COUNT][16];
	struct media_file_data items[END_COUNT] = {0};
	struct darray files = {0};
	struct shuffler shuffler;
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	struct media_file_data *last;
	size_t count = 0;
	size_t head;
	size_t next;
	size_t history;

	for (size_t i = 0; i < END_COUNT; i++) {
		snprintf(paths[i], sizeof(paths[i]), "end-%zu.mp4", i);
		snprintf(ids[i], sizeof(ids[i]), "end-id-%zu", i);
		items[i] = make_file(paths[i], ids[i], i);
	}
	files.array = items;
	files.num = END_COUNT;
	files.capacity = END_COUNT;

	shuffler_init(&shuffler);
	shuffler_set_seed(&shuffler, 0x1020304050607080ULL);
	shuffler_update_files(&shuffler, &files);
	last = shuffler.shuffled_files.array[shuffler.shuffled_files.num - 1];

	/* A prepeeked next cycle can leave the cursor at the end while head has
	 * not caught up yet. The current pass still ends after the last entry. */
	shuffler.head = 1;
	shuffler.next = shuffler.shuffled_files.num;
	shuffler.history = shuffler.shuffled_files.num;
	head = shuffler.head;
	next = shuffler.next;
	history = shuffler.history;

	input.files = &files;
	input.shuffler = &shuffler;
	input.shuffle = true;
	input.actual_media = last;
	input.current_media = last;
	input.current_media_index = last->index;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);
	CHECK(shuffler.head == head);
	CHECK(shuffler.next == next);
	CHECK(shuffler.history == history);

	input.actual_media = &items[0];
	CHECK(!mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 0);
	CHECK(remaining == NULL);

	shuffler_destroy(&shuffler);
#undef END_COUNT
}

static void test_shuffle_folder_and_duplicates(void)
{
	struct media_file_data children[2] = {0};
	struct media_file_data items[3] = {0};
	struct darray files = {0};
	struct shuffler shuffler;
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_item_snapshot *remaining = NULL;
	size_t count = 0;

	items[0] = make_file("same.mp4", "dup-a", 0);
	set_folder(&items[1], "folder", "id-folder", 1, children, 2);
	set_folder_child(&children[0], "folder/x.mp4", "x.mp4", "id-folder", &items[1], 0);
	set_folder_child(&children[1], "folder/y.mp4", "y.mp4", "id-folder", &items[1], 1);
	items[2] = make_file("same.mp4", "dup-b", 2);
	files.array = items;
	files.num = 3;
	files.capacity = 3;

	shuffler_init(&shuffler);
	shuffler_set_seed(&shuffler, 0xabcdef0123456789ULL);
	shuffler_update_files(&shuffler, &files);
	shuffler_select(&shuffler, &items[2]);

	input.files = &files;
	input.shuffler = &shuffler;
	input.shuffle = true;
	input.actual_media = &items[2];
	input.current_media = &items[2];
	input.current_media_index = 2;

	CHECK(mps_playlist_pass_copy(&input, &remaining, &count));
	CHECK(count == 3);
	CHECK(remaining_has_id(remaining, count, "dup-a"));
	CHECK(remaining_has_id(remaining, count, "id-folder/x.mp4"));
	CHECK(remaining_has_id(remaining, count, "id-folder/y.mp4"));
	CHECK(!remaining_has_id(remaining, count, "dup-b"));

	for (size_t i = 0; i < count; i++) {
		if (remaining[i].stable_id && strcmp(remaining[i].stable_id, "id-folder/x.mp4") == 0) {
			CHECK(remaining[i].is_folder_child);
			CHECK(remaining[i].folder_item_index == 0);
		}
	}
	mps_playlist_entries_free(remaining, count);
	shuffler_destroy(&shuffler);
}

int main(void)
{
	test_init_defaults();
	test_json_empty_snapshot_omits_unknowns();
	test_json_roundtrip_and_escaping();
	test_json_zero_values_are_known();
	test_json_known_flags_guard_invalid_values();
	test_state_names();
	test_calculate_speeds();
	test_calculate_per_entry_speeds();
	test_calculate_unknown_and_overflow();
	test_calculate_invalid_current();
	test_calculate_elapsed_handling();
	test_sequential_pass_with_folder();
	test_sequential_duplicates_and_validation();
	test_shuffle_pass_is_read_only();
	test_shuffle_loop_and_history_ambiguity();
	test_shuffle_end_of_pass_with_partial_head();
	test_shuffle_folder_and_duplicates();

	printf("playlist-telemetry tests passed\n");
	return 0;
}
