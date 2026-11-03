# Shuffle lifecycle and RNG (#5)

## Change note

The shuffle still uses VLC's incremental Fisher-Yates model: one automatic
selection per item per cycle, Previous follows actual selection history, manual
selection and playlist mutation preserve coherent history, and loop boundaries
avoid an immediate last-to-first repeat when possible.

Previously `rand()` used process-global state. This plugin did not seed it, so
its order could repeat predictably in otherwise similar process sessions, and
other components/playlist instances could consume the same stream. Each shuffler
now owns SplitMix64 state, seeded once from OBS's OS-generated UUID utility.
Tests set explicit seeds; activation does not reseed the generator.

**Shuffle reset behaviour** is independent from **Restart behavior**:

- **Preserve current shuffle cycle** is the default, including old scene
  collections without the new setting. Visibility/restart behavior remains as
  before; activation does not discard shuffle history.
- **Fresh shuffle when source becomes active**, with Shuffle enabled, starts a
  new cycle on each genuine OBS inactive-to-active transition, including the
  first activation after source creation/OBS restart. It selects the first item
  through normal playback switching and takes precedence over visibility's
  restart/resume action for that activation. It also applies with Always Play:
  returning from inactivity deliberately jumps to a fresh item. It does not
  affect an explicit media restart, manual selection, or automatic loop cycles.

If possible, the new first selection differs from the first selection of the
previous fresh activation. The previous identity is held by playlist UUID and,
for folder children, filename, not by a pointer into a replaceable playlist.
Removed identities impose no restriction; a one-item playlist may repeat.
Unreadable/invalid media still use the normal bounded playback retry behavior;
avoidance concerns the selected first candidate, not a guarantee about which
candidate will successfully decode.

A source shared between scenes, including nested scenes, stays active while any
main-view reference remains active. Switching scene names or changing Preview
alone does not reset it. Redundant activation callbacks are ignored. If an
activation overlaps a playlist update, the reset waits for that update to finish
and is cancelled if the source deactivates first.

Fresh activation supersedes the old pending switch/prefetch and neutralizes an
old end event, so the previous queue cannot replace the new first selection.

## Lifecycle audit

| Path | Existing behavior and retained contract |
| --- | --- |
| Deactivate / activate | Stop/restart, pause/resume, always-play, or stop/play-next follows Visibility behavior. Preserve retains the shuffle cycle; Fresh overrides activation only. |
| Media restart | Current File restarts the current decoder without generating a new cycle. First File selects the configured first file/first folder child. That explicit selection now updates shuffle history too. |
| Shuffle off -> on | Starts a new cycle anchored at the current item, rather than picking a new random first item. |
| Playlist add / remove | Rebinds surviving selections/history by stable identity before freeing the old playlist. Missing items are removed; new items join the undetermined part. |
| Folder contents | Flattens folder children into playable items; identifies them by parent UUID and filename. Empty folders do not become shuffle candidates. |
| Manual selection | Moves the selected item into determined history and requests its playback. Does not activate a new cycle. |
| Loop boundary | Retains VLC-style automatic reshuffle/history and immediate-repeat avoidance. |
| Source reload / OBS restart | Shuffle order is memory-only, not persisted. The saved current position anchors initialization. Fresh mode replaces it at the first activation; Preserve retains initialization behavior. |

The audit/test work also repaired folder/non-folder identity lookup null
comparisons and an unsigned reverse-history iteration that could underflow when
manual selection wrapped the history index to zero. Updating an unchanged
playlist also no longer treats its current item as a new manual selection:
that used to move it past a prefetched next item and silently skip that item. The existing embedded test
suite is now a standalone CTest, with assertions enabled and its broken empty
playlist expectation and deep-copy fixture ownership corrected.

## Automated checks

`shuffler-test` runs the existing cycle/history/mutation/folder/loop tests plus
explicit-seed determinism, interleaved independent instances, fresh-cycle
exclusion, and zero-history update regression cases.

`shuffle-activation-test` exercises the actual plugin callbacks using real
libobs without starting graphics/audio or decoder ticks. It checks the default,
fresh/redundant callbacks, complete fresh cycles, empty/single playlists,
shuffle-disabled mode, superseded generations/stale end events, active reference
counts, deferred reset, stable identity across playlist rebuilds, folder
selection, unchanged-playlist prefetch preservation, and First File restart history. It does not prove scene-graph callback
delivery or rendering/audio behavior; use the manual checks below for those.

## Manual OBS regression procedure

Use at least four distinct local clips with visible names and audible audio,
plus a folder with two clips. Test with loop both on and off.

1. Load an existing scene collection without the new setting. Confirm Preserve
   is selected. Exercise all four Visibility behaviors; activation, restart,
   Previous/Next, and manual selection must retain their previous behavior.
2. Enable Fresh with Shuffle. Enter/leave the source's scene several times.
   Each entry starts a fresh cycle, with a different first selected clip from
   the preceding entry when alternatives exist. Play a full cycle without
   manual selection: no item should be lost or duplicated.
3. For each Visibility behavior and each Restart behavior, leave during playback
   and during a pending Next transition, then return. Fresh must win over the
   old restart/resume target. Check Queue's Current/Up Next and Previous/Next.
4. Repeat while the old clip ends naturally, while a local-video standby is
   already prefetched, and with repeated rapid scene changes. No stale clip,
   extra Next, duplicate audio, or stuck/preloaded-only decoder may appear.
5. Reference the same source in two scenes and in a nested scene. Transition
   between references while it remains OBS-active: no fresh reset. Remove/hide
   the final active reference, then restore it: exactly one reset. In Studio
   Mode, changing Preview alone must not reset the Program source.
6. Reorder/add/remove files and folder children between activations, including
   removal of the previous first clip, empty folders, and a playlist reduced
   to one or zero playable entries. No crash or stale selection may occur.
7. Restart Current and Restart First while shuffled. Restart alone must not
   generate a new cycle; Previous must reflect an explicit First File selection.
8. Cross an automatic loop boundary. The last clip must not immediately repeat
   when there is another choice; Previous must retain coherent history.
9. Save/reload the collection and restart OBS on and off the playlist scene.
   Preserve retains saved-position initialization; Fresh randomizes on its
   first actual activation. Test an audio-only item and an invalid media target
   to verify the existing bounded retry path and parent-only audio routing.

Windows/macOS builds and actual OBS rendering/audio checks remain separate
validation requirements; headless CTest does not claim to cover them.
