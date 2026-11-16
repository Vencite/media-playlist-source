# Developer integration

For Companion and other external clients, use the [WebSocket API](playlist-telemetry.md).
It provides stable entry IDs, revision checks and selection on OBS's graphics thread.

## Source settings and entry IDs

Inspect source settings with `obs_source_get_settings()` / `obs_data_get_json()`,
or inspect the saved scene collection. Implementation: [media-playlist-source.c](../src/media-playlist-source.c).

When replacing the `playlist` array programmatically, preserve existing entry
`uuid` values and assign a distinct UUID to each new top-level entry, including
duplicate paths. MPS uses these IDs to retain current selection and shuffle history.
OBS supplies them when entries are edited through its Properties UI; this does
not cover JSON arrays constructed by external scripts.

## Legacy native selection

The `select_index` source procedure remains available for existing in-process
integrations. For new clients, prefer `GetPlaylistEntries` + `SelectPlaylistItem`.

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
struct calldata cd = {0};
calldata_set_int(&cd, "media_index", 3);
calldata_set_int(&cd, "folder_item_index", 3);
proc_handler_call(ph, "select_index", &cd);
calldata_free(&cd);
```

Indices are zero-based: this selects the fourth child of the fourth top-level
entry. For a standalone file, the child index is ignored. Invalid top-level
indices are ignored; an invalid child index falls back to zero.

Call on OBS's graphics thread, with a valid source reference, a valid playable
target and a non-empty target folder. Serialize the call with playlist updates;
indices can change after edits or folder monitoring.
