# Media Playlist Source

[![Latest release](https://img.shields.io/github/v/release/Vencite/media-playlist-source?include_prereleases=true&label=latest%20release)](https://github.com/Vencite/media-playlist-source/releases)
[![Upstream](https://img.shields.io/badge/upstream-CodeYan01%2Fmedia--playlist--source-181717?logo=github&logoColor=white)](https://github.com/CodeYan01/media-playlist-source)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-tested%20on%20OBS%2032-302E31?logo=obsstudio&logoColor=white)](https://obsproject.com/)
[![License](https://img.shields.io/github/license/Vencite/media-playlist-source?label=license)](https://github.com/Vencite/media-playlist-source/blob/master/LICENSE)

## About this fork

This repository is a personal fork of [CodeYan01/media-playlist-source](https://github.com/CodeYan01/media-playlist-source). The original plugin and the majority of its code were created by Ian Rodriguez / CodeYan01.

This fork started from a practical production problem: while using the plugin in OBS, I found a short transparent or blank frame between consecutive playlist files. Because I have other sources below the playlist in my scene, that single frame was visible on the live broadcast. I wanted playback behavior that was closer to seamless playback.

The first major change in this fork is dual-source A/B playback with preloading of the next item. The fix has also been submitted to the upstream project as [CodeYan01/media-playlist-source#60](https://github.com/CodeYan01/media-playlist-source/pull/60).

I intend to use this fork in my own production environment and continue improving it when that remains useful.

In practical terms, a fair amount of this fork is "vibe coded" with AI assistance. I am not a full-time OBS plugin developer, so AI coding tools help me understand and modify the codebase. I still try to treat production reliability seriously: changes are reviewed, compiled, tested in CI, and validated in OBS before I rely on them during live production.

The upstream repository remains the right place for the original project. If you want the original upstream version, please use [CodeYan01/media-playlist-source](https://github.com/CodeYan01/media-playlist-source). This fork may contain experimental or additional changes.

## Fork-specific changes

### 0.1.3-ven.1

- Seamless dual-source A/B playback ([upstream PR #60](https://github.com/CodeYan01/media-playlist-source/pull/60)).
- Preloads the next local video before switching.
- Prevents blank or transparent frames between playlist items.
- Fixes startup/bootstrap behavior when OBS starts on another scene.
- Fixes hidden standby source activation and enumeration.
- OBS 32 compatibility and build updates.
- Additional playback coordinator tests and a manual regression checklist.

## Introduction

An OBS Plugin that serves as an alternative to VLC Video Source. It uses the
Media Source internally.

## Features

- Allows editing the playlist without restarting the video, even if files are
reordered.
- Allows editing any setting without restarting the video.
- Saves the currently playing file so it would be played when OBS restarts.
- Allows selecting a file or folder item from the list to play.
- Shuffling (Based on VLC 4's implementation)
- - Allows adding/removing items from the playlist without the need to
reshuffle. The shuffling order of already played items will be saved, so
clicking Previous Item will still play the previous item. Selecting a file or
folder item also does not break the history.
- - Reshuffles when the last file in the playlist is played out, without
affecting history.
- Shows the filename of the current file in the Properties window.
- Has an option to play the first file or the current file when the source is
restarted.

## Limitations

- The Properties window will show the filename of the current file, but it can
not be automatically updated when the video ends as it could cause OBS to crash
when the video ends while interacting with the Properties window. Reopening the
Properties window will refresh the shown current file.
- While this plugin works with OBS 28 and up, it requires this change at 
https://github.com/obsproject/obs-studio/pull/8051 that allows this plugin to
save the index of the current file, so that restarting playback is not needed
when editing the list. This change isn't merged yet as of OBS 29.1.3.
A custom build with this PR is available
[here](https://github.com/CodeYan01/obs-studio/releases).
- Does not support audio track or subtitle selection yet.
- Does not automatically refresh folder contents yet, but can be manually done
by saving the settings again.

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
