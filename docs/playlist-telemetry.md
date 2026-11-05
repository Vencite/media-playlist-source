# Playlist WebSocket API — schema v1

Use obs-websocket v5 `CallVendorRequest` with vendor `media-playlist-source`.
Every request identifies an MPS source by exact `source_uuid` (preferred) or
`source_name`. If both are supplied, they must identify the same source.

## Request format

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "mps-state-1",
    "requestData": {
      "vendorName": "media-playlist-source",
      "requestType": "GetPlaylistState",
      "requestData": {"source_name": "My Playlist"}
    }
  }
}
```

Check both `d.requestStatus.result` and
`d.responseData.responseData.success` in the response. Vendor errors return
`success: false` and `error: {code, message}` even when the outer request succeeds.
Common codes: `missing`, `invalid`, `ambiguous`, `mismatch`, `notMPS`, `unavailable`.

## GetPlaylistState

Returns `success`, `schema_version: 1` and the following snapshot fields.
Unknown or inapplicable fields are omitted.

| Field | Meaning |
| --- | --- |
| `source_uuid`, `source_name` | Source identity and display name |
| `snapshot_revision` | Observation revision; continuous clock samples can share a revision |
| `state` | `playing`, `paused`, `stopped`, `ended`, `unknown` |
| `current_item_id` | Entry UUID, or `parent_uuid/filename` for a folder child |
| `current_path`, `current_file` | Active decoder's path and filename |
| `playlist_index`, `folder_item_index` | Zero-based indices; child index only for folder children |
| `elapsed_media_ms`, `duration_media_ms` | Original media-clock milliseconds |
| `speed_percent` | Effective playback speed |
| `remaining_known`, `remaining_ms` | Current item's remaining real time |
| `playlist_remaining_known`, `playlist_remaining_ms` | Current pass's remaining real time |
| `items_remaining_known`, `items_remaining` | Playable entries after the current item |
| `shuffle`, `loop`, `has_next`, `has_previous` | Playback and queue flags |

### Time and unknown values

- All three `*_known` flags are always present. False means the associated value
  is absent; true means a non-negative integer. Zero means actual zero, never unknown.
- Remaining times are milliseconds adjusted for speed. A 120-second local clip
  at 200% starts with `remaining_ms: 60000`; its media duration stays `120000`.
  URL/non-local inputs use 100%.
- Playlist remaining includes the current remainder and unplayed entries in the
  current pass/shuffle cycle. Loop does not add another cycle. Duplicates count
  separately; folder children count individually. Reads do not change shuffle.
- Pause does not subtract wall time. Current identity and clocks describe the
  active decoder, not a selection waiting to start.
- Future durations are discovered asynchronously for local files. URLs, invalid
  files and resource limits can leave totals unknown; known counts do not imply
  known durations. Discovery is bounded to 4096 entries per source and 8192 cached
  entries process-wide, with a 4096-byte path limit including its terminator.

## PlaylistStateChanged

Subscribe to **Vendors** (`512`, combined with other required subscriptions).
OBS sends `op: 5` / `d.eventType: "VendorEvent"` with:

```json
{
  "vendorName": "media-playlist-source",
  "eventType": "PlaylistStateChanged",
  "eventData": {"...": "full snapshot fields"}
}
```

This object is inside `d.eventData`. Each event contains a full snapshot, not a
patch. Playing-clock updates are approximately 1 Hz; unchanged paused/stopped
snapshots are not repeated. Optional `trigger` is `state` or `elapsed`.

Request a snapshot on connect/reconnect, filter by source UUID, and discard state
when the source is removed. Equal revisions can carry newer clock samples;
seek/restart changes the revision. Events are not a replay log.

## GetPlaylistEntries

Request data: source identifier plus optional `offset` (default 0) and `limit`
(default 100, range 1–200), both integers. Continuation pages (`offset > 0`)
require the first page's positive `playlist_revision`. On `stale`, restart at 0.

Returns `success`, `schema_version`, source identity, `playlist_revision`,
`total_count`, `offset`, `count`, `has_more`, `entries`, `shuffle`, `loop`, and
`next_offset` only when more entries exist. Empty `entries` is `[]`.

Each entry contains `item_id`, `filename`, `path`, `playlist_index`, `is_folder`,
`selectable`, `is_selected`, and `folder_item_index` for children. Folder headers
are not selectable. Duplicate paths have separate identities. `is_selected` is
logical selection; use `GetPlaylistState.current_item_id` for actual playback.

## SelectPlaylistItem

Request data: source identifier, `item_id` from `GetPlaylistEntries`, and optional
positive `playlist_revision` (recommended to reject stale selections).

IDs must be nonblank and at most 4096 bytes. Invalid/ambiguous IDs, folder headers
and revision mismatches return `invalid_or_stale`. A removed/resetting source or
unavailable video subsystem returns `unavailable`.

`success: true, accepted: true` confirms acceptance, not that playback has switched.
Observe the state snapshot/event for the actual playing item; do not blindly retry
an uncertain selection.

## SetPlaybackOptions

Request data: source identifier and at least one JSON boolean, `shuffle` or `loop`.
These assign values rather than toggle them; other source settings are preserved.
`success: true, accepted: true` confirms acceptance. Observe telemetry for the
applied state.

For play, pause, stop, next, previous, restart and seek, use standard OBS requests
`TriggerMediaInputAction` and `SetMediaInputCursor`.

See [runtime checks](../tests/playlist-telemetry.md) for manual validation.
