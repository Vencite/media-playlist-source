# MPS #4 — live OBS regression and release gate

These are **manual tests still to perform**, not automated-test results. Use a
newly built plugin, back up the scene collection, and record OS, OBS/plugin
versions and each result. Do not publish a release or start VEN OBS Utils #2
until MPS has passed its independent runtime/release gate.

API contract and complete wire examples: [schema v1](../docs/playlist-telemetry.md).
Also run the existing [A/B/audio/lifecycle checks](manual-regression.md) and
[shuffle checks](shuffle-lifecycle.md). Passing unit tests does not prove smooth
rendering, audio routing, or real websocket delivery.

## Preparation

- Prepare clips A/B/C with known different durations, visible names/timecode and
  audio. Include a duplicate A entry (different playlist UUID), a folder with
  two clips, one audio-only file, a corrupt file and a missing file.
- Create two separate MPS instances, not just two scene references to one
  source. Keep Queue and Control docks available.
- Use an authenticated obs-websocket v5 client. Subscribe to Vendors (`512`)
  plus any other needed intents. Log full `CallVendorRequest` responses and
  `VendorEvent` envelopes, source UUIDs, revisions and event arrival times.
- Measure playback with recording/output active as well as preview-only. Do not
  use `current_file_name` input settings as the expected identity or clock.

## 1. Request, event and omission contract

1. Request `media-playlist-source / GetPlaylistState` with `source_uuid`.
   Repeat using an exact `source_name`, and both matching identifiers.
2. Test missing/empty/non-string identifiers, absent source, non-MPS source,
   mismatching identifiers, and a bad UUID alongside a good name. They must
   return structured inner errors, never silently choose another source.
3. For every success response and every `PlaylistStateChanged` event, verify:
   - `schema_version == 1`, correct source UUID, all three known flags present;
   - false flag means the associated value key is absent;
   - true flag means its value exists and is a non-negative integer;
   - `0` survives as known zero, including current/pass completion;
   - no unknown value is null, `{}`, -1, an empty placeholder or guessed time;
   - standalone items have no `folder_item_index` key.
4. Verify normal wrappers: response at
   `d.responseData.responseData`; event at `d.eventData.eventData` with
   `d.eventType == VendorEvent`, vendorName and eventType as documented.
5. Disable Vendors subscription: these events should no longer arrive. Restore
   subscription and request a new baseline; do not assume missed events replay.

## 2. Sequential, folders, duplicates and loop

1. Play A -> B -> C. At 100%, compare playlist remaining to A's measured
   remaining + full B + full C. It must settle as inventory becomes known.
2. Use A -> A -> folder(X,Y) -> C. Both A entries contribute separately and have
   different IDs; folder children each contribute once with parent/filename IDs
   and zero-based indices. Counts exclude current and do not depend on durations.
3. Enable Loop. While C is current, count is zero and total equals only C's
   remaining time. No next loop is added even though Up Next is already prepared.
4. Test empty playlist, empty folder, one item, missing/corrupt item and URL/live
   input. Unknown fields must be omitted; known count may coexist with unknown
   aggregate. Recover by replacing invalid media with known files.

## 3. Shuffle and A/B identity boundaries

1. Capture queue/history before and after repeated requests and dock refreshes.
   Reading telemetry must not change the next file, RNG stream, history or cycle.
2. Let a whole cycle play with Loop off/on. Verify current-cycle membership and
   total. On the final item, next-loop preloading must not add a fresh cycle.
3. Request Next/Previous/manual Play Selected repeatedly around a cut. Every
   sample's identity/path and clock must belong to the same committed decoder,
   never next filename + previous elapsed time. A pending selection can leave
   the future pass unknown, but must not present mixed clocks.
4. Repeat with Previous crossing cycle history, duplicate paths, folder children
   and the fresh-activation shuffle mode. Recheck all #5 regressions.

## 4. Clock discontinuities and speed

1. Pause for at least 5 seconds. After the actual OBS pause boundary, remaining
   values stay constant and no identical paused events repeat. Resume advances.
2. Seek forward/backward while playing and while paused; restart Current/First;
   Next/Previous; select an arbitrary file. Observe immediate state updates once
   OBS applies each command, correct decoder identity and no stuck old time.
3. Repeat at 50%, 150% and 200%. Media duration stays in original media units;
   real time-to-end scales once. A 120-second local file at 200% starts with
   approximately 60000 ms remaining, not 120000 or 30000.
4. Mix local files with URLs where supported. Current/future URL effective speed
   stays 100%; future local files use configured speed. An unknown URL duration
   makes the playlist total unknown rather than inventing a duration.
5. Edit order, add/remove/replace files, refresh a folder, change Shuffle/Loop and
   speed while playing/paused. Check indices, IDs, pass membership and knownness.

## 5. Inventory load, cancellation and ownership

1. Use a long local playlist and two MPS instances. Discovery must not block
   starts/cuts or freeze the UI, create audible probe output or route extra audio.
2. During discovery, edit/reorder paths, remove a source, replace a file at the
   same path, then reload the collection. Old generations must not publish into
   the new playlist/source. Background identity revalidation must invalidate a
   changed file (not necessarily on the very next read).
3. Include duplicates and exceed documented cache bounds. No endless duplicate
   probing, unbounded source/thread growth, crash or false known total.
4. Exit OBS during a probe and during source removal; reopen the collection.
   No shutdown deadlock, use-after-free or worker outliving OBS/module services.
5. Compare recordings with/without discovery: no added black frames, audio gaps,
   double audio, mute/gain bypass or meaningful CPU/A/B performance regression.
   Record playlist size, media formats, CPU and observed discovery latency.

## 6. Events, routing and optional services

1. Change playback independently in two sources. UUID/name/identity/revision
   must never cross-contaminate. Rename preserves UUID selection.
2. Observe continuously playing events: approximately 1 Hz, not dock-refresh or
   video-frame rate. Pause/stop, seek/restart, committed cut, settings and newly
   known inventory must not wait for the continuous-event interval.
3. Leave paused/stopped unchanged: no repeated identical snapshots. Reconnect
   and request a fresh baseline; remove/recreate the source and clear old state.
4. Disable/uninstall obs-websocket and restart OBS with the plugin. Playback,
   both docks, media actions and clean shutdown must still work.

## 7. Dock and packaged platforms

1. Queue shows current elapsed/current remaining plus **Playlist remaining**
   from the same snapshot as the API, with `--:--` for unknown and real `00:00`
   for known zero. No independent Qt queue sum or wall-clock countdown.
2. Test compact/floating widths and light/dark themes; no clipping or new
   horizontal scroll. Control selection still does not start playback by itself.
3. Follow Program through nested scenes/groups and hidden ancestors. Preview
   never selects a target. No or multiple Program candidates must not show a
   misleading remaining time for a stale followed target.
4. Run all relevant existing audio/seamless/visibility/manual regression cases.
5. Obtain passing builds for Ubuntu x86_64, Windows x64 and macOS Universal.
   Linux local build is not evidence for the other platforms. Record actual CI
   run/artifact references before calling a platform verified.

## Sign-off

Record automated checks separately from user confirmation. Keep each manual
case open until an explicit successful result; failures require a fix/retest.
No release publication or downstream bridge implementation is authorized by
this checklist alone.
