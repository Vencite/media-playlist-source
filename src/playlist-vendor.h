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

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* obs-websocket vendor interface of this plugin.
 *
 * Vendor:       "media-playlist-source"
 * Request:      "GetPlaylistState"
 * Event:        "PlaylistStateChanged"
 * Event intent: 512 (obs-websocket vendor events); clients only receive the event
 *               when they subscribe with that intent.
 *
 * GetPlaylistState request data. Both identifiers are optional and use the exact
 * snapshot spelling; camelCase keys are not recognized.
 *
 *   { "source_uuid": "…", "source_name": "…" }
 *
 * Identifier resolution:
 *  - neither key sent: error "missing";
 *  - a key sent with an empty, blank, or non-string value: error "invalid";
 *  - source_uuid only: the uuid must exist, otherwise error "missing". There is no
 *    name fallback for an unknown uuid;
 *  - source_name only: exact name match. No match is "missing", several sources with
 *    that name are "ambiguous";
 *  - both sent: both must resolve to the same source, otherwise "missing" (an
 *    identifier that matches nothing) or "mismatch" (they resolve differently);
 *  - the resolved source must be a media_playlist_source_codeyan source, otherwise
 *    "notMPS";
 *  - when the playlist state getter cannot describe the source: "unavailable", also
 *    used for a callback that arrives after mps_playlist_vendor_stop().
 *
 * Responses are written to the response object that obs-websocket passes to the
 * vendor request callback. obs-websocket nests that object itself, so this plugin
 * never adds a "responseData" key. The wire response is, per obs-websocket 5.5.2:
 *
 *   { "vendorName": "media-playlist-source", "requestType": "GetPlaylistState",
 *     "responseData": { "success": true, …snapshot fields… } }
 *   { "vendorName": …, "requestType": …,
 *     "responseData": { "success": false, "error": { "code": …, "message": … } } }
 *
 * PlaylistStateChanged data is the full mps_playlist_telemetry_to_data() snapshot
 * plus "source_uuid", "source_name" and "trigger"; obs-websocket 5.5.2 wraps it as:
 *
 *   { "vendorName": "media-playlist-source", "eventType": "PlaylistStateChanged",
 *     "eventData": { …snapshot fields…, "source_uuid": …, "source_name": …,
 *                    "trigger": "state" | "elapsed" } }
 *
 * "trigger" is "state" for a change notification and "elapsed" for the periodic
 * update emitted about once per second while a playlist is playing. A paused or
 * stopped playlist keeps quiet while its snapshot is unchanged, and an identical
 * snapshot is never emitted twice for the same source.
 *
 * Snapshot fields follow the telemetry schema: unknown fields are omitted, never
 * sent as null, {} or sentinel numbers, and remaining_known,
 * playlist_remaining_known and items_remaining_known are always present. A known
 * value of 0 is a real zero. A source that is removed while the vendor is running
 * emits no event; consumers see it disappear by watching OBS or the revision.
 */
#define MPS_VENDOR_NAME "media-playlist-source"
#define MPS_VENDOR_REQUEST_GET_PLAYLIST_STATE "GetPlaylistState"
#define MPS_VENDOR_EVENT_PLAYLIST_STATE_CHANGED "PlaylistStateChanged"

/* Registers the obs-websocket vendor and request and starts the worker thread.
 * Must be called from obs_module_post_load(): obs-websocket only accepts vendor
 * registration from there. Does nothing, and logs an informational message, when
 * obs-websocket is not installed. Builds without the OBS frontend must not call it:
 * the frontend exit event is the only point where the vendor can be unregistered
 * while obs-websocket is still loaded. */
void mps_playlist_vendor_start(void);

/* Where mps_playlist_vendor_stop() is called from. obs-websocket has no vendor
 * unregister and this module cannot see obs-websocket unload, so the caller has to
 * say what it knows about the load order. */
enum mps_vendor_stop_mode {
	/* OBS frontend exit (OBS_FRONTEND_EVENT_EXIT): obs-websocket is still loaded,
	 * so the request is unregistered through its API. */
	MPS_VENDOR_STOP_FRONTEND_EXIT,
	/* Module unload fallback: the unload order relative to obs-websocket is
	 * unknown, so obs-websocket is not called at all. The request stays registered
	 * and a callback that still arrives answers "unavailable". */
	MPS_VENDOR_STOP_UNLOAD,
};

/* Stops the worker thread, gives up the request (frontend exit only) and waits for
 * in-flight request callbacks. Idempotent and safe to call from both places:
 * plugin-main calls MPS_VENDOR_STOP_FRONTEND_EXIT from its
 * OBS_FRONTEND_EVENT_EXIT handler before the inventory shutdown and
 * MPS_VENDOR_STOP_UNLOAD from obs_module_unload(); the second call is a no-op. From
 * the moment this returns, a vendor request callback that obs-websocket still
 * dispatches answers "unavailable" without calling OBS or obs-websocket. */
void mps_playlist_vendor_stop(enum mps_vendor_stop_mode mode);

/* Playlist change notification (mps_playlist_change_callback_t compatible through
 * the trampoline in playlist-vendor.c). Safe from any thread: it only marks the
 * worker dirty. Reading OBS state and emitting obs-websocket events only happens on
 * the worker thread. */
void mps_playlist_vendor_wake(void *unused);

/* Test seams. With manual scheduling enabled before mps_playlist_vendor_start() no
 * worker thread is created and the caller drives mps_playlist_vendor_tick(), which
 * performs one worker pass as of now_ms. */
void mps_playlist_vendor_set_manual_scheduling(bool manual);
void mps_playlist_vendor_tick(uint64_t now_ms);
