#ifdef NDEBUG
#undef NDEBUG
#endif

#include "program-sources.hpp"

#include <cassert>
#include <cstring>
#include <string>

namespace {

const char *test_source_name(void *)
{
	return "Program test source";
}

void *test_source_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

void test_source_destroy(void *) {}

bool test_transition_audio(void *, uint64_t *, obs_source_audio_mix *, uint32_t, size_t, size_t)
{
	return false;
}

void register_test_sources()
{
	obs_source_info info{};
	info.id = kMpsSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.get_name = test_source_name;
	info.create = test_source_create;
	info.destroy = test_source_destroy;
	obs_register_source(&info);
	info.id = "mps_test_transition";
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.audio_render = test_transition_audio;
	obs_register_source(&info);
}

void expect_only_playlist(obs_source_t *output, obs_source_t *expected)
{
	const auto program = mps_program_sources(output);
	assert(program.playlists.size() == 1);
	assert(program.playlists.front() == expected);
}

void drain_destroy_queue()
{
	obs_queue_task(
		OBS_TASK_DESTROY, [](void *) {}, nullptr, true);
}

void test_studio_mode_copies()
{
	OBSSourceAutoRelease playlist = obs_source_create(kMpsSourceId, "Playlist A", nullptr, nullptr);
	OBSSceneAutoRelease original = obs_scene_create("Preview A");
	OBSSourceAutoRelease transition = obs_source_create_private("mps_test_transition", "Program", nullptr);
	assert(playlist && original && transition);
	obs_sceneitem_t *item = obs_scene_add(original, playlist);
	assert(item && obs_scene_add(original, playlist));
	obs_source_t *original_source = obs_scene_get_source(original);

	/* Studio Mode off and Studio Mode with Duplicate Scene disabled both
	 * use the public scene. Multiple items still represent one source. */
	expect_only_playlist(original_source, playlist);
	obs_transition_set(transition, original_source);
	expect_only_playlist(transition, playlist);

	/* Duplicate Scene with Edit Properties off snapshots the scene items,
	 * but it shares the source and therefore the playback cursor. */
	OBSSceneAutoRelease refs = obs_scene_duplicate(original, "Program refs", OBS_SCENE_DUP_PRIVATE_REFS);
	assert(refs);
	obs_source_t *refs_source = obs_scene_get_source(refs);
	obs_transition_set(transition, refs_source);
	assert(refs_source != original_source);
	assert(std::strcmp(obs_source_get_uuid(refs_source), obs_source_get_uuid(original_source)) != 0);
	obs_sceneitem_remove(item);
	obs_sceneitem_t *remaining = obs_scene_find_source(original, "Playlist A");
	assert(remaining);
	assert(obs_sceneitem_set_visible(remaining, false));
	assert(mps_program_sources(original_source).playlists.empty());
	expect_only_playlist(transition, playlist);
	assert(obs_sceneitem_set_visible(remaining, true));

	/* Edit Properties on creates a private source with a separate UUID.
	 * UUID lookup can resolve it while public source enumeration skips it. */
	OBSSceneAutoRelease copy = obs_scene_duplicate(original, "Program copy", OBS_SCENE_DUP_PRIVATE_COPY);
	assert(copy);
	obs_transition_set(transition, obs_scene_get_source(copy));
	std::string private_uuid;
	{
		const auto program = mps_program_sources(transition);
		assert(program.playlists.size() == 1);
		obs_source_t *private_playlist = program.playlists.front();
		assert(private_playlist != playlist.Get());
		private_uuid = obs_source_get_uuid(private_playlist);
		assert(private_uuid != obs_source_get_uuid(playlist));
		OBSSourceAutoRelease lookup = obs_get_source_by_uuid(private_uuid.c_str());
		assert(lookup == private_playlist);
		obs_enum_sources(
			[](void *data, obs_source_t *source) {
				assert(source != data);
				return true;
			},
			private_playlist);
		obs_source_remove(playlist);
		assert(mps_program_sources(original_source).playlists.empty());
		expect_only_playlist(transition, private_playlist);
	}
	obs_transition_set(transition, nullptr);
	copy = nullptr;
	drain_destroy_queue();
	OBSSourceAutoRelease expired = obs_get_source_by_uuid(private_uuid.c_str());
	assert(!expired);
	assert(mps_program_sources(transition).playlists.empty());
	refs = nullptr;
	obs_source_remove(original_source);
	original = nullptr;
	drain_destroy_queue();
}

void test_nested_scenes_and_transition()
{
	OBSSourceAutoRelease first = obs_source_create(kMpsSourceId, "First", nullptr, nullptr);
	OBSSourceAutoRelease second = obs_source_create(kMpsSourceId, "Second", nullptr, nullptr);
	OBSSceneAutoRelease outgoing = obs_scene_create("Outgoing");
	OBSSceneAutoRelease incoming = obs_scene_create("Incoming");
	OBSSceneAutoRelease nested = obs_scene_create("Nested");
	OBSSourceAutoRelease transition = obs_source_create_private("mps_test_transition", "Fade", nullptr);
	assert(first && second && outgoing && incoming && nested && transition);
	obs_sceneitem_t *nested_item = obs_scene_add(outgoing, obs_scene_get_source(nested));
	obs_sceneitem_t *group = obs_scene_add_group(nested, "Group");
	assert(nested_item && group);
	assert(obs_scene_add(obs_sceneitem_group_get_scene(group), first));
	assert(obs_scene_add(incoming, first));
	obs_source_t *outgoing_source = obs_scene_get_source(outgoing);
	obs_source_t *incoming_source = obs_scene_get_source(incoming);
	assert(mps_program_sources(outgoing_source).scenes.size() == 3);
	assert(obs_sceneitem_set_visible(nested_item, false));
	assert(mps_program_sources(outgoing_source).playlists.empty());
	assert(obs_sceneitem_set_visible(nested_item, true));

	obs_transition_set(transition, outgoing_source);
	assert(obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, 500, incoming_source));
	expect_only_playlist(transition, first);
	assert(obs_scene_add(incoming, second));
	assert(mps_program_sources(transition).playlists.size() == 2);
	obs_source_remove(first);
	expect_only_playlist(transition, second);
	obs_transition_set(transition, incoming_source);
	expect_only_playlist(transition, second);
	obs_transition_set(transition, nullptr);
	obs_sceneitem_remove(group);
	obs_source_remove(outgoing_source);
	obs_source_remove(incoming_source);
	outgoing = nullptr;
	incoming = nullptr;
	drain_destroy_queue();
	obs_source_remove(obs_scene_get_source(nested));
	nested = nullptr;
	drain_destroy_queue();
}

} // namespace

int main()
{
	assert(obs_startup("en-US", nullptr, nullptr));
	register_test_sources();
	assert(mps_program_sources(nullptr).playlists.empty());
	test_studio_mode_copies();
	test_nested_scenes_and_transition();
	drain_destroy_queue();
	obs_shutdown();
}
