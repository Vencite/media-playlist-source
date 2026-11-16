#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "../src/folder-monitor.c"

static void touch_file(const char *dir, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	int fd = open(path, O_CREAT | O_WRONLY, 0600);
	assert(fd >= 0);
	assert(write(fd, "123", 3) == 3);
	close(fd);
}

int main(void)
{
	char dir[] = "/tmp/mps-folder-scan-XXXXXX";
	assert(mkdtemp(dir));
	touch_file(dir, "b.mp4");
	touch_file(dir, "A.MOV");
	touch_file(dir, "ignored.txt");
	struct mps_folder_monitor m = {0};
	struct mps_folder_result *r = bzalloc(sizeof(*r));
	r->count = 1;
	r->folders = bzalloc(sizeof(*r->folders));
	r->folders[0].path = bstrdup(dir);
	r->folders[0].id = bstrdup("folder-id");
	size_t examined = 0;
	scan_folder(&m, 0, r->folders, &examined);
	assert(r->folders[0].success);
	assert(r->folders[0].count == 2);
	assert(strcmp(r->folders[0].entries[0].name, "A.MOV") == 0);
	assert(strcmp(r->folders[0].entries[1].name, "b.mp4") == 0);
	assert(r->folders[0].entries[0].size == 3);
	mps_folder_result_free(r);
	r = bzalloc(sizeof(*r));
	r->count = 1;
	r->folders = bzalloc(sizeof(*r->folders));
	r->folders[0].path = bstrdup("/tmp/missing-mps-folder-12345");
	scan_folder(&m, 0, r->folders, &examined);
	assert(!r->folders[0].success);
	mps_folder_result_free(r);
	const char *names[] = {"b.mp4", "A.MOV", "ignored.txt"};
	char path[512];
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		assert(unlink(path) == 0);
	}
	r = bzalloc(sizeof(*r));
	r->count = 1;
	r->folders = bzalloc(sizeof(*r->folders));
	r->folders[0].path = bstrdup(dir);
	scan_folder(&m, 0, r->folders, &examined);
	assert(r->folders[0].success);
	assert(r->folders[0].count == 0);
	mps_folder_result_free(r);
	struct mps_folder_monitor *owned = mps_folder_monitor_create();
	const char *ids[] = {"test"};
	const char *paths[] = {dir};
	mps_folder_monitor_configure(owned, 1, ids, paths, 1, 5);
	mps_folder_monitor_configure(owned, 2, NULL, NULL, 0, 5);
	assert(mps_folder_monitor_take(owned) == NULL);
	mps_folder_monitor_destroy(owned);
	mps_folder_monitor_shutdown();
	assert(rmdir(dir) == 0);
	puts("folder scanner self-check OK");
}
