#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <util/bmem.h>
#include <util/dstr.h>

#include "shuffler.h"

#define ITEM_COUNT 20

static void init_items(struct darray *files, size_t count)
{
	DARRAY(struct media_file_data) items;
	items.da = *files;

	for (size_t i = 0; i < count; i++) {
		struct media_file_data data = {0};
		struct dstr id = {0};

		dstr_printf(&id, "%zu", i + 1);
		data.id = bstrdup(id.array);
		data.index = i;
		da_init(data.folder_items);
		da_push_back(items, &data);
		dstr_free(&id);
	}

	*files = items.da;
}

static void free_items(struct darray *files)
{
	DARRAY(struct media_file_data) items;
	items.da = *files;

	for (size_t i = 0; i < items.num; i++) {
		bfree(items.array[i].id);
		da_free(items.array[i].folder_items);
	}
	da_free(items);
	*files = items.da;
}

static void init_seeded_shuffler(struct shuffler *s, uint64_t seed, struct darray *files)
{
	shuffler_init(s);
	shuffler_set_seed(s, seed);
	shuffler_update_files(s, files);
}

static void collect_sequence(struct shuffler *s, size_t *sequence, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		assert(shuffler_has_next(s));
		struct media_file_data *item = shuffler_next(s);
		assert(item);
		sequence[i] = item->index;
	}
	assert(!shuffler_has_next(s));
}

static void test_same_seed_is_deterministic(void)
{
	struct darray files = {0};
	struct shuffler first;
	struct shuffler second;
	size_t first_sequence[ITEM_COUNT];
	size_t second_sequence[ITEM_COUNT];

	init_items(&files, ITEM_COUNT);
	init_seeded_shuffler(&first, 0x0123456789abcdefULL, &files);
	init_seeded_shuffler(&second, 0x0123456789abcdefULL, &files);
	collect_sequence(&first, first_sequence, ITEM_COUNT);
	collect_sequence(&second, second_sequence, ITEM_COUNT);
	assert(memcmp(first_sequence, second_sequence, sizeof(first_sequence)) == 0);

	shuffler_destroy(&first);
	shuffler_destroy(&second);
	free_items(&files);
}

static void test_seeded_shufflers_have_independent_streams(void)
{
	struct darray files = {0};
	struct shuffler first;
	struct shuffler second;
	size_t solo_first[ITEM_COUNT];
	size_t solo_second[ITEM_COUNT];
	size_t interleaved_first[ITEM_COUNT];
	size_t interleaved_second[ITEM_COUNT];

	init_items(&files, ITEM_COUNT);

	init_seeded_shuffler(&first, 0x1111111111111111ULL, &files);
	collect_sequence(&first, solo_first, ITEM_COUNT);
	shuffler_destroy(&first);

	init_seeded_shuffler(&second, 0x2222222222222222ULL, &files);
	collect_sequence(&second, solo_second, ITEM_COUNT);
	shuffler_destroy(&second);

	init_seeded_shuffler(&first, 0x1111111111111111ULL, &files);
	init_seeded_shuffler(&second, 0x2222222222222222ULL, &files);
	for (size_t i = 0; i < ITEM_COUNT; i++) {
		struct media_file_data *first_item = shuffler_next(&first);
		struct media_file_data *second_item = shuffler_next(&second);

		assert(first_item);
		assert(second_item);
		interleaved_first[i] = first_item->index;
		interleaved_second[i] = second_item->index;
	}

	/* Interleaving must not advance the other instance's stream. */
	assert(memcmp(solo_first, interleaved_first, sizeof(solo_first)) == 0);
	assert(memcmp(solo_second, interleaved_second, sizeof(solo_second)) == 0);

	shuffler_destroy(&first);
	shuffler_destroy(&second);
	free_items(&files);
}

static void test_reshuffle_avoid_does_not_repeat_avoided_item(void)
{
	struct darray files = {0};
	struct shuffler shuffler;
	struct media_file_data *avoided;
	DARRAY(struct media_file_data) items;

	init_items(&files, ITEM_COUNT);
	items.da = files;
	init_seeded_shuffler(&shuffler, 0x5555555555555555ULL, &files);
	avoided = &items.array[7];

	for (size_t cycle = 0; cycle < 50; cycle++) {
		shuffler_reshuffle_avoid(&shuffler, avoided);
		assert(shuffler.head == 1);
		assert(shuffler.next == 0);
		assert(shuffler.history == ITEM_COUNT);
		assert(shuffler_has_next(&shuffler));
		assert(!shuffler_has_prev(&shuffler));
		assert(shuffler_next(&shuffler) != avoided);
	}

	/* An item not present in the shuffler falls back to a plain reshuffle. */
	struct media_file_data absent = {0};
	absent.id = "absent";
	shuffler_reshuffle_avoid(&shuffler, &absent);
	assert(shuffler.head == 0);
	assert(shuffler.next == 0);
	assert(shuffler.history == ITEM_COUNT);

	shuffler_destroy(&shuffler);
	free_items(&files);
}

static void test_reshuffle_avoid_single_item(void)
{
	struct darray files = {0};
	struct shuffler shuffler;
	DARRAY(struct media_file_data) items;

	init_items(&files, 1);
	items.da = files;
	init_seeded_shuffler(&shuffler, 0x7777777777777777ULL, &files);
	shuffler_reshuffle_avoid(&shuffler, &items.array[0]);
	assert(shuffler.head == 0);
	assert(shuffler.next == 0);
	assert(shuffler.history == 1);
	assert(shuffler_next(&shuffler) == &items.array[0]);

	shuffler_destroy(&shuffler);
	free_items(&files);
}

static void test_update_files_after_history_wraps_to_zero(void)
{
	struct darray files = {0};
	struct shuffler shuffler;

	init_items(&files, 4);
	init_seeded_shuffler(&shuffler, 0x3333333333333333ULL, &files);
	shuffler_set_loop(&shuffler, true);

	for (size_t i = 0; i < 4; i++)
		assert(shuffler_next(&shuffler));

	/* Start a new cycle, consume its first item, then make manual
	 * selections until the history index wraps to zero. */
	assert(shuffler_peek_next(&shuffler));
	assert(shuffler_next(&shuffler));
	for (size_t i = 0; i < 3; i++)
		shuffler_select(&shuffler, shuffler.shuffled_files.array[3]);
	assert(shuffler.history == 0);

	shuffler_update_files(&shuffler, &files);
	assert(shuffler.head == 4);
	assert(shuffler.next == 4);
	assert(shuffler.history == 4);
	assert(shuffler_has_next(&shuffler));
	assert(shuffler_next(&shuffler));

	shuffler_destroy(&shuffler);
	free_items(&files);
}

int main(void)
{
	int result = test_shuffler();
	assert(result == 0);

	test_same_seed_is_deterministic();
	test_seeded_shufflers_have_independent_streams();
	test_reshuffle_avoid_does_not_repeat_avoided_item();
	test_reshuffle_avoid_single_item();
	test_update_files_after_history_wraps_to_zero();

	puts("shuffler tests passed");
	return result;
}
