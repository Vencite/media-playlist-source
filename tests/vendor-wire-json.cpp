/*
obs-websocket JSON converter and vendor envelopes, used by the Media Playlist Source
wire test.
Copyright (C) 2016-2021 Stephane Lepin <stephane.lepin@gmail.com>
Copyright (C) 2020-2021 Kyle Manning <tt2468@gmail.com>

Copied functions and envelope statements carry the obs-websocket copyrights above;
the test harness glue around them is Media Playlist Source work,
Copyright (C) 2023 Ian Rodriguez ianlemuelr@gmail.com.

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

/* Converts the vendor obs_data_t payloads into the JSON that obs-websocket puts on
 * the wire.
 *
 * The converter below is copied verbatim from obs-websocket 5.5.2
 * (https://github.com/obsproject/obs-websocket, tag 5.5.2, GPL-2.0-or-later),
 * src/utils/Json.cpp, functions set_json_string, set_json_number, set_json_bool,
 * set_json_object, set_json_array and Utils::Json::ObsDataToJson. Omitted from that
 * file: JsonArrayIsValidObsArray, JsonToObsData, the obs_data_set_json_* helpers and
 * the <fstream> file helpers, none of which ObsDataToJson reaches. The declaration
 * matches obs-websocket's src/utils/Json.h, including its includeDefault = false
 * default.
 *
 * The two envelopes are built with the same statements as obs-websocket 5.5.2 uses
 * for vendor payloads, quoted next to each implementation:
 *  - CallVendorRequest response: src/requesthandler/RequestHandler_General.cpp,
 *    RequestHandler::CallVendorRequest;
 *  - VendorEvent: src/obs-websocket.cpp, OnWebSocketApiVendorEvent, which
 *    WebSocketServer::BroadcastEvent then sends as
 *    {"op":5,"d":{"eventType","eventIntent","eventData"}} (the envelope below is the
 *    eventData of that message).
 *
 * Tests must know what a client actually receives, so they call this file instead of
 * obs_data_get_json(). The test build needs nlohmann/json.hpp from the OBS build
 * dependencies; the plugin itself does not link this file.
 */

#include <nlohmann/json.hpp>

#include <obs.h>
#include <util/bmem.h>

extern "C" {
#include "vendor-wire-json.h"
}

using json = nlohmann::json;

namespace Utils {
namespace Json {
json ObsDataToJson(obs_data_t *d, bool includeDefault = false);
}
} // namespace Utils

/* ---- begin vendored obs-websocket 5.5.2 src/utils/Json.cpp ---- */
// clang-format off
void set_json_string(json *j, const char *name, obs_data_item_t *item)
{
	const char *val = obs_data_item_get_string(item);
	j->emplace(name, val);
}
void set_json_number(json *j, const char *name, obs_data_item_t *item)
{
	enum obs_data_number_type type = obs_data_item_numtype(item);

	if (type == OBS_DATA_NUM_INT) {
		long long val = obs_data_item_get_int(item);
		j->emplace(name, val);
	} else {
		double val = obs_data_item_get_double(item);
		j->emplace(name, val);
	}
}
void set_json_bool(json *j, const char *name, obs_data_item_t *item)
{
	bool val = obs_data_item_get_bool(item);
	j->emplace(name, val);
}
void set_json_object(json *j, const char *name, obs_data_item_t *item, bool includeDefault)
{
	obs_data_t *obj = obs_data_item_get_obj(item);
	j->emplace(name, Utils::Json::ObsDataToJson(obj, includeDefault));
	obs_data_release(obj);
}
void set_json_array(json *j, const char *name, obs_data_item_t *item, bool includeDefault)
{
	json jArray = json::array();
	obs_data_array_t *array = obs_data_item_get_array(item);
	size_t count = obs_data_array_count(array);

	for (size_t idx = 0; idx < count; idx++) {
		obs_data_t *subItem = obs_data_array_item(array, idx);
		json jItem = Utils::Json::ObsDataToJson(subItem, includeDefault);
		obs_data_release(subItem);
		jArray.push_back(jItem);
	}

	obs_data_array_release(array);
	j->emplace(name, jArray);
}

json Utils::Json::ObsDataToJson(obs_data_t *d, bool includeDefault)
{
	json j = json::object();
	obs_data_item_t *item = nullptr;

	if (!d)
		return j;

	for (item = obs_data_first(d); item; obs_data_item_next(&item)) {
		enum obs_data_type type = obs_data_item_gettype(item);
		const char *name = obs_data_item_get_name(item);

		if (!obs_data_item_has_user_value(item) && !includeDefault)
			continue;

		switch (type) {
		case OBS_DATA_STRING:
			set_json_string(&j, name, item);
			break;
		case OBS_DATA_NUMBER:
			set_json_number(&j, name, item);
			break;
		case OBS_DATA_BOOLEAN:
			set_json_bool(&j, name, item);
			break;
		case OBS_DATA_OBJECT:
			set_json_object(&j, name, item, includeDefault);
			break;
		case OBS_DATA_ARRAY:
			set_json_array(&j, name, item, includeDefault);
			break;
		default:;
		}
	}

	return j;
}
// clang-format on
/* ---- end vendored obs-websocket 5.5.2 src/utils/Json.cpp ---- */

static char *vendor_wire_duplicate(const json &value)
{
	std::string text = value.dump();
	char *result = (char *)bmalloc(text.size() + 1);

	memcpy(result, text.c_str(), text.size() + 1);
	return result;
}

/* obs-websocket 5.5.2 src/requesthandler/RequestHandler_General.cpp:
 *   json responseData;
 *   responseData["vendorName"] = vendorName;
 *   responseData["requestType"] = requestType;
 *   responseData["responseData"] = Utils::Json::ObsDataToJson(obsResponseData);
 */
char *mps_vendor_wire_request_response(const char *vendor_name, const char *request_type, obs_data_t *response)
{
	json responseData;

	responseData["vendorName"] = vendor_name;
	responseData["requestType"] = request_type;
	responseData["responseData"] = Utils::Json::ObsDataToJson(response);

	return vendor_wire_duplicate(responseData);
}

/* obs-websocket 5.5.2 src/obs-websocket.cpp OnWebSocketApiVendorEvent:
 *   json eventData = Utils::Json::ObsDataToJson(obsEventData);
 *   json broadcastEventData;
 *   broadcastEventData["vendorName"] = vendorName;
 *   broadcastEventData["eventType"] = eventType;
 *   broadcastEventData["eventData"] = eventData;
 */
char *mps_vendor_wire_vendor_event(const char *vendor_name, const char *event_type, obs_data_t *event_data)
{
	json emittedData = Utils::Json::ObsDataToJson(event_data);
	json broadcastEventData;

	broadcastEventData["vendorName"] = vendor_name;
	broadcastEventData["eventType"] = event_type;
	broadcastEventData["eventData"] = emittedData;

	return vendor_wire_duplicate(broadcastEventData);
}

void mps_vendor_wire_json_free(char *json)
{
	bfree(json);
}
