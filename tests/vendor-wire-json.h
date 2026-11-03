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
#include <stddef.h>

#include <obs.h>

/* Wire-shape helpers for the obs-websocket vendor payloads.
 *
 * playlist-vendor.c produces obs_data_t objects, and obs-websocket converts them
 * with its own converter before nesting them in its envelopes. These helpers run
 * the data through that same converter (obs-websocket 5.5.2
 * Utils::Json::ObsDataToJson, vendored in vendor-wire-json.cpp) and build the
 * envelopes exactly as obs-websocket 5.5.2 does, so tests assert the JSON a client
 * receives instead of a test-local writer or obs_data_get_json().
 *
 * Each function returns a newly allocated compact JSON string; release it with
 * mps_vendor_wire_json_free(). The obs_data arguments are only read. */

/* CallVendorRequest responseData envelope:
 * {"requestType":…,"responseData":ObsDataToJson(response),"vendorName":…} */
char *mps_vendor_wire_request_response(const char *vendor_name, const char *request_type, obs_data_t *response);

/* VendorEvent eventData envelope:
 * {"eventData":ObsDataToJson(event_data),"eventType":…,"vendorName":…} */
char *mps_vendor_wire_vendor_event(const char *vendor_name, const char *event_type, obs_data_t *event_data);

void mps_vendor_wire_json_free(char *json);
