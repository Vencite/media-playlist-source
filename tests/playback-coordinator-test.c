#include <stdio.h>
#include <stdlib.h>

#include "playback-coordinator.h"

#define CHECK(condition)                                                                                \
	do {                                                                                            \
		if (!(condition)) {                                                                     \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE);                                                             \
		}                                                                                       \
	} while (false)

static void test_latest_rapid_request_wins(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t first_generation;
	uint64_t generation;
	enum mps_switch_request request;

	mps_coordinator_init(&coordinator);
	first_generation = mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 4, false);
	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	CHECK(generation == first_generation);
	CHECK(request == MPS_SWITCH_NEXT);

	mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 4, false);
	mps_coordinator_request(&coordinator, MPS_SWITCH_PREVIOUS, 4, false);
	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	CHECK(request == MPS_SWITCH_PREVIOUS);
	CHECK(generation > first_generation);
}

static void test_late_callback_cannot_complete_new_request(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t old_generation;
	uint64_t current_generation;

	mps_coordinator_init(&coordinator);
	old_generation = mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 2, false);
	mps_coordinator_request(&coordinator, MPS_SWITCH_PREVIOUS, 2, false);
	current_generation = coordinator.generation;

	CHECK(!mps_coordinator_matches(&coordinator, old_generation));
	CHECK(!mps_coordinator_complete(&coordinator, old_generation));
	CHECK(mps_coordinator_matches(&coordinator, current_generation));
	CHECK(coordinator.request_pending);
}

static void test_failures_are_bounded_by_candidate_count(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t generation;
	enum mps_switch_request request;

	mps_coordinator_init(&coordinator);
	mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 3, false);

	for (size_t i = 0; i < 2; i++) {
		CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
		CHECK(mps_coordinator_fail(&coordinator, generation) == MPS_SWITCH_FAILURE_RETRY);
		CHECK(coordinator.request_pending);
	}

	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	CHECK(mps_coordinator_fail(&coordinator, generation) == MPS_SWITCH_FAILURE_EXHAUSTED);
	CHECK(!coordinator.request_pending);
	CHECK(!coordinator.preparing);
}

static void test_stale_failure_does_not_consume_retry_budget(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t old_generation;

	mps_coordinator_init(&coordinator);
	old_generation = mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 2, false);
	mps_coordinator_request(&coordinator, MPS_SWITCH_PREVIOUS, 2, false);

	CHECK(mps_coordinator_fail(&coordinator, old_generation) == MPS_SWITCH_FAILURE_STALE);
	CHECK(coordinator.failed_candidates == 0);
}

static void test_successful_completion_resets_request_state(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t generation;
	enum mps_switch_request request;

	mps_coordinator_init(&coordinator);
	generation = mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 2, false);
	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	CHECK(mps_coordinator_complete(&coordinator, generation));
	CHECK(!coordinator.request_pending);
	CHECK(!coordinator.preparing);
	CHECK(coordinator.failed_candidates == 0);
}

static void test_retry_request_preserves_failure_count(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t generation;
	enum mps_switch_request request;

	mps_coordinator_init(&coordinator);
	mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 3, false);
	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	CHECK(mps_coordinator_fail(&coordinator, generation) == MPS_SWITCH_FAILURE_RETRY);
	mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 3, true);
	CHECK(coordinator.failed_candidates == 1);
}

static void test_cancel_invalidates_in_flight_generation(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t generation;
	uint64_t replacement_generation;
	enum mps_switch_request request;

	mps_coordinator_init(&coordinator);
	mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 2, false);
	CHECK(mps_coordinator_take_request(&coordinator, &generation, &request));
	mps_coordinator_cancel(&coordinator);
	CHECK(!mps_coordinator_matches(&coordinator, generation));
	CHECK(!mps_coordinator_complete(&coordinator, generation));
	CHECK(mps_coordinator_fail(&coordinator, generation) == MPS_SWITCH_FAILURE_STALE);
	CHECK(!coordinator.request_pending);
	CHECK(!coordinator.preparing);
	replacement_generation = mps_coordinator_request(&coordinator, MPS_SWITCH_PREVIOUS, 2, false);
	CHECK(replacement_generation != generation);
	CHECK(!mps_coordinator_complete(&coordinator, generation));
	CHECK(mps_coordinator_matches(&coordinator, replacement_generation));
}

static void test_restart_preserves_bootstrap_without_active_source(void)
{
	struct mps_playback_coordinator coordinator;
	uint64_t bootstrap_generation;

	mps_coordinator_init(&coordinator);
	bootstrap_generation = mps_coordinator_request(&coordinator, MPS_SWITCH_NEXT, 2, false);
	CHECK(!mps_coordinator_cancel_for_restart(&coordinator, false));
	CHECK(coordinator.request_pending);
	CHECK(coordinator.generation == bootstrap_generation);
	CHECK(mps_coordinator_cancel_for_restart(&coordinator, true));
	CHECK(!coordinator.request_pending);
}

static void test_active_child_enumeration_policy(void)
{
	CHECK(mps_source_is_active_child(true, false));
	CHECK(mps_source_is_active_child(true, true));
	CHECK(mps_source_is_active_child(false, true));
	CHECK(!mps_source_is_active_child(false, false));
}

int main(void)
{
	test_latest_rapid_request_wins();
	test_late_callback_cannot_complete_new_request();
	test_failures_are_bounded_by_candidate_count();
	test_stale_failure_does_not_consume_retry_budget();
	test_successful_completion_resets_request_state();
	test_retry_request_preserves_failure_count();
	test_cancel_invalidates_in_flight_generation();
	test_restart_preserves_bootstrap_without_active_source();
	test_active_child_enumeration_policy();
	puts("playback coordinator tests passed");
	return 0;
}
