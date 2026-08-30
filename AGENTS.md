# AGENTS.md

## Project

This repository is a fork of `CodeYan01/media-playlist-source`, an OBS Studio plugin implemented primarily in C.

Keep changes suitable for eventual upstream contribution:

- prefer small, focused diffs;
- follow the existing architecture, naming, and coding style;
- avoid unrelated refactors;
- preserve existing user-facing behavior unless the task explicitly requires changing it.

Important areas:

- `src/` - plugin implementation and playlist logic;
- `CMakeLists.txt`, `CMakePresets.json`, `cmake/` - build configuration;
- `.github/workflows/`, `.github/actions/` - authoritative CI behavior;
- `build-aux/` - formatting and build helpers.

Supported targets include Windows x64, macOS Universal, and Ubuntu x86_64.

## Before changing code

Before editing:

1. Inspect the working tree and current branch.
2. Read the complete control flow relevant to the task.
3. Check existing issues, commits, forks, PRs, and upstream OBS behavior when relevant.
4. Establish evidence for the root cause before implementing a fix.

Use:

```bash
git status
git branch --show-current
git remote -v

Expected remotes:

origin - Vencite/media-playlist-source
upstream - CodeYan01/media-playlist-source

Never push to upstream.

Implementation rules
Make the minimum change necessary to solve the requested problem correctly.
Preserve compatibility with existing OBS scene collections and saved settings.
Do not change public source IDs or persisted setting keys without a concrete requirement.
Do not add dependencies unless clearly necessary.
Do not introduce sleeps, arbitrary delays, timing hacks, or polling when a deterministic solution is possible.
Do not rewrite working code only for style.
Follow the repository's existing C conventions and formatting.
Treat audio callbacks, video rendering, source lifecycle, and threading as concurrency-sensitive.
Prefer explicit lifecycle/state handling over implicit timing assumptions.

This plugin is intended for live OBS use. Reliability and deterministic behavior are more important than cleverness.

Git safety

Never:

work directly on master;
use git reset --hard;
use destructive git clean commands;
force-push;
overwrite or revert unrelated user changes;
commit, push, create a tag, release, or pull request unless explicitly requested.

If the working tree contains unrelated changes, preserve them.

Build

CMakePresets.json is the source of truth for supported build presets.

On Linux/WSL, when dependencies are available:

cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64

Cross-platform validation is performed by GitHub Actions.

Relevant targets include:

Ubuntu x86_64;
Windows x64;
macOS Universal.

Do not claim a platform is verified until its corresponding build has actually passed.

Formatting

The repository CI uses:

clang-format for C/C++ source files;
gersemi for CMake files.

Use the repository-provided configuration and helper scripts.

Do not change formatting configuration merely to make a code change pass CI.

Bug fixes

For bug fixes:

Reproduce or characterize the failure before changing code when practical.
Confirm the root cause from code or runtime evidence.
Add a regression test when realistically possible.
Test the direct failure path and nearby lifecycle/edge cases.
If OBS rendering behavior cannot reasonably be automated, provide a precise manual regression procedure.

Avoid fixing symptoms when the underlying lifecycle or state problem can be corrected directly.

Completion

Before declaring work complete:

inspect the final diff;
verify there are no unintended changes;
run all relevant checks available in the current environment;
report exactly which checks were run and their results;
clearly identify anything still requiring CI or manual OBS testing.

Never describe an unrun check as passing.

Task-specific instructions

This file contains persistent repository rules only.

Detailed requirements, acceptance criteria, temporary constraints, and architecture for a specific task belong in the current task prompt and must be followed in addition to this file.
