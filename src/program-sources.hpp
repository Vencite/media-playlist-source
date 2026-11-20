#pragma once

#include <obs.hpp>

#include <vector>

inline constexpr const char *kMpsSourceId = "media_playlist_source_codeyan";

struct MpsProgramSources {
	std::vector<OBSSource> playlists;
	std::vector<OBSSource> scenes;
};

bool mps_is_playlist_source(obs_source_t *source);
MpsProgramSources mps_program_sources(obs_source_t *output);
