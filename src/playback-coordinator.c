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
