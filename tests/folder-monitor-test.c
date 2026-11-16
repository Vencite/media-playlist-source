#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <util/platform.h>
#include <util/threading.h>

static os_dir_t *gated_opendir(const char *path);
#define os_opendir gated_opendir
#include "../src/folder-monitor.c"
#undef os_opendir
#include "../src/folder-reconcile.c"
#include "../src/duration-inventory.c"

static struct {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	char *blocked_path;
	char *queued_path;
	bool entered;
	bool released;
	bool returned;
	bool queued_entered;
} filesystem_gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};

static os_dir_t *gated_opendir(const char *path)
{
	pthread_mutex_lock(&filesystem_gate.mutex);
	if (filesystem_gate.blocked_path && strcmp(path, filesystem_gate.blocked_path) == 0) {
		filesystem_gate.entered = true;
		pthread_cond_broadcast(&filesystem_gate.condition);
		while (!filesystem_gate.released)
			pthread_cond_wait(&filesystem_gate.condition, &filesystem_gate.mutex);
		filesystem_gate.returned = true;
	}
	if (filesystem_gate.queued_path && strcmp(path, filesystem_gate.queued_path) == 0)
		filesystem_gate.queued_entered = true;
	pthread_mutex_unlock(&filesystem_gate.mutex);
	return os_opendir(path);
}

/* A controlled blocked call characterizes worker scheduling, not real SMB timeouts. */
static struct mps_folder_monitor *test_blocked_filesystem(const char *directory)
{
	struct dstr blocked_path = {0}, queued_path = {0};
	dstr_printf(&blocked_path, "%s/.", directory);
	dstr_printf(&queued_path, "%s/./.", directory);
	pthread_mutex_lock(&filesystem_gate.mutex);
	filesystem_gate.blocked_path = blocked_path.array;
	filesystem_gate.queued_path = queued_path.array;
	pthread_mutex_unlock(&filesystem_gate.mutex);
	struct mps_folder_monitor *blocked = mps_folder_monitor_create();
	struct mps_folder_monitor *queued = mps_folder_monitor_create();
	const char *ids[] = {"blocked-source"};
	const char *paths[] = {blocked_path.array};
	mps_folder_monitor_configure(blocked, 1, ids, paths, 1, 5);
	pthread_mutex_lock(&filesystem_gate.mutex);
	while (!filesystem_gate.entered)
		pthread_cond_wait(&filesystem_gate.condition, &filesystem_gate.mutex);
	pthread_mutex_unlock(&filesystem_gate.mutex);

	ids[0] = "queued-source";
	paths[0] = queued_path.array;
	mps_folder_monitor_configure(queued, 1, ids, paths, 1, 5);
	assert(mps_folder_monitor_take(queued) == NULL);
	pthread_mutex_lock(&filesystem_gate.mutex);
	assert(!filesystem_gate.queued_entered);
	pthread_mutex_unlock(&filesystem_gate.mutex);

	/* This returns while the worker still holds its reference inside opendir. */
	mps_folder_monitor_destroy(blocked);
	pthread_mutex_lock(&filesystem_gate.mutex);
	assert(filesystem_gate.entered && !filesystem_gate.returned && !filesystem_gate.released);
	filesystem_gate.released = true;
	pthread_cond_broadcast(&filesystem_gate.condition);
	pthread_mutex_unlock(&filesystem_gate.mutex);
	return queued;
}

static void finish_blocked_filesystem_test(struct mps_folder_monitor *queued)
{
	/* The caller joined the shared worker after releasing the controlled syscall. */
	assert(filesystem_gate.returned);
	mps_folder_monitor_destroy(queued);
	bfree(filesystem_gate.blocked_path);
	bfree(filesystem_gate.queued_path);
	filesystem_gate.blocked_path = NULL;
	filesystem_gate.queued_path = NULL;
	pthread_cond_destroy(&filesystem_gate.condition);
	pthread_mutex_destroy(&filesystem_gate.mutex);
}

static char *make_test_directory(void)
{
#ifdef _WIN32
	wchar_t temporary[MAX_PATH], directory[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	assert(length > 0 && length < MAX_PATH);
	assert(GetTempFileNameW(temporary, L"mps", 0, directory));
	assert(DeleteFileW(directory));
	assert(CreateDirectoryW(directory, NULL));
	char *path = NULL;
	assert(os_wcs_to_utf8_ptr(directory, 0, &path));
	return path;
#else
	char directory[] = "/tmp/mps-folder-scan-XXXXXX";
	assert(mkdtemp(directory));
	return bstrdup(directory);
#endif
}

static void remove_test_directory(const char *path)
{
#ifdef _WIN32
	wchar_t *wide = NULL;
	assert(os_utf8_to_wcs_ptr(path, 0, &wide));
	assert(RemoveDirectoryW(wide));
	bfree(wide);
#else
	assert(rmdir(path) == 0);
#endif
}

static void write_file(const char *dir, const char *name, const char *contents)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *file = os_fopen(path, "wb");
	assert(file);
	assert(fwrite(contents, 1, 3, file) == 3);
	assert(fclose(file) == 0);
}

static void set_mtime(const char *dir, const char *name, long nanoseconds)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
#ifdef _WIN32
	wchar_t *wide = NULL;
	assert(os_utf8_to_wcs_ptr(path, 0, &wide));
	HANDLE file = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bfree(wide);
	assert(file != INVALID_HANDLE_VALUE);
	ULARGE_INTEGER ticks;
	ticks.QuadPart = (UINT64_C(1700000000) + UINT64_C(11644473600)) * UINT64_C(10000000) + nanoseconds / 100;
	FILETIME stamp = {.dwLowDateTime = ticks.LowPart, .dwHighDateTime = ticks.HighPart};
	assert(SetFileTime(file, NULL, NULL, &stamp));
	assert(CloseHandle(file));
#else
	struct timespec stamp[] = {{.tv_sec = 1700000000, .tv_nsec = nanoseconds},
				   {.tv_sec = 1700000000, .tv_nsec = nanoseconds}};
	assert(utimensat(AT_FDCWD, path, stamp, 0) == 0);
#endif
}

static void test_same_size_overwrite(const char *directory)
{
	set_mtime(directory, "A.MOV", 100000100);
	set_mtime(directory, "b.mp4", 200000200);
	char path[512];
	snprintf(path, sizeof(path), "%s/A.MOV", directory);
	struct mps_duration_file_id first_id, second_id;
	assert(duration_identify(NULL, path, &first_id));
	struct mps_folder_monitor monitor = {0};
	struct mps_folder_snapshot first = {.path = bstrdup(directory), .id = bstrdup("folder-id")};
	size_t examined = 0;
	scan_folder(&monitor, 0, &first, &examined);
	assert(first.success && first.count == 2);
	assert(first.entries[0].mtime == 1700000000);
	assert(first.entries[0].mtime_nsec == 100000100);
	struct media_file_data folder = {.path = (char *)directory, .id = "folder-id"};
	struct darray children;
	assert(mps_folder_reconcile(&folder, &first, NULL, MPS_FOLDER_NEWEST, false, &children));
	assert(strcmp(((struct media_file_data *)children.array)[0].filename, "b.mp4") == 0);
	mps_folder_children_free(&children);

	write_file(directory, "A.MOV", "456");
	set_mtime(directory, "A.MOV", 300000300);
	struct mps_folder_snapshot second = {.path = bstrdup(directory), .id = bstrdup("folder-id")};
	examined = 0;
	scan_folder(&monitor, 0, &second, &examined);
	assert(second.success && second.count == 2);
	assert(second.entries[0].size == first.entries[0].size);
	assert(second.entries[0].mtime == first.entries[0].mtime);
	assert(second.entries[0].mtime_nsec == 300000300);
	assert(!equal_stamp(&first.entries[0], &second.entries[0]));
	assert(duration_identify(NULL, path, &second_id));
	assert(first_id.size == second_id.size);
	assert(first_id.mtime_sec == second_id.mtime_sec);
	assert(first_id.mtime_nsec != second_id.mtime_nsec);
	assert(!duration_id_matches(&first_id, &second_id));
	bfree(first_id.path);
	bfree(second_id.path);
	assert(mps_folder_reconcile(&folder, &second, NULL, MPS_FOLDER_NEWEST, false, &children));
	assert(strcmp(((struct media_file_data *)children.array)[0].filename, "A.MOV") == 0);
	mps_folder_children_free(&children);
	struct mps_folder_result *result = bzalloc(sizeof(*result));
	result->count = 2;
	result->folders = bzalloc(2 * sizeof(*result->folders));
	result->folders[0] = first;
	result->folders[1] = second;
	mps_folder_result_free(result);
}

#ifdef _WIN32
static void test_case_only_rename(const char *directory)
{
	struct mps_folder_monitor monitor = {0};
	struct mps_folder_snapshot first = {.path = bstrdup(directory), .id = bstrdup("case-folder")};
	size_t examined = 0;
	scan_folder(&monitor, 0, &first, &examined);
	assert(first.success && first.count == 2);
	struct media_file_data folder = {.path = (char *)directory, .id = "case-folder"};
	struct darray children;
	assert(mps_folder_reconcile(&folder, &first, NULL, MPS_FOLDER_KEEP, false, &children));
	folder.folder_items.da = children;
	char old_path[512], new_path[512];
	snprintf(old_path, sizeof(old_path), "%s/A.MOV", directory);
	snprintf(new_path, sizeof(new_path), "%s/a.mov", directory);
	assert(os_rename(old_path, new_path) == 0);
	struct mps_folder_snapshot renamed = {.path = bstrdup(directory), .id = bstrdup("case-folder")};
	examined = 0;
	scan_folder(&monitor, 0, &renamed, &examined);
	assert(renamed.success && renamed.count == 2);
	assert(mps_folder_reconcile(&folder, &renamed, &first, MPS_FOLDER_KEEP, true, &children));
	assert(children.num == 1);
	mps_folder_children_free(&folder.folder_items.da);
	folder.folder_items.da = children;
	assert(mps_folder_reconcile(&folder, &renamed, &renamed, MPS_FOLDER_KEEP, true, &children));
	assert(children.num == 2);
	assert(strcmp(((struct media_file_data *)children.array)[1].filename, "a.mov") == 0);
	mps_folder_children_free(&children);
	mps_folder_children_free(&folder.folder_items.da);
	assert(os_rename(new_path, old_path) == 0);
	struct mps_folder_result *result = bzalloc(sizeof(*result));
	result->count = 2;
	result->folders = bzalloc(2 * sizeof(*result->folders));
	result->folders[0] = first;
	result->folders[1] = renamed;
	mps_folder_result_free(result);
}
#endif

int main(void)
{
	char *dir = make_test_directory();
	write_file(dir, "b.mp4", "123");
	write_file(dir, "A.MOV", "123");
	write_file(dir, "ignored.txt", "123");
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
	test_same_size_overwrite(dir);
#ifdef _WIN32
	test_case_only_rename(dir);
#endif
	r = bzalloc(sizeof(*r));
	r->count = 1;
	r->folders = bzalloc(sizeof(*r->folders));
	r->folders[0].path = bstrdup("this-mps-test-directory-does-not-exist");
	scan_folder(&m, 0, r->folders, &examined);
	assert(!r->folders[0].success);
	mps_folder_result_free(r);
	const char *names[] = {"b.mp4", "A.MOV", "ignored.txt"};
	char path[512];
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		assert(os_unlink(path) == 0);
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
	struct mps_folder_monitor *queued = test_blocked_filesystem(dir);
	mps_folder_monitor_shutdown();
	finish_blocked_filesystem_test(queued);
	remove_test_directory(dir);
	bfree(dir);
	puts("folder scanner self-check OK");
}
