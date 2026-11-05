# Playlist telemetry — schema v1

This describes the development implementation of MPS #4, not a claim that a
released package or a platform has passed live-OBS validation. Playback remains
owned by OBS and the existing MPS A/B coordinator. There is no bridge, network
countdown controller, or wall-clock playlist timer in this plugin.

## Availability and transport

The official obs-websocket Vendor API is optional. MPS registers in
`obs_module_post_load()`, after modules have loaded:

- vendor: `media-playlist-source`
- request: `GetPlaylistState`
- event: `PlaylistStateChanged`
- event subscription: **Vendors**, bit `512` (combine with other needed intents)

Missing/disabled obs-websocket or failed registration does not prevent playback
or dock use. A request for an unavailable vendor is rejected by obs-websocket
before the MPS callback runs. Do not scrape `current_file_name` as a fallback.

Packaged presets enable the frontend API. Its exit notification quiesces event
and inventory workers **before** OBS tears down source types and other module
APIs. Bare-libobs builds without `ENABLE_FRONTEND_API` do not start these
background services: native current-item snapshots remain available, but there
is no Vendor registration or future-file probing. This is intentional shutdown
safety, not a second transport. Qt is needed only for the docks.

## Unknown values: strict omission, never placeholders

Every successful snapshot includes these independent boolean/value pairs:

| Boolean (always present) | Associated value | Meaning |
| --- | --- | --- |
| `remaining_known` | `remaining_ms` | Current item's effective time to end |
| `playlist_remaining_known` | `playlist_remaining_ms` | Effective time to end of this pass |
| `items_remaining_known` | `items_remaining` | Playable entries after current in this pass |

- **False:** the associated value key is absent.
- **True:** the associated value key exists and is a non-negative integer.
- **Zero is real zero**, including an exhausted current pass. It never means
  missing data.
- Other unknown/not-applicable fields are also omitted. In particular a
  standalone item has no `folder_item_index` key.
- No unknown field is encoded as JSON `null`, `{}`, an empty-string placeholder,
  a negative sentinel, or a guessed time. Internal C `-1`/`SIZE_MAX` sentinels are
  not wire values.
- Consumers must reject inconsistent flags/values rather than start a misleading
  countdown. A known item count does **not** imply known future durations.

This policy is necessary because obs-websocket's OBS-data converter represents
null objects as `{}`, and libobs 31.1.1 cannot safely JSON-serialize those null
objects. Testing `obs_data_get_json()` alone on newer libobs does not test the
Vendor wire contract.

For example an unavailable duration with a determinate empty suffix is:

```json
{
  "remaining_known": false,
  "playlist_remaining_known": false,
  "items_remaining_known": true,
  "items_remaining": 0
}
```

These are example fields within a full snapshot, not a separate response type.

## Snapshot fields

| Field | Semantics |
| --- | --- |
| `schema_version` | Integer `1` |
| `source_uuid`, `source_name` | OBS instance identity and display name |
| `snapshot_revision` | Per-source monotonically increasing observation revision; identical observations do not advance it |
| `state` | `playing`, `paused`, `stopped`, `ended`, or `unknown` |
| `current_item_id` | Stable **entry** identity: top-level entry UUID, or `parent_uuid/filename` for a folder child |
| `current_path`, `current_file` | Committed active decoder's path and basename |
| `playlist_index` | Zero-based configured top-level index, if still applicable |
| `folder_item_index` | Zero-based child index, only for an applicable folder child |
| `elapsed_media_ms`, `duration_media_ms` | Original media-clock milliseconds, not elapsed wall time |
| `speed_percent` | Effective current-item speed, positive when known |
| `remaining_ms` | Real milliseconds to finish current item, conditional on `remaining_known` |
| `playlist_remaining_ms` | Real milliseconds to finish current pass, conditional on `playlist_remaining_known` |
| `items_remaining` | Number of future playable entries excluding current, conditional on `items_remaining_known` |
| `shuffle`, `loop`, `has_next`, `has_previous` | Playback/queue flags, not control commands |

Current identity and clocks come from the **same committed active decoder**.
A selection awaiting an A/B cut is not presented as already playing with the
old decoder's clock. If an entry has been removed/replaced while its decoder is
still draining, timing can still identify that decoder while playlist indices
and the future pass are unknown. Source UUID distinguishes instances, including
two sources using identical paths. Recreating a source starts a new lifetime;
clients must invalidate old state on removal/reconnect.

### Time and speed

For each item, effective remaining time is the ceiling of
`max(0, duration_media_ms - elapsed_media_ms) * 100 / speed_percent`.
Unknown elapsed/duration/speed or an overflow gives unknown, not zero.

MPS applies its configured speed to existing and future **local** files. OBS
uses 100% for URL/non-local inputs, even if the configured speed differs. Future
entries therefore use their own effective speed, not blindly the current
item's speed. A local 120-second clip at 200% has 60 seconds real remaining at
its start; its `duration_media_ms` remains `120000`. Changing speed can reopen
the decoder; do not assume the media position stays continuous.

Pause follows the OBS media clock, not time since the last event. Inventory
completion may turn an unknown total into a known total while paused, but does
not subtract elapsed wall time.

### Current pass, not an infinite loop

A playlist total includes current remaining plus durations of all not-yet-played
playable entries in the current sequential pass/shuffle cycle. Folder children
are flattened; duplicate entries count separately even when paths match. A
shared duration cache does not collapse their contributions.

Loop does not add another pass. Shuffle inspection never calls `peek_next`,
advances the RNG, changes history, or creates another cycle. Unplayed membership
is sufficient for a sum even where its eventual random order is not chosen.
During genuinely ambiguous history/pending-selection states the future count
and aggregate remain unknown. Preloading the next loop must not turn the last
item's exhausted current pass into an entire additional cycle.

## GetPlaylistState

Use an exact UUID (preferred) or exact source name. One identifier is required.
If both are supplied, **both must resolve to the same source**; an invalid UUID
is not silently rescued by a valid name. Ambiguous names, wrong types, empty or
non-string identifiers, and non-MPS sources are errors.

An obs-websocket v5 request:

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "mps-state-1",
    "requestData": {
      "vendorName": "media-playlist-source",
      "requestType": "GetPlaylistState",
      "requestData": {"source_uuid": "your-obs-source-uuid"}
    }
  }
}
```

The successful `op: 7` response has the normal obs-websocket `requestStatus`
and `d.responseData` wrapper:

```json
{
  "vendorName": "media-playlist-source",
  "requestType": "GetPlaylistState",
  "responseData": {
    "success": true,
    "schema_version": 1,
    "source_uuid": "your-obs-source-uuid",
    "source_name": "VT_PLAYLIST",
    "snapshot_revision": 28,
    "state": "playing",
    "current_item_id": "entry-uuid",
    "current_path": "/media/intro.mp4",
    "current_file": "intro.mp4",
    "playlist_index": 0,
    "elapsed_media_ms": 42000,
    "duration_media_ms": 180000,
    "remaining_known": true,
    "remaining_ms": 138000,
    "playlist_remaining_known": true,
    "playlist_remaining_ms": 817000,
    "items_remaining_known": true,
    "items_remaining": 3,
    "speed_percent": 100,
    "shuffle": false,
    "loop": true,
    "has_next": true,
    "has_previous": false
  }
}
```

The callback's structured error is inside this same inner `responseData`, with
`success: false` and `error: {code, message}`. The outer obs-websocket request
can still report success: the Vendor callback API has no outer failure-status
return. Consumers must check the inner result, not just `requestStatus.result`.
Error codes are `missing` (identifier absent/not found), `invalid` (bad value or
type), `ambiguous` (name not unique), `mismatch` (identifiers disagree), `notMPS`
(wrong source kind), and `unavailable` (snapshot unavailable or service stopped).

## PlaylistStateChanged

The normal `op: 5` event uses this envelope:

```json
{
  "eventType": "VendorEvent",
  "eventIntent": 512,
  "eventData": {
    "vendorName": "media-playlist-source",
    "eventType": "PlaylistStateChanged",
    "eventData": {
      "schema_version": 1,
      "source_uuid": "your-obs-source-uuid",
      "source_name": "VT_PLAYLIST",
      "snapshot_revision": 29,
      "trigger": "state",
      "state": "unknown",
      "remaining_known": false,
      "playlist_remaining_known": false,
      "items_remaining_known": false,
      "shuffle": false,
      "loop": false,
      "has_next": false,
      "has_previous": false
    }
  }
}
```

Each event carries a full snapshot, not a JSON patch. Playlist/state/item/speed
changes and newly available inventory wake a dedicated publisher. Video/audio
callbacks only notify it; they do not serialize or emit events. Continuous
playing-clock changes are limited to approximately 1 Hz. Identical paused or
stopped snapshots are not repeated. Consumers should request a fresh snapshot
on connect/reconnect and discard stale state on source removal; event delivery
is not a replay log. The optional `trigger` hint is `state` or `elapsed`.
Revisions order meaningful discontinuities, not every clock sample: continuous
samples can have the same revision. Do not discard every equal-revision event;
within one playing revision, an older media position must not replace a newer
one. Seek/restart changes the revision; reconnect requires a fresh baseline.

Use standard OBS controllable-media requests (`TriggerMediaInputAction`,
`SetMediaInputCursor`) for playback. This vendor defines no second controller.

## Duration discovery and bounds

One low-priority worker serves all inventories. Disposable private ffmpeg
sources are not activated, rendered, added as children, or routed through the
A/B slots. No `ffprobe` executable or runtime JSON library is required.
Production discovery uses regular local files only; URLs, streams, failures and
unreadable media can remain unknown. Cached identity includes absolute file
path, size and modification time, with background revalidation. Filesystem
changes are not instantly observable: settings refresh and revalidation cause
invalidation, not every snapshot read.

Cache limits are 4096 entries per source and 8192 process-wide. Admission
examines the first 4096 supplied playable paths per settings update, sharing
repeated paths; it does not scan an unlimited list to fill the distinct-path
budget. Paths including their terminator are limited to 4096 bytes, including
identified absolute paths. Snapshot enumeration is also bounded to 4096
configured top-level/playable entries; beyond that bound future count/total can
remain unknown while committed current-item timing remains available. Exceeding
a cache bound leaves untracked durations unknown.
POSIX identity checks preserve available nanosecond mtime precision; Windows
uses the precision supplied by its stat API. Playlist counts still count
duplicates separately. Cancellation invalidates obsolete generations; removing
a source never joins the global worker from OBS's destruction thread. Slow or
hung filesystem syscalls can delay discovery/shutdown and are not converted to
invented durations. Real video/A/B performance and shutdown still require the
manual checks below.

## Validation

See [telemetry regression checks](../tests/playlist-telemetry.md) for manual OBS
procedures and explicit release gates. Linux CI runs CTest in addition to the
existing cross-platform builds. Vendor wire tests use obs-websocket's real
OBS-data-to-JSON conversion and require **test-only** `nlohmann_json` headers on
Linux (`nlohmann-json3-dev` on Ubuntu). They add no plugin runtime dependency.
A playback-only build can use `-DBUILD_TESTING=OFF`.

## Playlist inspection and control (develop extension)

The same `media-playlist-source` vendor also exposes `GetPlaylistEntries`,
`SelectPlaylistItem` and `SetPlaybackOptions`. They use the same exact
`source_uuid` / `source_name` resolution and structured inner success/error
responses as `GetPlaylistState`. No VOS process or separate network server is
required. OBS WebSocket authentication protects this interface.

`schema_version` remains 1 for the existing telemetry; its unknown-value contract
and events are unchanged. Controls reuse the plugin's existing playback logic.
For play, pause, stop, next, previous, restart and seeking, use standard OBS
`TriggerMediaInputAction` / `SetMediaInputCursor` requests with the MPS input UUID.

### GetPlaylistEntries

Example `CallVendorRequest.requestData`:

```json
{
  "vendorName": "media-playlist-source",
  "requestType": "GetPlaylistEntries",
  "requestData": {"source_uuid": "mps-source-uuid", "offset": 0, "limit": 100}
}
```

The inner response has `success`, `schema_version`, `source_uuid`, `source_name`,
`playlist_revision`, `total_count`, `offset`, `count`, `has_more`, `entries`,
`shuffle` and `loop`. `next_offset` exists only when `has_more` is true. Limit is
1–200 (default100), offset is non-negative (default0); both must be integers.
Empty results are a real empty JSON array. Only the page's strings are allocated;
this request does not copy the entire playlist before slicing it.

Each entry has `item_id`, `filename`, `path`, `playlist_index`, `is_folder`,
`selectable`, `is_selected`, and `folder_item_index` for folder children.
Folder headers are included but not selectable; their children appear after them.
Duplicate paths remain separate entries. `is_selected` is the logical selection
marker, which can precede a decoder cut; use `GetPlaylistState.current_item_id`
for the authoritative playing decoder.

Every continuation page (`offset>0`) must send the `playlist_revision` returned
by the first page. A different revision returns `error.code="stale"`; start again
from offset0. Revisions may change after settings updates even when the list looks
unchanged. Reads never peek/advance shuffle or prepare another cycle.

### SelectPlaylistItem

```json
{
  "vendorName": "media-playlist-source",
  "requestType": "SelectPlaylistItem",
  "requestData": {
    "source_uuid": "mps-source-uuid",
    "item_id": "stable-entry-id-from-GetPlaylistEntries",
    "playlist_revision": 12
  }
}
```

`item_id` is required, nonblank and at most4096 bytes. An optional positive
integer `playlist_revision` prevents selection from a stale list; supplying it is
recommended. Unknown/ambiguous IDs, folder headers and a changed revision return
`invalid_or_stale`. A removed/resetting source or absent video subsystem is
unavailable. Selection is checked and executed on OBS's graphics/video thread,
with a held source reference, using the same selector as the dock.

`success=true, accepted=true` means the selection request was accepted, not that
the new decoder has already started. Observe `PlaylistStateChanged` for the actual
committed current identity and timing. Do not retry an uncertain selection blindly.

### SetPlaybackOptions

```json
{
  "vendorName": "media-playlist-source",
  "requestType": "SetPlaybackOptions",
  "requestData": {"source_uuid": "mps-source-uuid", "shuffle": true, "loop": false}
}
```

At least one of `shuffle` / `loop` is required and must be an actual JSON boolean.
These are assignments, not toggles. The partial OBS settings update preserves
playlist contents and other settings. `success=true, accepted=true` means OBS has
accepted the update; VIDEO-source settings are applied on its video thread.
The normal telemetry then reports the resulting mode. Source IDs and persisted
setting keys are unchanged.

### Client integration

Any obs-websocket v5 client can send these envelopes, subscribe to Vendors (512),
and consume `PlaylistStateChanged`. Tests exercise both existing state payloads
and the new list responses through the actual obs-websocket JSON converter.
Companion's standard media-source discovery may still filter out custom input
kinds: the presence of this API is not a claim that its standard module already
creates MPS variables or provides a ready preset. A client/module extension can
call this API directly without routing through VOS.
