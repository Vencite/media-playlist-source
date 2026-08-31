#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "playlist-context.h"

static struct media_file_data file(const char *path)
{
	struct media_file_data media = {0};
	media.path = (char *)path;
	return media;
}

static void test_empty_playlist(void)
{
	struct darray files = {0};
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_context output;

	input.files = &files;
	mps_playlist_context_resolve(&input, &output);

	assert(output.previous == NULL);
	assert(output.current == NULL);
	assert(output.next_path == NULL);
}

static void test_sequential_boundaries(void)
{
	struct media_file_data items[] = {file("first.mp4"), file("last.mp4")};
	struct darray files = {.array = items, .num = 2, .capacity = 2};

	assert(mps_previous_sequential_media(&files, &items[0], 0, 0, false) == NULL);
	assert(mps_next_sequential_media(&files, &items[1], 1, 0, false) == NULL);
	assert(mps_next_sequential_media(&files, &items[0], 0, 0, false) == &items[1]);
}

static void test_sequential_loop_wraps(void)
{
	struct media_file_data items[] = {file("first.mp4"), file("last.mp4")};
	struct darray files = {.array = items, .num = 2, .capacity = 2};

	assert(mps_previous_sequential_media(&files, &items[0], 0, 0, true) == &items[1]);
	assert(mps_next_sequential_media(&files, &items[1], 1, 0, true) == &items[0]);
}

static void test_folder_child_is_actual_context(void)
{
	struct media_file_data folder = file("folder");
	struct media_file_data children[] = {file("one.mp4"), file("two.mp4"), file("three.mp4")};
	struct media_file_data items[] = {folder, file("after.mp4")};
	struct darray files = {.array = items, .num = 2, .capacity = 2};
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_context output;

	items[0].is_folder = true;
	items[0].folder_items.array = children;
	items[0].folder_items.num = 3;
	items[0].folder_items.capacity = 3;
	children[0].filename = "one.mp4";
	children[1].filename = "two.mp4";
	children[2].filename = "three.mp4";
	input.files = &files;
	input.current_media = &items[0];
	input.actual_media = &children[1];
	input.current_media_index = 0;
	input.current_folder_item_index = 1;
	input.logical_next_path = children[2].path;
	mps_playlist_context_resolve(&input, &output);

	assert(output.previous == &children[0]);
	assert(output.current == &children[1]);
	assert(strcmp(output.current->path, "two.mp4") == 0);
	assert(strcmp(output.next_path, "three.mp4") == 0);
	assert(mps_next_sequential_media(&files, &items[0], 0, 2, false) == &items[1]);
}

static void test_shuffle_context_does_not_peek_next(void)
{
	struct media_file_data items[] = {file("one.mp4"), file("two.mp4"), file("three.mp4")};
	struct darray files = {.array = items, .num = 3, .capacity = 3};
	struct shuffler shuffler;
	struct mps_playlist_context_input input = {0};
	struct mps_playlist_context output;
	struct media_file_data *expected_previous;
	struct media_file_data *expected_next;
	size_t head;
	size_t next;

	shuffler_init(&shuffler);
	shuffler_update_files(&shuffler, &files);
	shuffler_select(&shuffler, &items[0]);
	assert(shuffler_has_next(&shuffler));
	input.files = &files;
	input.shuffler = &shuffler;
	input.shuffle = true;
	input.current_media = &items[0];
	input.actual_media = &items[0];
	expected_next = shuffler_peek_next(&shuffler);
	input.logical_next_path = expected_next->path;

	/* Resolve reads history only; the cached next value is copied through. */
	head = shuffler.head;
	next = shuffler.next;
	expected_previous = shuffler_has_prev(&shuffler) ? shuffler_peek_prev(&shuffler) : NULL;
	mps_playlist_context_resolve(&input, &output);
	assert(output.previous == expected_previous);
	assert(output.current == &items[0]);
	assert(strcmp(output.next_path, expected_next->path) == 0);
	assert(shuffler.head == head);
	assert(shuffler.next == next);
	mps_playlist_context_resolve(&input, &output);
	assert(shuffler.head == head);
	assert(shuffler.next == next);
	shuffler_destroy(&shuffler);
}

int main(void)
{
	test_empty_playlist();
	test_sequential_boundaries();
	test_sequential_loop_wraps();
	test_folder_child_is_actual_context();
	test_shuffle_context_does_not_peek_next();
	puts("playlist context tests passed");
	return 0;
}
