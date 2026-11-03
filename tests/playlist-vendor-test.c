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

/* Exercises the obs-websocket vendor layer against a fake obs-websocket proc
 * handler: vendor registration, request responses and event payloads all travel
 * through the proc API that obs-websocket serves on the global OBS proc handler.
 * Scheduling is deterministic: manual scheduling disables the worker thread and the
 * tests step mps_playlist_vendor_tick() themselves, so no test sleeps.
 *
 * The wire assertions convert the payloads with obs-websocket's own converter (see
 * vendor-wire-json.cpp), never with obs_data_get_json(), so they check exactly the
 * JSON a client receives. Snapshot payloads come from the real playlist-telemetry
 * serializer; only mps_playlist_telemetry_get() is faked, because that is the
 * integration-layer function the plugin supplies rather than the telemetry module. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/darray.h>

#include "obs-websocket-api.h"
#include "playlist-dock.h"
#include "playlist-telemetry.h"
#include "playlist-vendor.h"
#include "vendor-wire-json.h"

#define CHECK(condition)                                                                                \
	do {                                                                                            \
		if (!(condition)) {                                                                     \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE);                                                             \
		}                                                                                       \
	} while (false)

/* ------------------------------------------------------------------------- */
/* Fake obs-websocket                                                        */
/* ------------------------------------------------------------------------- */

struct fake_websocket {
	bool available;
	proc_handler_t *ph;
	char vendor_name[128];
	size_t vendor_register_calls;
	bool request_registered;
	char request_type[128];
	obs_websocket_request_callback_function request_callback;
	void *request_priv_data;
	size_t request_unregister_calls;
	char unregistered_type[128];
	DARRAY(obs_data_t *) events;
	char last_event_type[128];
};

static struct fake_websocket websocket;

static void fake_obs_websocket_get_ph(void *data, calldata_t *cd)
{
	(void)data;

	if (websocket.available)
		calldata_set_ptr(cd, "ph", websocket.ph);
}

static void fake_vendor_register(void *data, calldata_t *cd)
{
	const char *name = calldata_string(cd, "name");

	(void)data;

	++websocket.vendor_register_calls;
	snprintf(websocket.vendor_name, sizeof(websocket.vendor_name), "%s", name ? name : "");
	calldata_set_ptr(cd, "vendor", &websocket);
}

static void fake_vendor_request_register(void *data, calldata_t *cd)
{
	struct obs_websocket_request_callback *callback = calldata_ptr(cd, "callback");
	const char *type = calldata_string(cd, "type");

	(void)data;

	websocket.request_registered = callback && callback->callback;
	if (websocket.request_registered) {
		snprintf(websocket.request_type, sizeof(websocket.request_type), "%s", type ? type : "");
		websocket.request_callback = callback->callback;
		websocket.request_priv_data = callback->priv_data;
	}
	calldata_set_bool(cd, "success", websocket.request_registered);
}

static void fake_vendor_request_unregister(void *data, calldata_t *cd)
{
	const char *type = calldata_string(cd, "type");

	(void)data;

	++websocket.request_unregister_calls;
	snprintf(websocket.unregistered_type, sizeof(websocket.unregistered_type), "%s", type ? type : "");
	websocket.request_registered = false;
	websocket.request_callback = NULL;
	calldata_set_bool(cd, "success", true);
}

static void fake_vendor_event_emit(void *data, calldata_t *cd)
{
	const char *type = calldata_string(cd, "type");
	obs_data_t *event_data = calldata_ptr(cd, "data");

	(void)data;

	snprintf(websocket.last_event_type, sizeof(websocket.last_event_type), "%s", type ? type : "");
	if (event_data) {
		obs_data_addref(event_data);
		da_push_back(websocket.events, &event_data);
	}
	calldata_set_bool(cd, "success", true);
}

static void fake_websocket_install(void)
{
	proc_handler_t *global = obs_get_proc_handler();

	websocket.available = true;
	websocket.ph = proc_handler_create();
	proc_handler_add(websocket.ph, "void vendor_register(in string name, out ptr vendor)", fake_vendor_register,
			 NULL);
	proc_handler_add(websocket.ph,
			 "void vendor_request_register(in string type, in ptr callback, out bool success)",
			 fake_vendor_request_register, NULL);
	proc_handler_add(websocket.ph, "void vendor_request_unregister(in string type, out bool success)",
			 fake_vendor_request_unregister, NULL);
	proc_handler_add(websocket.ph, "void vendor_event_emit(in string type, in ptr data, out bool success)",
			 fake_vendor_event_emit, NULL);
	proc_handler_add(global, "void obs_websocket_api_get_ph(out ptr ph)", fake_obs_websocket_get_ph, NULL);
}

/* ------------------------------------------------------------------------- */
/* Fake OBS sources                                                          */
/* ------------------------------------------------------------------------- */

static const char *const fake_playlist_source_id = "media_playlist_source_codeyan";
static const char *const fake_other_source_id = "mps_vendor_test_other_source";

static char fake_source_dummy;

static void *fake_source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	(void)source;
	return &fake_source_dummy;
}

static void fake_source_destroy(void *data)
{
	(void)data;
}

static const char *fake_source_name(void *data)
{
	(void)data;
	return "Media Playlist";
}

static uint32_t fake_source_width(void *data)
{
	(void)data;
	return 64;
}

static uint32_t fake_source_height(void *data)
{
	(void)data;
	return 64;
}

static enum obs_media_state fake_source_state(void *data)
{
	/* The vendor layer uses the snapshot state; this keeps the source a plausible
	 * CONTROLLABLE_MEDIA source for OBS. */
	(void)data;
	return OBS_MEDIA_STATE_PLAYING;
}

static const struct obs_source_info fake_playlist_source_info = {
	.id = "media_playlist_source_codeyan",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = fake_source_name,
	.create = fake_source_create,
	.destroy = fake_source_destroy,
	.get_width = fake_source_width,
	.get_height = fake_source_height,
	.media_get_state = fake_source_state,
};

static const struct obs_source_info fake_other_source_info = {
	.id = "mps_vendor_test_other_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fake_source_name,
	.create = fake_source_create,
	.destroy = fake_source_destroy,
	.get_width = fake_source_width,
	.get_height = fake_source_height,
};

/* ------------------------------------------------------------------------- */
/* Fake playlist change listener API                                         */
/* ------------------------------------------------------------------------- */

static mps_playlist_change_callback_t listener_callback;
static void *listener_data;
static size_t listener_add_calls;
static size_t listener_remove_calls;

void mps_playlist_change_add_listener(mps_playlist_change_callback_t callback, void *data)
{
	++listener_add_calls;
	listener_callback = callback;
	listener_data = data;
}

void mps_playlist_change_remove_listener(mps_playlist_change_callback_t callback, void *data)
{
	(void)callback;
	(void)data;
	++listener_remove_calls;
	listener_callback = NULL;
	listener_data = NULL;
}

static void fake_playlist_changed(void)
{
	CHECK(listener_callback != NULL);
	listener_callback(listener_data, "f0f0f0f0-0000-0000-0000-000000000000");
}

/* ------------------------------------------------------------------------- */
/* Scripted playlist telemetry                                               */
/* ------------------------------------------------------------------------- */

/* The real serializer is linked in; only the integration-layer getter is faked. The
 * string fields of `scripted` point at literals and are duplicated into the snapshot
 * that mps_playlist_telemetry_free() releases. */
static struct mps_playlist_telemetry scripted;
static bool telemetry_unavailable;

bool mps_playlist_telemetry_get(obs_source_t *source, struct mps_playlist_telemetry *snapshot)
{
	if (telemetry_unavailable)
		return false;

	snapshot->source_uuid = bstrdup(obs_source_get_uuid(source));
	snapshot->source_name = bstrdup(obs_source_get_name(source));
	snapshot->current_item_id = scripted.current_item_id ? bstrdup(scripted.current_item_id) : NULL;
	snapshot->current_path = scripted.current_path ? bstrdup(scripted.current_path) : NULL;
	snapshot->current_file = scripted.current_file ? bstrdup(scripted.current_file) : NULL;
	snapshot->snapshot_revision = scripted.snapshot_revision;
	snapshot->state = scripted.state;
	snapshot->speed_percent = scripted.speed_percent;
	snapshot->elapsed_media_ms = scripted.elapsed_media_ms;
	snapshot->duration_media_ms = scripted.duration_media_ms;
	snapshot->remaining_ms = scripted.remaining_ms;
	snapshot->playlist_remaining_ms = scripted.playlist_remaining_ms;
	snapshot->playlist_remaining_known = scripted.playlist_remaining_known;
	snapshot->shuffle = scripted.shuffle;
	snapshot->loop = scripted.loop;
	snapshot->has_next = scripted.has_next;
	snapshot->has_previous = scripted.has_previous;
	snapshot->playlist_index = scripted.playlist_index;
	snapshot->folder_item_index = scripted.folder_item_index;
	snapshot->items_remaining = scripted.items_remaining;
	return true;
}

static void script_reset(void)
{
	mps_playlist_telemetry_init(&scripted);
}

/* Every field known: the full schema on the wire. */
static void script_full(void)
{
	script_reset();
	scripted.current_item_id = (char *)"item-1";
	scripted.current_path = (char *)"/media/a.mp4";
	scripted.current_file = (char *)"a.mp4";
	scripted.snapshot_revision = 7;
	scripted.state = OBS_MEDIA_STATE_PLAYING;
	scripted.speed_percent = 100;
	scripted.elapsed_media_ms = 1000;
	scripted.duration_media_ms = 5000;
	scripted.remaining_ms = 4000;
	scripted.playlist_remaining_ms = 9000;
	scripted.playlist_remaining_known = true;
	scripted.playlist_index = 1;
	scripted.items_remaining = 2;
	scripted.loop = true;
	scripted.has_next = true;
}

/* Every optional field unknown: only the *_known flags and stable fields remain. */
static void script_unknowns(void)
{
	script_reset();
	scripted.snapshot_revision = 3;
	scripted.state = OBS_MEDIA_STATE_PAUSED;
}

/* Known zeros must be serialized as zeros, not omitted and not sentinels. */
static void script_known_zeros(void)
{
	script_reset();
	scripted.snapshot_revision = 4;
	scripted.state = OBS_MEDIA_STATE_PLAYING;
	scripted.remaining_ms = 0;
	scripted.playlist_remaining_ms = 0;
	scripted.playlist_remaining_known = true;
	scripted.items_remaining = 0;
}

/* ------------------------------------------------------------------------- */
/* Plugin support stubs                                                      */
/* ------------------------------------------------------------------------- */

const char *PLUGIN_NAME = "media-playlist-source";
const char *PLUGIN_VERSION = "test";

void obs_log(int log_level, const char *format, ...)
{
	(void)log_level;
	(void)format;
}

/* ------------------------------------------------------------------------- */
/* Test helpers                                                              */
/* ------------------------------------------------------------------------- */

static DARRAY(obs_source_t *) created_sources;

static obs_source_t *create_source(const char *id, const char *name)
{
	obs_source_t *source = obs_source_create(id, name, NULL, NULL);

	CHECK(source != NULL);
	da_push_back(created_sources, &source);
	return source;
}

static void destroy_queue_barrier(void *data)
{
	(void)data;
}

static void reset_fixtures(void)
{
	mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);

	for (size_t i = 0; i < created_sources.num; i++)
		obs_source_release(created_sources.array[i]);
	da_free(created_sources);

	/* Source release queues asynchronous destruction (including hotkeys).
	 * Headless OBS has no audio/video workers, so obs_wait_for_destroy_queue()
	 * returns without draining it. Finish these tasks before reusing fixtures
	 * or letting obs_shutdown() free the global hotkey state. */
	obs_queue_task(OBS_TASK_DESTROY, destroy_queue_barrier, NULL, true);

	for (size_t i = 0; i < websocket.events.num; i++)
		obs_data_release(websocket.events.array[i]);
	da_free(websocket.events);

	script_reset();
	telemetry_unavailable = false;

	websocket.available = true;
	websocket.vendor_name[0] = '\0';
	websocket.vendor_register_calls = 0;
	websocket.request_registered = false;
	websocket.request_type[0] = '\0';
	websocket.request_callback = NULL;
	websocket.request_priv_data = NULL;
	websocket.request_unregister_calls = 0;
	websocket.unregistered_type[0] = '\0';
	websocket.last_event_type[0] = '\0';

	listener_callback = NULL;
	listener_data = NULL;
	listener_add_calls = 0;
	listener_remove_calls = 0;
}

static void start_vendor(void)
{
	mps_playlist_vendor_set_manual_scheduling(true);
	mps_playlist_vendor_start();
}

static size_t event_count(void)
{
	return websocket.events.num;
}

static obs_data_t *event_at(size_t index)
{
	CHECK(index < event_count());
	return websocket.events.array[index];
}

static void check_json(const char *actual, const char *expected)
{
	if (strcmp(actual, expected) != 0) {
		fprintf(stderr, "JSON mismatch\n  expected: %s\n  actual:   %s\n", expected, actual);
		exit(EXIT_FAILURE);
	}
}

/* What obs-websocket sends for the CallVendorRequest responseData envelope. */
static char *request_wire(obs_data_t *response)
{
	return mps_vendor_wire_request_response(MPS_VENDOR_NAME, MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE, response);
}

/* What obs-websocket sends for the VendorEvent eventData envelope. */
static char *event_wire(size_t index)
{
	return mps_vendor_wire_vendor_event(MPS_VENDOR_NAME, MPS_VENDOR_EVENT_PLAYLIST_STATE_CHANGED, event_at(index));
}

static obs_data_t *invoke_request(const char *request_json)
{
	obs_data_t *request;
	obs_data_t *response;

	CHECK(websocket.request_callback != NULL);
	request = obs_data_create_from_json(request_json);
	CHECK(request != NULL);
	response = obs_data_create();
	websocket.request_callback(request, response, websocket.request_priv_data);
	obs_data_release(request);

	return response;
}

static void check_error_response(obs_data_t *response, const char *code)
{
	obs_data_t *error = obs_data_get_obj(response, "error");
	bool ok = !obs_data_get_bool(response, "success") && error &&
		  strcmp(obs_data_get_string(error, "code"), code) == 0 &&
		  obs_data_get_string(error, "message")[0] != '\0' &&
		  !obs_data_has_user_value(response, "responseData");

	if (!ok) {
		char *wire = request_wire(response);

		fprintf(stderr, "expected error '%s', got: %s\n", code, wire);
		mps_vendor_wire_json_free(wire);
	}

	CHECK(ok);
	if (error)
		obs_data_release(error);
}

static void check_request_success(obs_data_t *response)
{
	CHECK(obs_data_get_bool(response, "success"));
	CHECK(obs_data_get_obj(response, "error") == NULL);
	/* obs-websocket owns the responseData envelope; the payload stays flat. */
	CHECK(!obs_data_has_user_value(response, "responseData"));
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_start_without_obs_websocket_is_a_no_op(void)
{
	reset_fixtures();
	websocket.available = false;
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();

	start_vendor();
	CHECK(websocket.vendor_register_calls == 0);
	CHECK(!websocket.request_registered);
	CHECK(listener_add_calls == 0);

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 0);

	mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);
	CHECK(websocket.request_unregister_calls == 0);
}

static void test_start_registers_vendor_and_request(void)
{
	reset_fixtures();

	start_vendor();
	CHECK(websocket.vendor_register_calls == 1);
	CHECK(strcmp(websocket.vendor_name, MPS_VENDOR_NAME) == 0);
	CHECK(websocket.request_registered);
	CHECK(strcmp(websocket.request_type, MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE) == 0);
	CHECK(websocket.request_callback != NULL);
	CHECK(listener_add_calls == 1);

	/* A second start must not register twice. */
	mps_playlist_vendor_start();
	CHECK(websocket.vendor_register_calls == 1);
	CHECK(listener_add_calls == 1);

	mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);
	CHECK(websocket.request_unregister_calls == 1);
	CHECK(strcmp(websocket.unregistered_type, MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE) == 0);
	CHECK(listener_remove_calls == 1);
}

static void test_stop_fences_late_request_callback(void)
{
	obs_websocket_request_callback_function late_callback;
	void *late_priv_data;
	obs_data_t *request;
	obs_data_t *response;
	char *wire;

	reset_fixtures();
	start_vendor();
	late_callback = websocket.request_callback;
	late_priv_data = websocket.request_priv_data;
	CHECK(late_callback != NULL);

	mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);

	/* obs-websocket has no vendor unregister. A callback that was already dispatched
	 * when the vendor stopped must answer without touching OBS or obs-websocket. */
	request = obs_data_create_from_json("{}");
	response = obs_data_create();
	late_callback(request, response, late_priv_data);
	obs_data_release(request);

	check_error_response(response, "unavailable");
	wire = request_wire(response);
	check_json(wire, "{\"requestType\":\"GetPlaylistState\",\"responseData\":{\"error\":{\"code\":"
			 "\"unavailable\",\"message\":\"media playlist vendor is not running\"},"
			 "\"success\":false},\"vendorName\":\"media-playlist-source\"}");
	mps_vendor_wire_json_free(wire);
	obs_data_release(response);
}

static void test_request_wire_success_full_snapshot(void)
{
	obs_source_t *source;
	obs_data_t *response;
	char request[256];
	char expected[2048];
	char *wire;

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\"}", obs_source_get_uuid(source));
	response = invoke_request(request);
	check_request_success(response);

	snprintf(expected, sizeof(expected),
		 "{\"requestType\":\"GetPlaylistState\",\"responseData\":{"
		 "\"current_file\":\"a.mp4\",\"current_item_id\":\"item-1\",\"current_path\":\"/media/a.mp4\","
		 "\"duration_media_ms\":5000,\"elapsed_media_ms\":1000,\"has_next\":true,\"has_previous\":false,"
		 "\"items_remaining\":2,\"items_remaining_known\":true,\"loop\":true,\"playlist_index\":1,"
		 "\"playlist_remaining_known\":true,\"playlist_remaining_ms\":9000,\"remaining_known\":true,"
		 "\"remaining_ms\":4000,\"schema_version\":1,\"shuffle\":false,\"snapshot_revision\":7,"
		 "\"source_name\":\"Playlist A\",\"source_uuid\":\"%s\",\"speed_percent\":100,"
		 "\"state\":\"playing\",\"success\":true},\"vendorName\":\"media-playlist-source\"}",
		 obs_source_get_uuid(source));
	wire = request_wire(response);
	check_json(wire, expected);
	mps_vendor_wire_json_free(wire);
	obs_data_release(response);
}

static void test_request_wire_unknown_fields_omitted(void)
{
	obs_source_t *source;
	obs_data_t *response;
	char expected[1024];
	char *wire;

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_unknowns();
	start_vendor();

	response = invoke_request("{\"source_name\":\"Playlist A\"}");
	check_request_success(response);

	snprintf(expected, sizeof(expected),
		 "{\"requestType\":\"GetPlaylistState\",\"responseData\":{"
		 "\"has_next\":false,\"has_previous\":false,\"items_remaining_known\":false,\"loop\":false,"
		 "\"playlist_remaining_known\":false,\"remaining_known\":false,\"schema_version\":1,"
		 "\"shuffle\":false,\"snapshot_revision\":3,\"source_name\":\"Playlist A\","
		 "\"source_uuid\":\"%s\",\"state\":\"paused\",\"success\":true},"
		 "\"vendorName\":\"media-playlist-source\"}",
		 obs_source_get_uuid(source));
	wire = request_wire(response);
	check_json(wire, expected);

	/* The approved schema carries no nulls, no empty objects and no sentinel numbers.
	 * The sentinel and null probes are prefixed with ':' so uuid digits cannot match. */
	CHECK(strstr(wire, ":null") == NULL);
	CHECK(strstr(wire, "{}") == NULL);
	CHECK(strstr(wire, ":-1") == NULL);
	CHECK(strstr(wire, ":18446744073709551615") == NULL);
	CHECK(strstr(wire, "\"remaining_ms\"") == NULL);
	CHECK(strstr(wire, "\"playlist_remaining_ms\"") == NULL);
	CHECK(strstr(wire, "\"items_remaining\"") == NULL);
	CHECK(strstr(wire, "\"elapsed_media_ms\"") == NULL);

	mps_vendor_wire_json_free(wire);
	obs_data_release(response);
}

static void test_request_wire_known_zeros(void)
{
	obs_source_t *source;
	obs_data_t *response;
	char expected[1024];
	char *wire;

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_known_zeros();
	start_vendor();

	snprintf(expected, sizeof(expected), "{\"source_uuid\":\"%s\"}", obs_source_get_uuid(source));
	response = invoke_request(expected);
	check_request_success(response);

	snprintf(expected, sizeof(expected),
		 "{\"requestType\":\"GetPlaylistState\",\"responseData\":{"
		 "\"has_next\":false,\"has_previous\":false,\"items_remaining\":0,\"items_remaining_known\":true,"
		 "\"loop\":false,\"playlist_remaining_known\":true,\"playlist_remaining_ms\":0,"
		 "\"remaining_known\":true,\"remaining_ms\":0,\"schema_version\":1,\"shuffle\":false,"
		 "\"snapshot_revision\":4,\"source_name\":\"Playlist A\",\"source_uuid\":\"%s\","
		 "\"state\":\"playing\",\"success\":true},\"vendorName\":\"media-playlist-source\"}",
		 obs_source_get_uuid(source));
	wire = request_wire(response);
	check_json(wire, expected);
	mps_vendor_wire_json_free(wire);
	obs_data_release(response);
}

static void test_request_wire_error_envelope(void)
{
	obs_data_t *response;
	char *wire;

	reset_fixtures();
	start_vendor();

	response = invoke_request("{}");
	check_error_response(response, "missing");
	wire = request_wire(response);
	check_json(wire, "{\"requestType\":\"GetPlaylistState\",\"responseData\":{\"error\":{\"code\":"
			 "\"missing\",\"message\":\"source_uuid or source_name is required\"},"
			 "\"success\":false},\"vendorName\":\"media-playlist-source\"}");
	mps_vendor_wire_json_free(wire);
	obs_data_release(response);
}

static void test_request_requires_snake_case_keys(void)
{
	obs_source_t *source;
	char request[256];
	obs_data_t *response;

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	/* The camelCase spelling is not part of the contract. */
	snprintf(request, sizeof(request), "{\"sourceUuid\":\"%s\"}", obs_source_get_uuid(source));
	response = invoke_request(request);
	check_error_response(response, "missing");
	obs_data_release(response);

	snprintf(request, sizeof(request), "{\"sourceName\":\"%s\"}", obs_source_get_name(source));
	response = invoke_request(request);
	check_error_response(response, "missing");
	obs_data_release(response);
}

static void test_request_invalid_identifiers(void)
{
	obs_data_t *response;

	reset_fixtures();
	start_vendor();

	response = invoke_request("{\"source_uuid\":\"\"}");
	check_error_response(response, "invalid");
	obs_data_release(response);

	response = invoke_request("{\"source_name\":\"   \"}");
	check_error_response(response, "invalid");
	obs_data_release(response);

	response = invoke_request("{\"source_uuid\":5}");
	check_error_response(response, "invalid");
	obs_data_release(response);
}

static void test_request_uuid_and_name_resolution(void)
{
	obs_source_t *first;
	obs_source_t *second;
	obs_data_t *response;
	char request[512];

	reset_fixtures();
	first = create_source(fake_playlist_source_id, "Playlist A");
	second = create_source(fake_playlist_source_id, "Playlist B");
	script_full();
	start_vendor();

	/* Either identifier resolves on its own. */
	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\"}", obs_source_get_uuid(first));
	response = invoke_request(request);
	check_request_success(response);
	obs_data_release(response);

	response = invoke_request("{\"source_name\":\"Playlist B\"}");
	check_request_success(response);
	obs_data_release(response);

	/* Both identifiers pointing at the same source are accepted. */
	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\",\"source_name\":\"%s\"}",
		 obs_source_get_uuid(first), obs_source_get_name(first));
	response = invoke_request(request);
	check_request_success(response);
	obs_data_release(response);

	/* An unknown uuid is never replaced by a matching name. */
	snprintf(request, sizeof(request),
		 "{\"source_uuid\":\"00000000-0000-0000-0000-000000000000\",\"source_name\":\"%s\"}",
		 obs_source_get_name(first));
	response = invoke_request(request);
	check_error_response(response, "missing");
	obs_data_release(response);

	/* Identifiers that resolve to different sources are a mismatch. */
	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\",\"source_name\":\"%s\"}",
		 obs_source_get_uuid(first), obs_source_get_name(second));
	response = invoke_request(request);
	check_error_response(response, "mismatch");
	obs_data_release(response);

	/* A name alone that matches nothing is missing, and matching is exact. */
	response = invoke_request("{\"source_name\":\"Playlist\"}");
	check_error_response(response, "missing");
	obs_data_release(response);
}

/* OBS keeps source names unique: creating a second source with the same name renames
 * it. The documented "ambiguous" error therefore stays a defensive branch that this
 * OBS version cannot reach through the public source API, and the test pins what the
 * branch protects: a name request matches exactly one source. */
static void test_duplicate_source_names_stay_unambiguous(void)
{
	obs_source_t *first;
	obs_source_t *second;
	obs_data_t *response;

	reset_fixtures();
	first = create_source(fake_playlist_source_id, "Duplicate");
	second = create_source(fake_playlist_source_id, "Duplicate");
	CHECK(strcmp(obs_source_get_name(first), "Duplicate") == 0);
	CHECK(strcmp(obs_source_get_name(second), "Duplicate 2") == 0);
	script_full();
	start_vendor();

	response = invoke_request("{\"source_name\":\"Duplicate\"}");
	check_request_success(response);
	obs_data_release(response);
}

static void test_request_not_playlist_source(void)
{
	obs_source_t *other;
	obs_data_t *response;
	char request[256];

	reset_fixtures();
	other = create_source(fake_other_source_id, "Other");
	script_full();
	start_vendor();

	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\"}", obs_source_get_uuid(other));
	response = invoke_request(request);
	check_error_response(response, "notMPS");
	obs_data_release(response);

	response = invoke_request("{\"source_name\":\"Other\"}");
	check_error_response(response, "notMPS");
	obs_data_release(response);
}

static void test_request_unavailable_snapshot(void)
{
	obs_source_t *source;
	obs_data_t *response;
	char request[256];

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	telemetry_unavailable = true;
	start_vendor();

	snprintf(request, sizeof(request), "{\"source_uuid\":\"%s\"}", obs_source_get_uuid(source));
	response = invoke_request(request);
	check_error_response(response, "unavailable");
	obs_data_release(response);
}

static void test_event_wire_envelope(void)
{
	obs_source_t *source;
	char expected[2048];
	char *wire;

	reset_fixtures();
	source = create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	/* start() marks the worker dirty, so the first pass reports the playlist. */
	CHECK(event_count() == 0);
	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);
	CHECK(strcmp(websocket.last_event_type, MPS_VENDOR_EVENT_PLAYLIST_STATE_CHANGED) == 0);

	snprintf(expected, sizeof(expected),
		 "{\"eventData\":{"
		 "\"current_file\":\"a.mp4\",\"current_item_id\":\"item-1\",\"current_path\":\"/media/a.mp4\","
		 "\"duration_media_ms\":5000,\"elapsed_media_ms\":1000,\"has_next\":true,\"has_previous\":false,"
		 "\"items_remaining\":2,\"items_remaining_known\":true,\"loop\":true,\"playlist_index\":1,"
		 "\"playlist_remaining_known\":true,\"playlist_remaining_ms\":9000,\"remaining_known\":true,"
		 "\"remaining_ms\":4000,\"schema_version\":1,\"shuffle\":false,\"snapshot_revision\":7,"
		 "\"source_name\":\"Playlist A\",\"source_uuid\":\"%s\",\"speed_percent\":100,"
		 "\"state\":\"playing\",\"trigger\":\"state\"},"
		 "\"eventType\":\"PlaylistStateChanged\",\"vendorName\":\"media-playlist-source\"}",
		 obs_source_get_uuid(source));
	wire = event_wire(0);
	check_json(wire, expected);
	mps_vendor_wire_json_free(wire);
}

static void test_paused_source_does_not_repeat_identical_snapshot(void)
{
	char *wire;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_unknowns();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);
	wire = event_wire(0);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	CHECK(strstr(wire, "\"remaining_known\":false") != NULL);
	CHECK(strstr(wire, "\"playlist_remaining_known\":false") != NULL);
	CHECK(strstr(wire, "\"items_remaining_known\":false") != NULL);
	mps_vendor_wire_json_free(wire);

	/* A wake without a change emits nothing. */
	mps_playlist_vendor_wake(NULL);
	mps_playlist_vendor_tick(1500);
	CHECK(event_count() == 1);

	/* Paused playlists never get the periodic update. */
	mps_playlist_vendor_tick(5000);
	CHECK(event_count() == 1);

	/* A real change is reported immediately. */
	scripted.snapshot_revision = 5;
	fake_playlist_changed();
	mps_playlist_vendor_tick(5100);
	CHECK(event_count() == 2);
	wire = event_wire(1);
	CHECK(strstr(wire, "\"snapshot_revision\":5") != NULL);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	mps_vendor_wire_json_free(wire);
}

static void test_elapsed_progress_is_throttled_to_one_hertz(void)
{
	char *wire;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(5000);
	CHECK(event_count() == 1);
	wire = event_wire(0);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	mps_vendor_wire_json_free(wire);

	/* Unchanged snapshot: no repeat even though the pass ran. */
	mps_playlist_vendor_tick(5500);
	CHECK(event_count() == 1);

	/* The getter keeps snapshot_revision stable while a playlist only advances its
	 * clocks, so this is elapsed-only progress: coalesced inside the interval... */
	scripted.elapsed_media_ms = 1500;
	scripted.remaining_ms = 3500;
	mps_playlist_vendor_tick(5800);
	CHECK(event_count() == 1);

	/* ...and reported once the interval has passed. */
	mps_playlist_vendor_tick(6000);
	CHECK(event_count() == 2);
	wire = event_wire(1);
	CHECK(strstr(wire, "\"trigger\":\"elapsed\"") != NULL);
	CHECK(strstr(wire, "\"remaining_ms\":3500") != NULL);
	CHECK(strstr(wire, "\"snapshot_revision\":7") != NULL);
	mps_vendor_wire_json_free(wire);

	scripted.elapsed_media_ms = 2000;
	scripted.remaining_ms = 3000;
	mps_playlist_vendor_tick(6500);
	CHECK(event_count() == 2);
	mps_playlist_vendor_tick(7000);
	CHECK(event_count() == 3);
	wire = event_wire(2);
	CHECK(strstr(wire, "\"trigger\":\"elapsed\"") != NULL);
	mps_vendor_wire_json_free(wire);
}

/* Regression: a change notification for another playlist, or a source appearing or
 * disappearing, wakes a scan for every playlist. Clock-only progress must still wait
 * for its own interval instead of being pushed out by that wake. */
static void test_global_wake_with_elapsed_only_change_emits_nothing(void)
{
	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);

	scripted.elapsed_media_ms = 1200;
	scripted.remaining_ms = 3800;
	mps_playlist_vendor_wake(NULL);
	mps_playlist_vendor_tick(1100);
	CHECK(event_count() == 1);

	mps_playlist_vendor_wake(NULL);
	mps_playlist_vendor_tick(1500);
	CHECK(event_count() == 1);

	mps_playlist_vendor_tick(2000);
	CHECK(event_count() == 2);
}

/* A snapshot_revision change is a discontinuity and is reported at once. */
static void test_revision_change_emits_immediately(void)
{
	char *wire;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);

	scripted.snapshot_revision = 8;
	scripted.elapsed_media_ms = 1200;
	mps_playlist_vendor_wake(NULL);
	mps_playlist_vendor_tick(1100);
	CHECK(event_count() == 2);
	wire = event_wire(1);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	CHECK(strstr(wire, "\"snapshot_revision\":8") != NULL);
	mps_vendor_wire_json_free(wire);
}

/* A paused seek changes revision and time, so it is reported at once even though a
 * paused playlist has no periodic cadence. */
static void test_paused_seek_emits_immediately(void)
{
	char *wire;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_unknowns();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);

	scripted.snapshot_revision = 4;
	scripted.elapsed_media_ms = 2500;
	scripted.remaining_ms = 1000;
	fake_playlist_changed();
	mps_playlist_vendor_tick(1100);
	CHECK(event_count() == 2);
	wire = event_wire(1);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	CHECK(strstr(wire, "\"elapsed_media_ms\":2500") != NULL);
	mps_vendor_wire_json_free(wire);
}

/* Two playing instances keep their own cadence: repeated scans with clock-only
 * progress never flood them, and each still reports once per interval. */
static void test_two_instances_do_not_flood(void)
{
	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	create_source(fake_playlist_source_id, "Playlist B");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 2);

	scripted.elapsed_media_ms = 1100;
	scripted.remaining_ms = 3900;
	for (uint64_t now = 1100; now < 2000; now += 100) {
		mps_playlist_vendor_wake(NULL);
		mps_playlist_vendor_tick(now);
	}
	CHECK(event_count() == 2);

	mps_playlist_vendor_tick(2000);
	CHECK(event_count() == 4);
}

static void test_playlist_change_wakes_worker_immediately(void)
{
	char *wire;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);

	/* The listener callback only marks the worker dirty; the discontinuity is
	 * reported on the next pass even though the interval has not elapsed. */
	scripted.snapshot_revision = 10;
	scripted.state = OBS_MEDIA_STATE_PAUSED;
	fake_playlist_changed();
	mps_playlist_vendor_tick(1010);
	CHECK(event_count() == 2);
	wire = event_wire(1);
	CHECK(strstr(wire, "\"trigger\":\"state\"") != NULL);
	CHECK(strstr(wire, "\"state\":\"paused\"") != NULL);
	mps_vendor_wire_json_free(wire);
}

static void test_new_and_removed_sources_are_picked_up(void)
{
	obs_source_t *second;

	reset_fixtures();
	create_source(fake_playlist_source_id, "Playlist A");
	script_full();
	start_vendor();

	mps_playlist_vendor_tick(1000);
	CHECK(event_count() == 1);

	second = create_source(fake_playlist_source_id, "Playlist B");
	mps_playlist_vendor_tick(1100);
	CHECK(event_count() == 2);

	/* Removal emits no event; the untracked source is pruned without disturbing the
	 * remaining one. */
	obs_source_remove(second);
	obs_data_t *removed_response = invoke_request("{\"source_name\":\"Playlist B\"}");
	check_error_response(removed_response, "missing");
	obs_data_release(removed_response);
	mps_playlist_vendor_tick(2100);
	CHECK(event_count() == 2);

	scripted.snapshot_revision = 11;
	mps_playlist_vendor_tick(2200);
	CHECK(event_count() == 3);
}

static void test_unload_stop_fences_requests_without_obs_websocket_calls(void)
{
	obs_websocket_request_callback_function late_callback;
	void *late_priv_data;
	obs_data_t *request;
	obs_data_t *response;

	reset_fixtures();
	start_vendor();
	late_callback = websocket.request_callback;
	late_priv_data = websocket.request_priv_data;
	CHECK(late_callback != NULL);

	/* Module unload cannot know whether obs-websocket is still loaded, so it must not
	 * call into obs-websocket: the request stays registered and the fence answers. */
	mps_playlist_vendor_stop(MPS_VENDOR_STOP_UNLOAD);
	CHECK(websocket.request_unregister_calls == 0);
	CHECK(websocket.request_registered);

	request = obs_data_create_from_json("{\"source_name\":\"Playlist A\"}");
	response = obs_data_create();
	late_callback(request, response, late_priv_data);
	obs_data_release(request);

	check_error_response(response, "unavailable");
	obs_data_release(response);

	/* The second stop of the same shutdown path is a no-op. */
	mps_playlist_vendor_stop(MPS_VENDOR_STOP_UNLOAD);
	CHECK(websocket.request_unregister_calls == 0);
}

static void test_stop_before_start_is_safe(void)
{
	reset_fixtures();

	mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);
	mps_playlist_vendor_stop(MPS_VENDOR_STOP_UNLOAD);
	CHECK(websocket.request_unregister_calls == 0);
}

int main(void)
{
	CHECK(obs_startup("en-US", NULL, NULL));
	obs_register_source(&fake_playlist_source_info);
	obs_register_source(&fake_other_source_info);
	fake_websocket_install();

	test_start_without_obs_websocket_is_a_no_op();
	test_start_registers_vendor_and_request();
	test_stop_fences_late_request_callback();
	test_unload_stop_fences_requests_without_obs_websocket_calls();
	test_request_wire_success_full_snapshot();
	test_request_wire_unknown_fields_omitted();
	test_request_wire_known_zeros();
	test_request_wire_error_envelope();
	test_request_requires_snake_case_keys();
	test_request_invalid_identifiers();
	test_request_uuid_and_name_resolution();
	test_duplicate_source_names_stay_unambiguous();
	test_request_not_playlist_source();
	test_request_unavailable_snapshot();
	test_event_wire_envelope();
	test_paused_source_does_not_repeat_identical_snapshot();
	test_elapsed_progress_is_throttled_to_one_hertz();
	test_global_wake_with_elapsed_only_change_emits_nothing();
	test_revision_change_emits_immediately();
	test_paused_seek_emits_immediately();
	test_two_instances_do_not_flood();
	test_playlist_change_wakes_worker_immediately();
	test_new_and_removed_sources_are_picked_up();
	test_stop_before_start_is_safe();

	reset_fixtures();
	proc_handler_destroy(websocket.ph);
	obs_shutdown();
	printf("playlist-vendor: all tests passed\n");
	return 0;
}
