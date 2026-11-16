# Playlist WebSocket API

Read playback information, browse playlist entries and control MPS through
[obs-websocket v5](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md). The vendor name is `media-playlist-source`; data uses schema v1.

## 1. Connect and choose a source

Enable **Tools → WebSocket Server Settings** in OBS, then connect and authenticate
your client using the address, port (default `4455`) and password shown there.

Every MPS request needs either `source_uuid` (recommended) or the exact
`source_name`. If you provide both, they must identify the same MPS source.

In Companion's OBS Studio connection, choose **Custom – Send Vendor Request**:

| Field | Example |
| --- | --- |
| Vendor name | `media-playlist-source` |
| Request type | `GetPlaylistState` |
| Request data | `{"source_name":"My Playlist"}` |

For other clients, the full WebSocket request is:

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

The examples below show the **inner request data**. Keep the same outer envelope
and change its vendor `requestType` to the operation you need.

## 2. Read playback — GetPlaylistState

Returns `success`, `schema_version: 1` and a snapshot. Unknown or inapplicable
fields are omitted, except the three `*_known` flags, which are always present.

| Information | Fields |
| --- | --- |
| Source | `source_uuid`, `source_name` |
| State | `state`: `playing`, `paused`, `stopped`, `ended`, `unknown` |
| Playing entry | `current_item_id`, `current_file`, `current_path` |
| Position in playlist | `playlist_index`, `folder_item_index` (folder children only); both zero-based |
| Media clock | `elapsed_media_ms`, `duration_media_ms`, `speed_percent` |
| Current file remaining | `remaining_known` → `remaining_ms` |
| Current pass remaining | `playlist_remaining_known` → `playlist_remaining_ms` |
| Entries after current | `items_remaining_known` → `items_remaining` |
| Modes/navigation | `shuffle`, `loop`, `has_next`, `has_previous` |
| Observation revision | `snapshot_revision` |

`current_item_id` is a top-level entry UUID or `parent_uuid/filename` for a folder
child. Identity and timing describe the active decoder, not a pending selection.

### Understand the times

- All times are milliseconds. Media-clock fields describe the original file;
  remaining times account for speed. A 120-second local file at 200% starts with
  `remaining_ms: 60000`, while `duration_media_ms` stays `120000`.
- Playlist remaining means the current file's remainder plus unplayed entries
  in the current pass/shuffle cycle. Loop does not add another cycle. The API
  still provides this value with Loop on, although the Queue dock hides it.
- Folder children and duplicate entries count separately. Reading state does not
  change shuffle. Pause freezes playback time; newly discovered durations may
  still make a previously unknown total available.
- A false `*_known` flag means its value field is absent. True means a
  non-negative integer. **Zero is real zero**, never a substitute for unknown.
- Local-file durations are discovered in the background. URLs, unreadable files
  and resource limits may leave totals unknown. Non-local inputs use 100% speed.

## 3. Browse the playlist — GetPlaylistEntries

```json
{"source_name":"My Playlist","offset":0,"limit":100}
```

`offset` is a non-negative integer; `limit` is 1–200 (default 100).

The response contains source identity, `success`, `schema_version`,
`playlist_revision`, `total_count`, `offset`, `count`, `entries`, `has_more`,
`shuffle` and `loop`. `next_offset` appears only when more entries exist;
an empty list is `[]`.

Each entry contains `item_id`, `filename`, `path`, `playlist_index`, `is_folder`,
`selectable`, `is_selected`, and `folder_item_index` for children. Folder headers
are not selectable; duplicate paths remain distinct entries.

For every subsequent page (`offset > 0`), send the first page's positive
`playlist_revision`. If the response reports `stale`, restart at offset zero.

`is_selected` marks logical selection. Use `GetPlaylistState.current_item_id`
to determine which entry is actually playing.

## 4. Control playback

### SelectPlaylistItem

Use an `item_id` from the playlist response. Include its revision to avoid
selecting from an outdated list:

```json
{"source_name":"My Playlist","item_id":"entry-uuid","playlist_revision":12}
```

`playlist_revision` is optional but must be a positive integer when supplied.
`item_id` must be nonblank and at most 4096 bytes. Folder headers, missing or
ambiguous IDs and revision mismatches return `invalid_or_stale`.

### SetPlaybackOptions

```json
{"source_name":"My Playlist","shuffle":true,"loop":false}
```

Supply at least one actual JSON boolean. Values are assignments, not toggles;
other settings are preserved.

For both controls, `success: true, accepted: true` confirms acceptance, not that
playback has already changed. Observe state updates for the applied result.
Do not blindly retry an uncertain selection.

For play, pause, stop, next, previous and restart, use OBS's standard
`TriggerMediaInputAction`; use `SetMediaInputCursor` for seeking.

## 5. Receive live updates — PlaylistStateChanged

Subscribe to **Vendors** (`512`, combined with other required event subscriptions).
Request an initial snapshot on connect/reconnect, then handle `op: 5` events:

| WebSocket field | Value |
| --- | --- |
| `d.eventType` | `VendorEvent` |
| `d.eventData.vendorName` | `media-playlist-source` |
| `d.eventData.eventType` | `PlaylistStateChanged` |
| `d.eventData.eventData` | Full snapshot, with optional `trigger`: `state` or `elapsed` |

Filter by source UUID. Playing-clock updates are approximately 1 Hz; unchanged
paused/stopped snapshots are not repeated. Discard state when a source is removed.

Continuous clock samples can share `snapshot_revision`; do not discard them just
because the revision is equal. Within a playing revision, an older clock sample
should not replace a newer one. Seek/restart changes the revision. Reconnect needs
a fresh baseline: events are not a replay log.

## Errors and limits

For request responses (`op: 7`), check both `d.requestStatus.result` and the inner
`d.responseData.responseData.success`. A successful outer request can contain an
MPS error: `success: false` with `error: {code, message}`.

Common codes: `missing`, `invalid`, `ambiguous`, `mismatch`, `notMPS`, `unavailable`.
Playlist paging also uses `stale`; selection uses `invalid_or_stale`.

Duration discovery is bounded to 4096 entries per source and 8192 cached entries
process-wide, with a 4096-byte path limit including its terminator. Exceeding
limits leaves affected durations unknown.

See [developer integration](developer-integration.md) for settings/native access
and [runtime checks](../tests/playlist-telemetry.md) for manual validation.
