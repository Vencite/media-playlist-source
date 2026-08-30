#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mps_switch_request {
	MPS_SWITCH_NONE,
	MPS_SWITCH_NEXT,
	MPS_SWITCH_PREVIOUS,
	MPS_SWITCH_SELECT,
};

enum mps_switch_failure {
	MPS_SWITCH_FAILURE_STALE,
	MPS_SWITCH_FAILURE_RETRY,
	MPS_SWITCH_FAILURE_EXHAUSTED,
};

struct mps_playback_coordinator {
	uint64_t generation;
	enum mps_switch_request request;
	size_t failed_candidates;
	size_t candidate_limit;
	bool request_pending;
	bool preparing;
};

void mps_coordinator_init(struct mps_playback_coordinator *coordinator);
uint64_t mps_coordinator_request(struct mps_playback_coordinator *coordinator, enum mps_switch_request request,
				 size_t candidate_limit, bool preserve_failures);
bool mps_coordinator_take_request(struct mps_playback_coordinator *coordinator, uint64_t *generation,
				  enum mps_switch_request *request);
bool mps_coordinator_matches(const struct mps_playback_coordinator *coordinator, uint64_t generation);
void mps_coordinator_cancel(struct mps_playback_coordinator *coordinator);
bool mps_coordinator_cancel_for_restart(struct mps_playback_coordinator *coordinator, bool active_source_present);
enum mps_switch_failure mps_coordinator_fail(struct mps_playback_coordinator *coordinator, uint64_t generation);
bool mps_coordinator_complete(struct mps_playback_coordinator *coordinator, uint64_t generation);
