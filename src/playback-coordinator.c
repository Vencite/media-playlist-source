#include "playback-coordinator.h"

#include <string.h>

void mps_coordinator_init(struct mps_playback_coordinator *coordinator)
{
	memset(coordinator, 0, sizeof(*coordinator));
}

uint64_t mps_coordinator_request(struct mps_playback_coordinator *coordinator, enum mps_switch_request request,
				 size_t candidate_limit, bool preserve_failures)
{
	coordinator->generation++;
	coordinator->request = request;
	coordinator->candidate_limit = candidate_limit;
	coordinator->request_pending = true;
	coordinator->preparing = false;
	if (!preserve_failures)
		coordinator->failed_candidates = 0;
	return coordinator->generation;
}

bool mps_coordinator_take_request(struct mps_playback_coordinator *coordinator, uint64_t *generation,
				  enum mps_switch_request *request)
{
	if (!coordinator->request_pending || coordinator->preparing)
		return false;

	coordinator->preparing = true;
	*generation = coordinator->generation;
	*request = coordinator->request;
	return true;
}

bool mps_coordinator_matches(const struct mps_playback_coordinator *coordinator, uint64_t generation)
{
	return coordinator->request_pending && coordinator->generation == generation;
}

void mps_coordinator_cancel(struct mps_playback_coordinator *coordinator)
{
	coordinator->generation++;
	coordinator->request = MPS_SWITCH_NONE;
	coordinator->failed_candidates = 0;
	coordinator->candidate_limit = 0;
	coordinator->request_pending = false;
	coordinator->preparing = false;
}

bool mps_coordinator_cancel_for_restart(struct mps_playback_coordinator *coordinator, bool active_source_present)
{
	if (!active_source_present || !coordinator->request_pending)
		return false;

	mps_coordinator_cancel(coordinator);
	return true;
}

bool mps_source_is_active_child(bool active_slot, bool child_added)
{
	return active_slot || child_added;
}

enum mps_switch_failure mps_coordinator_fail(struct mps_playback_coordinator *coordinator, uint64_t generation)
{
	if (!mps_coordinator_matches(coordinator, generation))
		return MPS_SWITCH_FAILURE_STALE;

	coordinator->failed_candidates++;
	coordinator->preparing = false;
	if (coordinator->failed_candidates >= coordinator->candidate_limit) {
		coordinator->request_pending = false;
		return MPS_SWITCH_FAILURE_EXHAUSTED;
	}

	coordinator->generation++;
	return MPS_SWITCH_FAILURE_RETRY;
}

bool mps_coordinator_complete(struct mps_playback_coordinator *coordinator, uint64_t generation)
{
	if (!mps_coordinator_matches(coordinator, generation))
		return false;

	coordinator->request_pending = false;
	coordinator->preparing = false;
	coordinator->failed_candidates = 0;
	return true;
}

uint64_t mps_audio_clock_timestamp(struct mps_audio_clock *clock, uint64_t decoder_ns, uint64_t capture_ns)
{
	/* Keep decoder spacing through delivery jitter. A seek or restart can
	 * move the decoder clock by much more than the time since capture. */
	const uint64_t max_skew_ns = 100000000;
	if (clock->initialized && decoder_ns >= clock->last_decoder_ns) {
		const uint64_t elapsed_ns = decoder_ns - clock->decoder_origin_ns;
		if (elapsed_ns <= UINT64_MAX - clock->capture_origin_ns) {
			const uint64_t mapped_ns = clock->capture_origin_ns + elapsed_ns;
			const uint64_t skew_ns = mapped_ns > capture_ns ? mapped_ns - capture_ns
									: capture_ns - mapped_ns;
			if (skew_ns <= max_skew_ns) {
				clock->last_decoder_ns = decoder_ns;
				return mapped_ns;
			}
		}
	}
	*clock = (struct mps_audio_clock){decoder_ns, capture_ns, decoder_ns, true};
	return capture_ns;
}
