# Manual regression checks

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
3. After playback is initialized, leave the playlist scene and return. Existing playback behavior must be preserved.
4. With `A` active, request **Next** to `B`, then use **Restart Current** before `B` is activated. `B` must be cancelled; `A` remains current and restarts.
5. Check **Stop/Restart**, **Pause/Unpause**, **Always Play**, and **Stop/Play Next** independently during activation and scene visibility changes. None may create duplicate audio or duplicate decoders.
6. Let `A` end naturally. The normal `A -> B` transition must remain seamless.

## Local-video decoder start lifecycle

Use a local-video playlist `A -> B` and verify each behavior independently:

1. **Always Play**, visible: let `A` end. `B` must play rather than remain on its first preloaded frame.
2. **Always Play**, hidden: hide during `A`, then let `A` end. `B` must advance while hidden.
3. **Pause/Unpause**, visible: let `A` end. `B` must play.
4. **Pause/Unpause**, hidden: hide during `A`, change the selected/current target to `B`, and wait. `B` must remain stopped while hidden; after showing the parent, it must start rather than remain on its preloaded frame.
5. **Stop/Restart** and **Stop/Play Next**: hide and show around a pending local-video transition. Each must retain its existing stop/restart or stop/select-next behavior.
