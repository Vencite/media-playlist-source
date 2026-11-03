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
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs.h>

#include "playlist-context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPS_PLAYLIST_TELEMETRY_SCHEMA_VERSION 1

/* Unknown numeric values in C. Unknown fields are omitted from JSON; only the
 * explicit *_known flags and known values are serialized. */
#define MPS_PLAYLIST_TELEMETRY_UNKNOWN_INT INT64_C(-1)
#define MPS_PLAYLIST_TELEMETRY_UNKNOWN_SIZE SIZE_MAX

/* A pure playback/playlist snapshot. Media-clock fields
 * (elapsed_media_ms/duration_media_ms) are in the media's original units;
 * remaining_ms and playlist_remaining_ms are effective real milliseconds after
 * applying speed_percent. */
struct mps_playlist_telemetry {
	char *source_uuid;
	char *source_name;
	char *current_item_id;
	char *current_path;
	char *current_file;
	uint64_t snapshot_revision;
	enum obs_media_state state;
	int64_t speed_percent;
	int64_t elapsed_media_ms;
	int64_t duration_media_ms;
	int64_t remaining_ms;
	int64_t playlist_remaining_ms;
	bool playlist_remaining_known;
	bool shuffle;
	bool loop;
	bool has_next;
	bool has_previous;
	size_t playlist_index;
	size_t folder_item_index;
	size_t items_remaining;
};

/* Initializes every field to its unknown/empty value. The caller owns the
 * string fields and must call mps_playlist_telemetry_free. */
void mps_playlist_telemetry_init(struct mps_playlist_telemetry *snapshot);
void mps_playlist_telemetry_free(struct mps_playlist_telemetry *snapshot);

/* Serializes schema version 1. Unknown identity/timing/index fields are omitted,
 * never sent as null, empty objects, or sentinel numbers. remaining_known,
 * playlist_remaining_known, and items_remaining_known are always emitted; the
 * corresponding value field is present only when its flag is true. A value of
 * 0 is a real zero and is present. playlist_remaining_known is emitted false if
 * the snapshot flag is false or playlist_remaining_ms is negative. The caller
 * owns the returned obs_data_t. */
obs_data_t *mps_playlist_telemetry_to_data(const struct mps_playlist_telemetry *snapshot);

/* Computes remaining_ms and playlist_remaining_ms from the snapshot's current
 * media clocks and the durations of entries that follow the current one.
 * future_media_durations contains original media units for `count` entries and
 * future_speed_percents their effective speeds; a NULL speeds array uses
 * snapshot->speed_percent for every future entry. The current item always uses
 * its own snapshot->speed_percent. A negative elapsed_media_ms is unknown and
 * makes current and playlist remaining unknown; non-negative elapsed values at
 * or beyond duration clamp to zero remaining media time. Playlist totals stay
 * unknown if any future duration is non-positive, any speed is invalid, or
 * pass_known is false. items_remaining is known whenever pass_known is true and
 * a current item exists; it does not depend on duration availability. */
void mps_playlist_telemetry_calculate_at_speeds(struct mps_playlist_telemetry *snapshot,
						const int64_t *future_media_durations,
						const int64_t *future_speed_percents, size_t count, bool pass_known);

/* Convenience wrapper that uses snapshot->speed_percent for all future entries. */
void mps_playlist_telemetry_calculate(struct mps_playlist_telemetry *snapshot, const int64_t *future_media_durations,
				      size_t count, bool pass_known);

/* Copies the playable entries that follow the current item in the current
 * sequential pass or shuffle cycle. The current entry is never included, and
 * the configured pass never continues into another loop iteration. Shuffle
 * lookups are read-only and conservatively fail when the cursor state is
 * ambiguous. On success *remaining is allocated and must be freed with
 * mps_playlist_entries_free; on failure it is NULL and *count is 0. */
bool mps_playlist_pass_copy(const struct mps_playlist_context_input *input,
			    struct mps_playlist_item_snapshot **remaining, size_t *count);

/* Fills snapshot from an MPS source. The caller must hold a reference to
 * source for the duration of the call; this function is provided by the plugin
 * integration layer, not by the pure telemetry module. */
bool mps_playlist_telemetry_get(obs_source_t *source, struct mps_playlist_telemetry *snapshot);

#ifdef __cplusplus
}
#endif
