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

#include <obs-module.h>
#include <plugin-support.h>
#include "duration-inventory.h"
#include "playlist-vendor.h"

#ifdef MPS_ENABLE_FRONTEND_API
#include <obs-frontend-api.h>

/* Quiesce observers/probes while OBS services and module APIs still exist.
 * Module unload alone is too late: source type IDs are already freed then. */
static void telemetry_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		mps_playlist_vendor_stop(MPS_VENDOR_STOP_FRONTEND_EXIT);
		mps_duration_inventory_stop();
	}
}
#endif

#ifdef MPS_ENABLE_PLAYLIST_DOCK
#include "playlist-dock.h"
#endif

#ifdef TEST_SHUFFLER
#include "shuffler.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info media_playlist_source_info;

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&media_playlist_source_info);
#ifdef MPS_ENABLE_FRONTEND_API
	obs_frontend_add_event_callback(telemetry_frontend_event, NULL);
#endif
#ifdef MPS_ENABLE_PLAYLIST_DOCK
	mps_playlist_dock_register();
#endif
#ifdef TEST_SHUFFLER
	test_shuffler();
#endif
	return true;
}

void obs_module_post_load(void)
{
#ifdef MPS_ENABLE_FRONTEND_API
	mps_playlist_vendor_start();
	mps_duration_inventory_start(mps_playlist_vendor_wake, NULL);
#else
	/* A bare libobs host has no pre-shutdown notification. Do not start
	 * background source creation or use another module's API without one. */
	obs_log(LOG_INFO, "background telemetry disabled: frontend lifecycle API is unavailable");
#endif
}

void obs_module_unload(void)
{
	mps_playlist_vendor_stop(MPS_VENDOR_STOP_UNLOAD);
	mps_duration_inventory_stop();
#ifdef MPS_ENABLE_FRONTEND_API
	obs_frontend_remove_event_callback(telemetry_frontend_event, NULL);
#endif
#ifdef MPS_ENABLE_PLAYLIST_DOCK
	mps_playlist_dock_unregister();
#endif
	obs_log(LOG_INFO, "plugin unloaded");
}
