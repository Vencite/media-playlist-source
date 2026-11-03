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

#include <stdint.h>
#include <string.h>

#include <obs-module.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include "obs-websocket-api.h"
#include "playlist-dock.h"
#include "playlist-telemetry.h"
#include "playlist-vendor.h"
#include "plugin-support.h"

/* Must match media_playlist_source_info.id in media-playlist-source.h. */
#define MPS_VENDOR_SOURCE_ID "media_playlist_source_codeyan"

/* Cadence of PlaylistStateChanged while a playlist is playing. */
#define MPS_VENDOR_EMIT_INTERVAL_MS 1000

/* Request fields and the keys of PlaylistStateChanged. The snapshot schema uses the
 * same names, so events created from an unavailable snapshot still name the source. */
#define MPS_VENDOR_FIELD_UUID "source_uuid"
#define MPS_VENDOR_FIELD_NAME "source_name"
#define MPS_VENDOR_FIELD_STATE "state"
#define MPS_VENDOR_FIELD_ELAPSED_MS "elapsed_media_ms"
#define MPS_VENDOR_FIELD_REMAINING_MS "remaining_ms"
#define MPS_VENDOR_FIELD_PLAYLIST_REMAINING_MS "playlist_remaining_ms"
#define MPS_VENDOR_STATE_PLAYING "playing"
#define MPS_VENDOR_EVENT_TRIGGER "trigger"
#define MPS_VENDOR_EVENT_TRIGGER_STATE "state"
#define MPS_VENDOR_EVENT_TRIGGER_ELAPSED "elapsed"

struct mps_vendor_track {
	char *uuid;
	/* Payload without the continuous clocks: any difference is a discontinuity. */
	char *state_key;
	/* Complete payload: also moves when a playlist only advances its clocks. */
	char *full_key;
	uint64_t last_emit_ms;
	bool seen;
};

struct mps_vendor_state {
	DARRAY(struct mps_vendor_track) tracks;
	obs_websocket_vendor handle;
	pthread_t thread;
	size_t active_requests;
	os_event_t *event;
	bool mutex_ready;
	bool accepting;
	bool running;
	bool thread_started;
	bool manual_scheduling;
	bool periodic;
};

struct mps_vendor_scan {
	uint64_t now_ms;
	bool playing;
};

struct mps_vendor_lookup {
	const char *uuid;
	const char *name;
	obs_source_t *uuid_match;
	obs_source_t *name_match;
	size_t name_match_count;
};

/* The fields below are written by start()/stop() and by the worker thread, which
 * stop() joins before it clears anything, so only the wake path and the vendor
 * request callback need the mutex. */
static struct mps_vendor_state vendor;
static pthread_mutex_t vendor_mutex;

/* Portability: the worker waits with os_event_timedwait() milliseconds. That keeps
 * MSVC out of pthread_cond_timedwait(), which has no CLOCK_REALTIME counterpart on
 * Windows. Auto-reset events also coalesce concurrent change notifications. Both
 * primitives are created once and never destroyed, so a callback or notification that
 * still arrives after stop() cannot touch freed memory. */
static bool vendor_primitives_init(void)
{
	if (!vendor.mutex_ready) {
		if (pthread_mutex_init(&vendor_mutex, NULL) != 0)
			return false;
		vendor.mutex_ready = true;
	}

	if (!vendor.event && os_event_init(&vendor.event, OS_EVENT_TYPE_AUTO) != 0)
		return false;

	return vendor.event != NULL;
}

static uint64_t vendor_now_ms(void)
{
	return (uint64_t)(os_gettime_ns() / 1000000ULL);
}

static bool vendor_is_playlist_source(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);

	return id && strcmp(id, MPS_VENDOR_SOURCE_ID) == 0;
}

/* Returns a new reference to the snapshot payload of source, or NULL when the
 * playlist state getter cannot describe it. The caller must hold a source
 * reference for the complete query. */
static obs_data_t *vendor_source_snapshot(obs_source_t *source)
{
	struct mps_playlist_telemetry telemetry;
	obs_data_t *snapshot = NULL;

	mps_playlist_telemetry_init(&telemetry);
	if (mps_playlist_telemetry_get(source, &telemetry))
		snapshot = mps_playlist_telemetry_to_data(&telemetry);
	mps_playlist_telemetry_free(&telemetry);

	return snapshot;
}

/* Composes a comparable key from the live source name and a payload; renames are
 * meaningful state changes too. */
static char *vendor_compose_key(obs_source_t *source, obs_data_t *data)
{
	const char *name = obs_source_get_name(source);
	const char *json = obs_data_get_json(data);
	size_t name_len = name ? strlen(name) : 0;
	size_t json_len = json ? strlen(json) : 0;
	char *key = bzalloc(name_len + json_len + 2);

	snprintf(key, name_len + json_len + 2, "%s\n%s", name ? name : "", json ? json : "");

	return key;
}

/* Meaningful-state key: the payload without the continuous clocks. The main getter
 * keeps snapshot_revision stable while a playlist only advances its clocks, so a
 * difference here is a real discontinuity (seek, restart, list or inventory change,
 * play state, identity, indices, known flags, duration). */
static char *vendor_state_key(obs_source_t *source, obs_data_t *snapshot)
{
	obs_data_t *meaningful = obs_data_create();
	char *key;

	obs_data_apply(meaningful, snapshot);
	obs_data_erase(meaningful, MPS_VENDOR_FIELD_ELAPSED_MS);
	obs_data_erase(meaningful, MPS_VENDOR_FIELD_REMAINING_MS);
	obs_data_erase(meaningful, MPS_VENDOR_FIELD_PLAYLIST_REMAINING_MS);
	key = vendor_compose_key(source, meaningful);
	obs_data_release(meaningful);

	return key;
}

static struct mps_vendor_track *vendor_track_for(const char *uuid)
{
	struct mps_vendor_track track = {0};

	if (!uuid)
		return NULL;

	for (size_t i = 0; i < vendor.tracks.num; i++) {
		if (strcmp(vendor.tracks.array[i].uuid, uuid) == 0)
			return &vendor.tracks.array[i];
	}

	track.uuid = bstrdup(uuid);
	da_push_back(vendor.tracks, &track);

	return &vendor.tracks.array[vendor.tracks.num - 1];
}

static void vendor_tracks_prune(void)
{
	for (size_t i = vendor.tracks.num; i > 0; i--) {
		struct mps_vendor_track *track = &vendor.tracks.array[i - 1];

		if (track->seen) {
			track->seen = false;
			continue;
		}

		bfree(track->uuid);
		bfree(track->state_key);
		bfree(track->full_key);
		da_erase(vendor.tracks, i - 1);
	}
}

static void vendor_tracks_clear(void)
{
	for (size_t i = 0; i < vendor.tracks.num; i++) {
		bfree(vendor.tracks.array[i].uuid);
		bfree(vendor.tracks.array[i].state_key);
		bfree(vendor.tracks.array[i].full_key);
	}
	da_free(vendor.tracks);
}

static void vendor_emit_snapshot(obs_source_t *source, obs_data_t *snapshot, const char *trigger)
{
	obs_data_t *event = obs_data_create();

	obs_data_apply(event, snapshot);
	/* Events are unsolicited, so they identify the playlist even when the snapshot
	 * had no identity to serialize. */
	obs_data_set_string(event, MPS_VENDOR_FIELD_UUID, obs_source_get_uuid(source));
	obs_data_set_string(event, MPS_VENDOR_FIELD_NAME, obs_source_get_name(source));
	obs_data_set_string(event, MPS_VENDOR_EVENT_TRIGGER, trigger);
	obs_websocket_vendor_emit_event(vendor.handle, MPS_VENDOR_EVENT_PLAYLIST_STATE_CHANGED, event);
	obs_data_release(event);
}

static void vendor_emit_source(struct mps_vendor_scan *scan, obs_source_t *source)
{
	obs_data_t *snapshot = vendor_source_snapshot(source);
	struct mps_vendor_track *track;
	char *state_key;
	char *full_key;
	bool state_changed;
	bool full_changed;
	bool periodic_due;
	bool playing;

	if (!snapshot)
		return;

	/* The snapshot state is authoritative for the cadence. The raw source media state
	 * is transient while a playlist switches between media. */
	playing = strcmp(obs_data_get_string(snapshot, MPS_VENDOR_FIELD_STATE), MPS_VENDOR_STATE_PLAYING) == 0;
	if (playing)
		scan->playing = true;

	state_key = vendor_state_key(source, snapshot);
	full_key = vendor_compose_key(source, snapshot);
	track = vendor_track_for(obs_source_get_uuid(source));
	if (!track) {
		bfree(state_key);
		bfree(full_key);
		obs_data_release(snapshot);
		return;
	}

	track->seen = true;
	state_changed = !track->state_key || strcmp(track->state_key, state_key) != 0;
	full_changed = !track->full_key || strcmp(track->full_key, full_key) != 0;
	periodic_due = scan->now_ms - track->last_emit_ms >= MPS_VENDOR_EMIT_INTERVAL_MS;

	/* A meaningful change (revision, state, identity, indices, known flags, duration) is
	 * reported immediately, whatever caused the scan. Clock-only progress keeps the ~1 Hz
	 * cadence, so a change notification for another playlist, or a new source, can never
	 * push a playing playlist's continuous clocks past its own cadence. */
	if (state_changed || (full_changed && playing && periodic_due)) {
		vendor_emit_snapshot(source, snapshot,
				     state_changed ? MPS_VENDOR_EVENT_TRIGGER_STATE : MPS_VENDOR_EVENT_TRIGGER_ELAPSED);
		bfree(track->state_key);
		track->state_key = state_key;
		state_key = NULL;
		bfree(track->full_key);
		track->full_key = full_key;
		full_key = NULL;
		track->last_emit_ms = scan->now_ms;
	}

	bfree(state_key);
	bfree(full_key);
	obs_data_release(snapshot);
}

static bool vendor_scan_source(void *param, obs_source_t *source)
{
	struct mps_vendor_scan *scan = param;
	obs_source_t *ref;

	if (!vendor_is_playlist_source(source))
		return true;

	ref = obs_source_get_ref(source);
	if (!ref)
		return true;

	if (!obs_source_removed(ref))
		vendor_emit_source(scan, ref);

	obs_source_release(ref);
	return true;
}

void mps_playlist_vendor_tick(uint64_t now_ms)
{
	struct mps_vendor_scan scan = {.now_ms = now_ms, .playing = false};

	obs_enum_sources(vendor_scan_source, &scan);
	vendor_tracks_prune();

	pthread_mutex_lock(&vendor_mutex);
	vendor.periodic = scan.playing;
	pthread_mutex_unlock(&vendor_mutex);
}

static void *vendor_thread_main(void *unused)
{
	(void)unused;

	for (;;) {
		bool periodic;

		pthread_mutex_lock(&vendor_mutex);
		if (!vendor.running) {
			pthread_mutex_unlock(&vendor_mutex);
			break;
		}
		periodic = vendor.periodic;
		pthread_mutex_unlock(&vendor_mutex);

		/* Block until a change notification arrives, or until the ~1 Hz slot while a
		 * playlist is playing. A wake only triggers a scan; it never forces clock-only
		 * progress out early. */
		if (periodic)
			os_event_timedwait(vendor.event, MPS_VENDOR_EMIT_INTERVAL_MS);
		else
			os_event_wait(vendor.event);

		pthread_mutex_lock(&vendor_mutex);
		if (!vendor.running) {
			pthread_mutex_unlock(&vendor_mutex);
			break;
		}
		pthread_mutex_unlock(&vendor_mutex);

		mps_playlist_vendor_tick(vendor_now_ms());
	}

	return NULL;
}

void mps_playlist_vendor_wake(void *unused)
{
	(void)unused;

	/* Reachable from audio/video threads, so it only signals: no OBS or obs-websocket
	 * call happens here. Before start() and after stop() the event does not exist yet
	 * or is no longer waited on, and the signal is simply skipped. */
	if (!vendor.event)
		return;

	os_event_signal(vendor.event);
}

static void vendor_playlist_changed(void *data, const char *source_uuid)
{
	(void)data;
	(void)source_uuid;
	mps_playlist_vendor_wake(NULL);
}

static void vendor_source_signal(void *data, calldata_t *params)
{
	(void)data;
	(void)params;
	mps_playlist_vendor_wake(NULL);
}

static void vendor_connect_signals(void)
{
	signal_handler_t *signals = obs_get_signal_handler();

	if (!signals)
		return;

	signal_handler_connect(signals, "source_create", vendor_source_signal, NULL);
	signal_handler_connect(signals, "source_remove", vendor_source_signal, NULL);
	signal_handler_connect(signals, "source_rename", vendor_source_signal, NULL);
}

static void vendor_disconnect_signals(void)
{
	signal_handler_t *signals = obs_get_signal_handler();

	if (!signals)
		return;

	signal_handler_disconnect(signals, "source_create", vendor_source_signal, NULL);
	signal_handler_disconnect(signals, "source_remove", vendor_source_signal, NULL);
	signal_handler_disconnect(signals, "source_rename", vendor_source_signal, NULL);
}

static void vendor_respond_error(obs_data_t *response, const char *code, const char *message)
{
	obs_data_t *error = obs_data_create();

	obs_data_set_string(error, "code", code);
	obs_data_set_string(error, "message", message);
	obs_data_set_bool(response, "success", false);
	obs_data_set_obj(response, "error", error);
	obs_data_release(error);
}

static bool vendor_identifier_blank(const char *value)
{
	return value[strspn(value, " \t")] == '\0';
}

/* Reads one request identifier. Returns false with *code set when the client sent
 * the key with an unusable value, *present reports whether it can be resolved. */
static bool vendor_read_identifier(obs_data_t *request, const char *key, bool *present, const char **value,
				   const char **code, const char **message)
{
	const char *text;

	*present = obs_data_has_user_value(request, key);
	if (!*present)
		return true;

	text = obs_data_get_string(request, key);
	if (!text || !*text || vendor_identifier_blank(text)) {
		*code = "invalid";
		*message = "identifier must be a non-empty string";
		return false;
	}

	*value = text;
	return true;
}

static bool vendor_lookup_scan(void *param, obs_source_t *source)
{
	if (obs_source_removed(source))
		return true;
	struct mps_vendor_lookup *lookup = param;
	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);

	if (lookup->uuid && uuid && strcmp(uuid, lookup->uuid) == 0 && !lookup->uuid_match)
		lookup->uuid_match = obs_source_get_ref(source);

	if (lookup->name && name && strcmp(name, lookup->name) == 0) {
		++lookup->name_match_count;
		if (!lookup->name_match)
			lookup->name_match = obs_source_get_ref(source);
	}

	return true;
}

/* Resolves the request identifiers into a new source reference.
 *
 * source_uuid and source_name are matched exactly. When both are sent, both must
 * resolve and they must resolve to the same source: an unknown uuid is an error even
 * when the name would match, so a stale uuid is never silently replaced by a name. */
static obs_source_t *vendor_resolve_source(obs_data_t *request, const char **code, const char **message)
{
	struct mps_vendor_lookup lookup = {0};
	obs_source_t *resolved = NULL;
	bool uuid_present = false;
	bool name_present = false;
	const char *uuid = NULL;
	const char *name = NULL;

	if (!vendor_read_identifier(request, MPS_VENDOR_FIELD_UUID, &uuid_present, &uuid, code, message))
		return NULL;
	if (!vendor_read_identifier(request, MPS_VENDOR_FIELD_NAME, &name_present, &name, code, message))
		return NULL;

	if (!uuid_present && !name_present) {
		*code = "missing";
		*message = "source_uuid or source_name is required";
		return NULL;
	}

	lookup.uuid = uuid;
	lookup.name = name;
	obs_enum_sources(vendor_lookup_scan, &lookup);

	if (uuid_present && !lookup.uuid_match) {
		*code = "missing";
		*message = "no source has the requested source_uuid";
		goto done;
	}

	if (name_present) {
		if (lookup.name_match_count > 1) {
			*code = "ambiguous";
			*message = "source_name matches more than one source";
			goto done;
		}
		if (!lookup.name_match) {
			*code = "missing";
			*message = "no source has the requested source_name";
			goto done;
		}
	}

	if (lookup.uuid_match && lookup.name_match && lookup.uuid_match != lookup.name_match) {
		*code = "mismatch";
		*message = "source_uuid and source_name resolve to different sources";
		goto done;
	}

	/* Both identifiers resolving to the same source hold two references. */
	if (lookup.uuid_match && lookup.name_match) {
		obs_source_release(lookup.name_match);
		lookup.name_match = NULL;
	}

	resolved = lookup.uuid_match ? lookup.uuid_match : lookup.name_match;
	lookup.uuid_match = NULL;
	lookup.name_match = NULL;

	if (!vendor_is_playlist_source(resolved)) {
		*code = "notMPS";
		*message = "source is not a media_playlist_source_codeyan source";
		goto done;
	}

	*code = NULL;
	*message = NULL;

done:
	if (lookup.uuid_match)
		obs_source_release(lookup.uuid_match);
	if (lookup.name_match)
		obs_source_release(lookup.name_match);
	if (*code) {
		if (resolved)
			obs_source_release(resolved);
		resolved = NULL;
	}

	return resolved;
}

static void vendor_request_get_playlist_state(obs_data_t *request, obs_data_t *response, void *unused)
{
	const char *code = NULL;
	const char *message = NULL;
	obs_source_t *source;
	obs_data_t *snapshot;
	bool accepting;

	(void)unused;

	pthread_mutex_lock(&vendor_mutex);
	accepting = vendor.accepting;
	if (accepting)
		++vendor.active_requests;
	pthread_mutex_unlock(&vendor_mutex);

	if (!accepting) {
		/* obs-websocket has no vendor unregister and keeps callbacks that were
		 * dispatched around module shutdown. Answer without touching OBS or
		 * obs-websocket so a stopped vendor can never reach a dead proc handler. */
		vendor_respond_error(response, "unavailable", "media playlist vendor is not running");
		return;
	}

	source = vendor_resolve_source(request, &code, &message);
	if (!source) {
		vendor_respond_error(response, code, message);
		goto done;
	}

	snapshot = vendor_source_snapshot(source);
	if (!snapshot) {
		vendor_respond_error(response, "unavailable", "playlist state is not available for this source");
		goto release_source;
	}

	obs_data_set_bool(response, "success", true);
	obs_data_apply(response, snapshot);
	obs_data_release(snapshot);

release_source:
	obs_source_release(source);

done:
	pthread_mutex_lock(&vendor_mutex);
	--vendor.active_requests;
	pthread_mutex_unlock(&vendor_mutex);
	/* Wakes stop()'s drain loop; safe after stop because the event outlives the vendor. */
	os_event_signal(vendor.event);
}

void mps_playlist_vendor_set_manual_scheduling(bool manual)
{
	pthread_mutex_lock(&vendor_mutex);
	vendor.manual_scheduling = manual;
	pthread_mutex_unlock(&vendor_mutex);
}

void mps_playlist_vendor_start(void)
{
	obs_websocket_vendor handle;
	bool manual;

	if (!vendor_primitives_init()) {
		obs_log(LOG_WARNING, "failed to create %s synchronization primitives", MPS_VENDOR_NAME);
		return;
	}

	pthread_mutex_lock(&vendor_mutex);
	if (vendor.running) {
		pthread_mutex_unlock(&vendor_mutex);
		return;
	}
	manual = vendor.manual_scheduling;
	pthread_mutex_unlock(&vendor_mutex);

	handle = obs_websocket_register_vendor(MPS_VENDOR_NAME);
	if (!handle) {
		obs_log(LOG_INFO, "obs-websocket not available, %s vendor disabled", MPS_VENDOR_NAME);
		return;
	}
	vendor.handle = handle;

	/* Accept requests before registering: obs-websocket can dispatch as soon as the
	 * request exists. */
	pthread_mutex_lock(&vendor_mutex);
	vendor.accepting = true;
	pthread_mutex_unlock(&vendor_mutex);

	if (!obs_websocket_vendor_register_request(handle, MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE,
						   vendor_request_get_playlist_state, NULL))
		obs_log(LOG_WARNING, "failed to register %s request", MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE);

	mps_playlist_change_add_listener(vendor_playlist_changed, NULL);
	vendor_connect_signals();

	pthread_mutex_lock(&vendor_mutex);
	vendor.running = true;
	vendor.periodic = false;
	if (!manual) {
		if (pthread_create(&vendor.thread, NULL, vendor_thread_main, NULL) == 0)
			vendor.thread_started = true;
		else
			obs_log(LOG_WARNING, "failed to start %s worker thread", MPS_VENDOR_NAME);
	}
	pthread_mutex_unlock(&vendor_mutex);

	/* The first pass reports every playlist; a wake only triggers a scan. */
	os_event_signal(vendor.event);
}

void mps_playlist_vendor_stop(enum mps_vendor_stop_mode mode)
{
	bool was_running;

	/* Never started, so the mutex was never created. */
	if (!vendor.event)
		return;

	pthread_mutex_lock(&vendor_mutex);
	was_running = vendor.running;
	pthread_mutex_unlock(&vendor_mutex);

	/* plugin-main stops the vendor at OBS_FRONTEND_EVENT_EXIT and calls this again from
	 * obs_module_unload(). The second call must not touch obs-websocket at all. */
	if (!was_running)
		return;

	/* Stop accepting requests first: from here on a callback that obs-websocket still
	 * dispatches answers `unavailable` without touching OBS or obs-websocket. */
	pthread_mutex_lock(&vendor_mutex);
	vendor.accepting = false;
	pthread_mutex_unlock(&vendor_mutex);

	mps_playlist_change_remove_listener(vendor_playlist_changed, NULL);
	vendor_disconnect_signals();

	pthread_mutex_lock(&vendor_mutex);
	vendor.running = false;
	pthread_mutex_unlock(&vendor_mutex);
	os_event_signal(vendor.event);

	if (vendor.thread_started) {
		pthread_join(vendor.thread, NULL);
		vendor.thread_started = false;
	}

	/* obs-websocket offers no vendor unregister. At frontend exit it is still loaded,
	 * so the request can be dropped through its API; clearing the header's cached proc
	 * handler first makes that call re-resolve obs-websocket through the global proc
	 * handler instead of dereferencing a cached pointer. At module unload the load order
	 * relative to obs-websocket is unknown, so the request is left in place and the
	 * accepting fence above answers "unavailable" instead. */
	if (mode == MPS_VENDOR_STOP_FRONTEND_EXIT && vendor.handle) {
		_ph = NULL;
		if (obs_websocket_ensure_ph() &&
		    !obs_websocket_vendor_unregister_request(vendor.handle, MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE))
			obs_log(LOG_WARNING, "failed to unregister %s request", MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE);
	}
	vendor.handle = NULL;

	/* Drain in-flight callbacks. The worker thread has been joined, so only this loop
	 * waits on the event. */
	for (;;) {
		pthread_mutex_lock(&vendor_mutex);
		if (!vendor.active_requests) {
			vendor.periodic = false;
			pthread_mutex_unlock(&vendor_mutex);
			break;
		}
		pthread_mutex_unlock(&vendor_mutex);
		os_event_wait(vendor.event);
	}

	vendor_tracks_clear();
}
