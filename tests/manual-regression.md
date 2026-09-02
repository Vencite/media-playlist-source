# Manual regression checks

## Audio routing during recording and streaming (#56)

Use media with an audible audio track and an OBS 32 build containing this plugin:

1. Add a Media Playlist Source and set its mixer volume to approximately `-30 dB`.
2. Start playback and recording. Confirm the recording respects the parent volume.
3. Start streaming/output while playback continues and record another sample.
4. Change the parent volume while streaming, then mute and unmute the parent.

Expected: monitoring, recording, and streaming use the same parent gain; volume
changes take effect while output is active; and parent mute produces silence in
recording and streaming. The internal decoder must not produce an additional
independent audio path.

## Shared-source scene transitions (#58)

1. Create one Media Playlist Source instance and reference that same source in
   Scene A and Scene B.
2. Select a visible-duration transition such as a 2-second Fade.
3. Set the parent source mixer volume to approximately `-30 dB` and start
   recording or streaming.
4. Transition A -> B and B -> A several times.
5. Repeat with the Media Playlist Source parent completely muted.

Expected: there is no full-volume burst during either transition, and the
parent gain remains effective for the entire transition. With the parent muted,
the output remains silent throughout. Monitoring and encoded output must agree.

## Playlist Queue and Playlist Control dock lifecycle

With the plugin built with the frontend and Qt options enabled:

1. Open OBS's **Docks** menu and enable **Playlist Queue** and
   **Playlist Control**.
2. Create two Media Playlist Source instances with different names and
   playlists; confirm both appear in each dock's selector.
3. In **Playlist Queue**, verify the separated **Previous**, **Now Playing**,
   and **Up Next** sections show actual filenames, including a folder child.
   Confirm long names are elided in the dock and the full path is available in
   the tooltip.
4. In **Playlist Queue**, verify progress, elapsed/duration, and remaining
   time for normal video, audio-only media, short files, and media with
   unknown duration. Confirm filenames reset to `—` and timing resets to
   `--:--` when no media is active.
5. Use sequential and shuffle playback, with loop both enabled and disabled;
   press **Next**, **Previous**, manually select an item, restart, stop, and
   activate/deactivate the source. Confirm Queue's **Up Next** is the actual
   canonical target and opening, closing, refreshing, or timing updates do not
   change shuffle order.
6. In **Playlist Control**, verify standalone files and expanded folder
   children are listed with their configured order and canonical item indices.
   Folder rows must not be playable selections.
7. Click a playable row and confirm that selection alone does not start media.
   Press **Play Selected** and confirm it starts the selected standalone file or
   folder child. Confirm the current item has the `▶`/bold indication and the
   button is disabled with no playable selection.
8. Switch Control between both MPS sources, enable Shuffle, and confirm the
   list remains the configured media collection rather than being presented as
   the future shuffle queue. Preserve a manually inspected row and scroll
   position while playback changes.
9. Rename the selected source and confirm each selector label updates while
   its selected UUID and displayed state remain unchanged. Remove the selected
   source from the scene collection, then destroy it and create another Media
   Playlist Source.
10. Bind Queue to MPS A and Control to MPS B. Change playback in both sources
    and confirm notifications update only the corresponding selected state.

Expected: both independent docks follow only their own selected source, clear
their selectors and content when that source disappears, and remain safe during
source rename, removal, scene-collection changes, and OBS shutdown. No crash,
stale source pointer, accidental playback on row click, or cross-contamination
between the docks is observed.

## Playlist dock Program follow, duration, and layout

1. Leave **Follow Program MPS** disabled in both docks and confirm both source
   selectors retain their existing independent manual behavior.
2. Enable **Follow Program MPS** from each dock's background or Source-area
   context menu. Confirm the visible status reads **Program** when the Program
   scene contains exactly one visible MPS and that each dock selects it.
3. In Studio Mode, put MPS A in Preview and MPS B in Program. Confirm both
   following docks select B and never follow Preview.
4. Switch Program to a scene with no visible MPS. Confirm the status reads
   **No active MPS** and the previous/manual source selection is retained.
5. Put two visible MPS sources in Program. Confirm the status reads
   **2 active MPS**, an already active selected MPS remains selected, and a
   non-active selected source is not replaced by an arbitrary candidate.
6. Manually select one of multiple active MPS sources while Follow remains on,
   then switch to a Program scene containing exactly one MPS. Confirm the manual
   choice is accepted first and the later unambiguous scene is followed.
7. Repeat with an MPS inside a nested scene, a group, and nested combinations.
   Hide each ancestor item in turn and confirm the contained MPS is excluded.
8. Reference the same MPS more than once in Program and confirm it is counted
   once. Exercise scene graphs that reference one another and confirm traversal
   remains bounded and OBS stays responsive.
9. Keep Queue following MPS A while Control is manually bound to MPS B, then
   enable Follow independently in Control. Confirm the two settings and displays
   never cross-contaminate.
10. Play several items and confirm Queue shows cached duration for Previous,
    canonical active duration for Now Playing, and standby duration for Up Next
    only when already safely available. Confirm unknown, URL, audio-only, and
    non-prefetched targets leave Up Next duration blank when unavailable,
    without extra media sources, probing, or playback changes.
11. Check Queue near 280-320 px, 400-500 px, and wider widths. Confirm filenames
    remain left aligned and elide with full-path tooltips, durations stay right
    aligned, separators remain coherent, and no horizontal scrollbar or large
    minimum width appears.
12. Check Control at the same widths with standalone and folder-child entries.
    Confirm marker, number, and filename columns align; current and selected rows
    remain distinct; **Shuffle ON** is compact; and the full-width native
    **Play Selected** button is usable.
13. Confirm selecting a Control row never starts playback. Confirm the Selected
    filename elides with a full-path tooltip and stable-ID selection still
    survives reorder or clears when removed.
14. Repeat the dock layout checks under available OBS light and dark themes.
    Confirm native palette text, separators, progress bar, tree selection, and
    play icon remain legible without fixed theme colors.

## Final playlist dock polish checks

A. Enable Queue Follow on MPS A, then manually choose MPS B. Confirm Queue
   remains on B, Follow is unchecked, and the Program status is hidden. Queue
   may retain its lightweight graph listener for source-dropdown markers;
   Control should release its Follow-only scene listeners. Repeat independently
   in Control.
B. With Follow disabled, put MPS A and MPS C visibly in Program. Confirm both
   entries have the native play indicator and emphasized text in Queue's source
   dropdown, while no source is selected automatically. With Follow enabled and
   two active sources, confirm both remain marked and no arbitrary source is
   selected.
C. Verify Control's marker column is centered, `#` and all numbers including
   `1.1`/`1.2` are centered, and `File` plus filenames are left aligned for
   standalone and folder-child rows.
D. Under dark and any available light OBS theme, confirm Previous, Current, and
   Up Next durations (including `--:--`) remain readable without overpowering
   filenames.
E. Resize both docks to approximately 280-320 px, 400-500 px, and a wide or
   floating layout. Confirm icons, header padding, filenames, duration values,
   and Play Selected remain usable without clipping or a new horizontal bar.

## Playlist lifecycle regression matrix

Use a local-video playlist with audio containing `A`, `B`, and `C`, and run the
following checks independently. Repeat the relevant rows with an audio-only
file and, where supported, a URL media item.

| Operation or state | Expected result |
| --- | --- |
| Automatic `A -> B -> C` playback | Continuous playback with no duplicate audio, dead decoder, or new gap between files. |
| **Next** and **Previous** | The requested item becomes current exactly once; no stale child or duplicate audio remains. |
| **Restart Current** during a pending switch | The pending target is cancelled and the current item restarts. |
| **Stop / Play** | Playback stops and resumes without a duplicate decoder or stale audio queue. |
| **Pause / Unpause** | Audio and video pause and resume together. |
| Scene deactivate/reactivate | Each visibility behavior retains its documented stop, pause, always-play, or play-next semantics. |
| **Always Play** while hidden | Playback continues through preloaded items while hidden; showing the source does not restart the standby item. |
| **Stop/Restart** and **Stop/Play Next** | The existing restart and next-item behavior remains deterministic across visibility changes. |
| Local video with audio | Preload and seamless A/B switching remain intact while the parent audio routing stays authoritative. |
| Audio-only media | The decoder remains live and audio is emitted once through the parent source. |
| URL media | Supported URLs start, switch, stop, and resume without invalid lifecycle state or duplicate audio. |

## Active item metadata after playlist reorder

1. Create a local-video playlist with at least `A`, `B`, and `C`; start `B`.
2. Reorder the playlist or insert an item before `B`, without changing `B`'s playlist ID or target.
3. Press Next, then press Stop before the new item completes its cut.
4. Restart Current File.

Expected: the current logical item is `B`, including when `B` is a folder item. A duplicate path elsewhere in the playlist must not be selected instead.

## Always Play while hidden

1. Create a local-video playlist `A -> B -> C` and choose **Always Play even when not visible**.
2. Show the source while `A` starts, then hide it.
3. Wait longer than the combined duration of `A` and `B`.
4. Show the source again.

Expected: playback has advanced into the expected position of `B` or `C`; it is not stopped on `B`'s preloaded first frame. Repeat after changing the playlist/current target while hidden, with Pause/Unpause selected, to confirm the source remains paused until shown.

At the `A -> B` boundary, `B` must begin actual playback while the source is hidden (rather than only displaying its preloaded first frame). Showing the parent again must not be required to start `B`.

## Bootstrap and restart lifecycle

Use a saved local-video playlist with at least `A` and `B`:

1. Start OBS while the playlist scene is current. The first file must start without pressing **Next**.
2. Start OBS on another scene, then switch to the playlist scene. The first file must start without pressing **Next**.
3. After playback is initialized, leave the playlist scene while `A` is active and `B` is pending, then return. Existing playback behavior must be preserved.
4. With `A` active, request **Next** to `B`, then use **Restart Current** before `B` is activated. `B` must be cancelled; `A` remains current and restarts.
5. Verify that a prefetched standby `B` with `child_added == false` does not activate or play while hidden.
6. Check **Stop/Restart**, **Pause/Unpause**, **Always Play**, and **Stop/Play Next** independently during activation and scene visibility changes. There must be no duplicate enumeration, audio, or decoders.
7. Let `A` end naturally. The normal `A -> B` transition must remain seamless.

## Local-video decoder start lifecycle

Use a local-video playlist `A -> B` and verify each behavior independently:

1. **Always Play**, visible: let `A` end. `B` must play rather than remain on its first preloaded frame.
2. **Always Play**, hidden: hide during `A`, then let `A` end. `B` must advance while hidden.
3. **Pause/Unpause**, visible: let `A` end. `B` must play.
4. **Pause/Unpause**, hidden: hide during `A`, change the selected/current target to `B`, and wait. `B` must remain stopped while hidden; after showing the parent, it must start rather than remain on its preloaded frame.
5. **Stop/Restart** and **Stop/Play Next**: hide and show around a pending local-video transition. Each must retain its existing stop/restart or stop/select-next behavior.
