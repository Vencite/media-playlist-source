# Media Playlist Source

[![Latest fork release](https://img.shields.io/github/v/release/Vencite/media-playlist-source?include_prereleases&display_name=tag&sort=date&label=latest%20fork%20release)](https://github.com/Vencite/media-playlist-source/releases)
[![Upstream](https://img.shields.io/badge/upstream-CodeYan01%2Fmedia--playlist--source-181717?logo=github&logoColor=white)](https://github.com/CodeYan01/media-playlist-source)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-tested%20on%20OBS%2032-302E31?logo=obsstudio&logoColor=white)](https://obsproject.com/)
[![License](https://img.shields.io/github/license/Vencite/media-playlist-source?label=license)](https://github.com/Vencite/media-playlist-source/blob/master/LICENSE)

## About this fork

This repository is an independently maintained fork of
[CodeYan01/media-playlist-source](https://github.com/CodeYan01/media-playlist-source).
The original plugin and most of its original code were created by Ian Rodriguez /
CodeYan01; this fork is maintained and developed by Vencite.

The fork began with a practical production problem: while using the plugin in
OBS, I found a short transparent or blank frame between consecutive playlist
files. Because other sources were below the playlist in my scene, that single
frame was visible on the live broadcast. I wanted playback behavior that was
closer to seamless playback, which led to the dual-source A/B playback and
preloading used here.

I use this fork in my own production environment and intend to continue
maintaining and developing it independently as long as it remains useful.

A fair amount of this fork is developed with AI-assisted coding tools. I am not
a full-time OBS plugin developer, so these tools help me understand and modify
the codebase. I still treat production reliability seriously: changes are
reviewed, compiled, tested in CI, and validated in OBS before I rely on them
during live production.

The upstream repository remains the home of the original project. If you want
the upstream version, use
[CodeYan01/media-playlist-source](https://github.com/CodeYan01/media-playlist-source).

## What it is

Media Playlist Source is an OBS input source that serves as an alternative to
VLC Video Source. It uses Media Source internally and can play a playlist of
local files, folders, and URLs supported by FFmpeg.

## Features

- Allows editing the playlist without restarting the video, even if files are
reordered.
- Allows editing any setting without restarting the video.
- Saves the currently playing file so it would be played when OBS restarts.
- Allows selecting a file or folder item from the list to play.
- Shuffling based on VLC 4's implementation:
  - Allows adding/removing items from the playlist without the need to
reshuffle. The shuffling order of already played items will be saved, so
clicking Previous Item will still play the previous item. Selecting a file or
folder item also does not break the history.
  - Reshuffles when the last file in the playlist is played out, without
affecting history.
- Shows the filename of the current file in the Properties window.
- Has an option to play the first file or the current file when the source is
restarted.
- Preloads the next local video to support seamless A/B playlist switching.

## Limitations

- The Properties window will show the filename of the current file, but it can
not be automatically updated when the video ends as it could cause OBS to crash
when the video ends while interacting with the Properties window. Reopening the
Properties window will refresh the shown current file.
- Preserving the current item when editable-list entries are edited or
reordered depends on OBS preserving the custom UUID fields used by this plugin;
see [OBS PR #8051](https://github.com/obsproject/obs-studio/pull/8051).
- Does not support audio track or subtitle selection yet.
- Does not automatically refresh folder contents yet, but can be manually done
by saving the settings again.

## Releases

Download current Vencite fork builds from the
[GitHub Releases page](https://github.com/Vencite/media-playlist-source/releases).
Stable fork releases use the `vMAJOR.MINOR.PATCH-ven` tag convention; fork
prereleases append a numeric suffix, such as `vMAJOR.MINOR.PATCH-ven.1`.

## For Developers
To find out the keys used in the source [settings](https://docs.obsproject.com/reference-sources#c.obs_source_get_settings),
use [obs_data_get_json](https://docs.obsproject.com/reference-settings#c.obs_data_get_json)
to view it, or check the scene collection json. You could also check
[src/media-playlist-source.c](src/media-playlist-source.c)

To select a file programmatically:
```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
struct calldata cd = {0};
calldata_set_int(&cd, "media_index", 3); // 4th file
calldata_set_int(&cd, "folder_item_index", 3) // 4th folder item of the 4th file
proc_handler_call(ph, "select_index", &cd);
calldata_free(&cd);
```
`media_index` - the 0-based index of the file in the playlist

`folder_item_index` - the 0-based index of the folder item in the folder at `media_index`

If the file at `media_index` is not a folder, the second parameter is ignored.
If `media_index` is higher than the playlist item count, it will be set to 0.
If `folder_item_index` is higher than the folder item count or `media_index`,
it will be set to 0.

## Upstream author

The original plugin was created by Ian Rodriguez / CodeYan01. For questions,
bug reports, or feature requests concerning the original project, please
contact the upstream author by mentioning @codeyan in the
[OBS Discord server](https://discord.gg/obsproject), in #plugins-and-tools.

Support for the upstream author's development can be provided through the
original author's [PayPal donation page](https://www.paypal.com/donate/?hosted_button_id=S9WJDUDB8CK5S).
