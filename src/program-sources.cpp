#include "program-sources.hpp"

#include <cstring>
#include <unordered_set>

namespace {

constexpr unsigned kMaxSceneDepth = 64;

struct ProgramTraversal {
	MpsProgramSources sources;
	std::unordered_set<obs_source_t *> visited_scenes;
	std::unordered_set<obs_source_t *> visited_playlists;
};

void visit_scene(obs_scene_t *scene, ProgramTraversal &traversal, unsigned depth)
{
	if (!scene || depth > kMaxSceneDepth)
		return;
	obs_source_t *source = obs_scene_get_source(scene);
	if (obs_source_removed(source) || !traversal.visited_scenes.insert(source).second)
		return;
	traversal.sources.scenes.emplace_back(source);

	struct ItemTraversal {
		ProgramTraversal &traversal;
		unsigned depth;
	} items{traversal, depth};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			if (!obs_sceneitem_visible(item))
				return true;
			auto &items = *static_cast<ItemTraversal *>(data);
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (mps_is_playlist_source(source) && !obs_source_removed(source) &&
			    items.traversal.visited_playlists.insert(source).second)
				items.traversal.sources.playlists.emplace_back(source);
			obs_scene_t *nested = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
									   : obs_scene_from_source(source);
			visit_scene(nested, items.traversal, items.depth + 1);
			return true;
		},
		&items);
}

} // namespace

bool mps_is_playlist_source(obs_source_t *source)
{
	if (!source)
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return id && std::strcmp(id, kMpsSourceId) == 0;
}

MpsProgramSources mps_program_sources(obs_source_t *output)
{
	ProgramTraversal traversal;
	if (!output)
		return {};
	if (obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION) {
		/* OBS promotes B to A when both video and audio finish. Read B first
		 * so completion between the reads cannot drop the incoming scene. */
		OBSSourceAutoRelease incoming = obs_transition_get_source(output, OBS_TRANSITION_SOURCE_B);
		OBSSourceAutoRelease outgoing = obs_transition_get_source(output, OBS_TRANSITION_SOURCE_A);
		if (outgoing)
			visit_scene(obs_scene_from_source(outgoing), traversal, 0);
		if (incoming)
			visit_scene(obs_scene_from_source(incoming), traversal, 0);
	} else {
		visit_scene(obs_scene_from_source(output), traversal, 0);
	}
	return std::move(traversal.sources);
}
