#include "folder-monitor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

#define MAX_FOLDERS 1024
#define MAX_FOLDER_ENTRIES 16384
#define MAX_SCAN_ENTRIES 65536
#define MAX_PATH_BYTES 4096

struct mps_folder_monitor {
	struct mps_folder_monitor *next;
	struct mps_folder_result *configuration;
	struct mps_folder_result *result;
	uint64_t serial;
	uint64_t next_scan_ns;
	unsigned interval_seconds;
	size_t references;
	bool destroyed;
};

static struct {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_t thread;
	struct mps_folder_monitor *monitors;
	bool started;
	bool shutdown;
} worker = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};

void mps_folder_result_free(struct mps_folder_result *result)
{
	if (!result)
		return;
	for (size_t i = 0; i < result->count; ++i) {
		struct mps_folder_snapshot *folder = &result->folders[i];
		for (size_t j = 0; j < folder->count; ++j)
			bfree(folder->entries[j].name);
		bfree(folder->entries);
		bfree(folder->id);
		bfree(folder->path);
	}
	bfree(result->folders);
	bfree(result);
}

static void release_monitor(struct mps_folder_monitor *monitor)
{
	if (--monitor->references)
		return;
	mps_folder_result_free(monitor->configuration);
	mps_folder_result_free(monitor->result);
	bfree(monitor);
}

struct mps_folder_monitor *mps_folder_monitor_create(void)
{
	struct mps_folder_monitor *monitor = bzalloc(sizeof(*monitor));
	monitor->references = 1;
	pthread_mutex_lock(&worker.mutex);
	monitor->next = worker.monitors;
	worker.monitors = monitor;
	pthread_mutex_unlock(&worker.mutex);
	return monitor;
}

void mps_folder_monitor_destroy(struct mps_folder_monitor *monitor)
{
	if (!monitor)
		return;
	pthread_mutex_lock(&worker.mutex);
	struct mps_folder_monitor **link = &worker.monitors;
	while (*link && *link != monitor)
		link = &(*link)->next;
	if (*link)
		*link = monitor->next;
	monitor->destroyed = true;
	++monitor->serial;
	release_monitor(monitor);
	pthread_cond_broadcast(&worker.condition);
	pthread_mutex_unlock(&worker.mutex);
}

static struct mps_folder_result *copy_configuration(const struct mps_folder_result *configuration)
{
	struct mps_folder_result *copy = bzalloc(sizeof(*copy));
	copy->generation = configuration->generation;
	copy->count = configuration->count;
	copy->folders = copy->count ? bzalloc(copy->count * sizeof(*copy->folders)) : NULL;
	for (size_t i = 0; i < copy->count; ++i) {
		copy->folders[i].id = bstrdup(configuration->folders[i].id);
		copy->folders[i].path = bstrdup(configuration->folders[i].path);
	}
	return copy;
}

static bool cancelled(struct mps_folder_monitor *monitor, uint64_t serial)
{
	pthread_mutex_lock(&worker.mutex);
	bool stop = worker.shutdown || monitor->destroyed || monitor->serial != serial;
	pthread_mutex_unlock(&worker.mutex);
	return stop;
}

static bool supported(const char *name)
{
	const char *extension = os_get_path_extension(name);
	if (!extension || !*extension)
		return false;
	struct dstr needle = {0};
	dstr_cat_ch(&needle, '*');
	dstr_cat(&needle, extension);
	/* Match the existing folder extension filter, including case handling. */
	struct dstr filters = {0};
	dstr_copy(
		&filters,
		"*.mp4 *.mpg *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm *.mp3 *.m4a *.ogg *.aac *.wav *.opus *.flac *.mka");
	bool valid = dstr_find_i(&filters, needle.array) != NULL;
	dstr_free(&needle);
	dstr_free(&filters);
	return valid;
}

/* Return -1 on failure, 0 for non-files, 1 for a regular file. */
static int file_metadata(const char *path, struct mps_folder_entry *entry)
{
#ifdef _WIN32
	wchar_t *wide = NULL;
	if (!os_utf8_to_wcs_ptr(path, 0, &wide))
		return -1;
	/* Follow links and allow concurrent copying, playback, deletion and renames. */
	HANDLE file = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				  FILE_FLAG_BACKUP_SEMANTICS, NULL);
	bfree(wide);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	BY_HANDLE_FILE_INFORMATION info;
	bool success = GetFileInformationByHandle(file, &info) != 0;
	CloseHandle(file);
	if (!success)
		return -1;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return 0;
	ULARGE_INTEGER size = {.LowPart = info.nFileSizeLow, .HighPart = info.nFileSizeHigh};
	ULARGE_INTEGER stamp = {.LowPart = info.ftLastWriteTime.dwLowDateTime,
				.HighPart = info.ftLastWriteTime.dwHighDateTime};
	if (size.QuadPart > INT64_MAX)
		return -1;
	entry->size = (int64_t)size.QuadPart;
	/* FILETIME counts 100 ns intervals from 1601; keep the Unix epoch used by stat. */
	entry->mtime = (int64_t)(stamp.QuadPart / UINT64_C(10000000)) - INT64_C(11644473600);
	entry->mtime_nsec = (int64_t)(stamp.QuadPart % UINT64_C(10000000)) * 100;
#else
	struct stat info;
	if (os_stat(path, &info) != 0)
		return -1;
	if (!S_ISREG(info.st_mode))
		return 0;
	entry->size = (int64_t)info.st_size;
	entry->mtime = (int64_t)info.st_mtime;
#if defined(__APPLE__)
	entry->mtime_nsec = info.st_mtimespec.tv_nsec;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
	entry->mtime_nsec = info.st_mtim.tv_nsec;
#else
	/* shortcut: strict ISO stat hides subsecond fields; enable POSIX fields if needed. */
	entry->mtime_nsec = 0;
#endif
#endif
	return 1;
}

static int compare_entries(const void *left, const void *right)
{
	return strcmp(((const struct mps_folder_entry *)left)->name, ((const struct mps_folder_entry *)right)->name);
}

static void scan_folder(struct mps_folder_monitor *monitor, uint64_t serial, struct mps_folder_snapshot *folder,
			size_t *examined)
{
	os_dir_t *dir = os_opendir(folder->path);
	if (!dir)
		return;
	DARRAY(struct mps_folder_entry) entries;
	da_init(entries);
	bool success = true;
	size_t seen = 0;
	for (;;) {
		if (cancelled(monitor, serial)) {
			success = false;
			break;
		}
		errno = 0;
#ifdef _WIN32
		SetLastError(ERROR_SUCCESS);
#endif
		struct os_dirent *ent = os_readdir(dir);
		if (!ent) {
#ifdef _WIN32
			if (GetLastError() != ERROR_NO_MORE_FILES)
#else
			if (errno)
#endif
				success = false;
			break;
		}
		if (++seen > MAX_FOLDER_ENTRIES || ++*examined > MAX_SCAN_ENTRIES) {
			success = false;
			break;
		}
		if (ent->directory || !supported(ent->d_name))
			continue;
		struct dstr path = {0};
		dstr_copy(&path, folder->path);
		dstr_cat_ch(&path, '/');
		dstr_cat(&path, ent->d_name);
		struct mps_folder_entry entry = {0};
		int metadata = path.len < MAX_PATH_BYTES ? file_metadata(path.array, &entry) : -1;
		dstr_free(&path);
		if (metadata < 0) {
			success = false;
			break;
		}
		if (!metadata)
			continue;
		entry.name = bstrdup(ent->d_name);
		da_push_back(entries, &entry);
	}
	os_closedir(dir);
	if (success) {
		if (entries.num > 1)
			qsort(entries.array, entries.num, sizeof(*entries.array), compare_entries);
		folder->entries = entries.array;
		folder->count = entries.num;
		folder->success = true;
	} else {
		for (size_t i = 0; i < entries.num; ++i)
			bfree(entries.array[i].name);
		da_free(entries);
	}
}

static void wait_until(uint64_t next_ns)
{
	uint64_t now = os_gettime_ns();
	uint64_t delta = next_ns > now ? next_ns - now : 0;
	struct timespec deadline;
	timespec_get(&deadline, TIME_UTC);
	deadline.tv_sec += (time_t)(delta / UINT64_C(1000000000));
	deadline.tv_nsec += (long)(delta % UINT64_C(1000000000));
	if (deadline.tv_nsec >= 1000000000L) {
		++deadline.tv_sec;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&worker.condition, &worker.mutex, &deadline);
}

static void *monitor_thread(void *unused)
{
	(void)unused;
	os_set_thread_name("mps-folder-monitor");
	pthread_mutex_lock(&worker.mutex);
	while (!worker.shutdown) {
		uint64_t now = os_gettime_ns();
		uint64_t next_ns = UINT64_MAX;
		struct mps_folder_monitor *monitor = NULL;
		for (struct mps_folder_monitor *item = worker.monitors; item; item = item->next) {
			if (!item->configuration || !item->configuration->count)
				continue;
			if (item->next_scan_ns <= now) {
				monitor = item;
				break;
			}
			if (item->next_scan_ns < next_ns)
				next_ns = item->next_scan_ns;
		}
		if (!monitor) {
			if (next_ns == UINT64_MAX)
				pthread_cond_wait(&worker.condition, &worker.mutex);
			else
				wait_until(next_ns);
			continue;
		}
		struct mps_folder_result *result = copy_configuration(monitor->configuration);
		uint64_t serial = monitor->serial;
		++monitor->references;
		monitor->next_scan_ns = now + (uint64_t)monitor->interval_seconds * UINT64_C(1000000000);
		pthread_mutex_unlock(&worker.mutex);
		size_t examined = 0;
		for (size_t i = 0; i < result->count && !cancelled(monitor, serial); ++i)
			scan_folder(monitor, serial, &result->folders[i], &examined);
		pthread_mutex_lock(&worker.mutex);
		if (!worker.shutdown && !monitor->destroyed && monitor->serial == serial) {
			mps_folder_result_free(monitor->result);
			monitor->result = result;
			monitor->next_scan_ns =
				os_gettime_ns() + (uint64_t)monitor->interval_seconds * UINT64_C(1000000000);
		} else {
			mps_folder_result_free(result);
		}
		release_monitor(monitor);
	}
	pthread_mutex_unlock(&worker.mutex);
	return NULL;
}

void mps_folder_monitor_configure(struct mps_folder_monitor *monitor, uint64_t generation, const char *const *ids,
				  const char *const *paths, size_t count, unsigned interval_seconds)
{
	if (!monitor)
		return;
	struct mps_folder_result configuration = {.generation = generation, .count = count};
	if (count > MAX_FOLDERS)
		configuration.count = 0;
	configuration.folders = configuration.count ? bzalloc(configuration.count * sizeof(*configuration.folders))
						    : NULL;
	for (size_t i = 0; i < configuration.count; ++i) {
		configuration.folders[i].id = (char *)ids[i];
		configuration.folders[i].path = (char *)paths[i];
	}
	struct mps_folder_result *copy = copy_configuration(&configuration);
	bfree(configuration.folders);
	pthread_mutex_lock(&worker.mutex);
	mps_folder_result_free(monitor->configuration);
	mps_folder_result_free(monitor->result);
	monitor->configuration = copy;
	monitor->result = NULL;
	monitor->interval_seconds = interval_seconds ? interval_seconds : 5;
	monitor->next_scan_ns = 0;
	++monitor->serial;
	if (copy->count && !worker.shutdown && !worker.started) {
		worker.started = pthread_create(&worker.thread, NULL, monitor_thread, NULL) == 0;
	}
	pthread_cond_broadcast(&worker.condition);
	pthread_mutex_unlock(&worker.mutex);
}

struct mps_folder_result *mps_folder_monitor_take(struct mps_folder_monitor *monitor)
{
	if (!monitor)
		return NULL;
	pthread_mutex_lock(&worker.mutex);
	struct mps_folder_result *result = monitor->result;
	monitor->result = NULL;
	pthread_mutex_unlock(&worker.mutex);
	return result;
}

void mps_folder_monitor_shutdown(void)
{
	pthread_mutex_lock(&worker.mutex);
	worker.shutdown = true;
	bool join = worker.started;
	worker.started = false;
	pthread_cond_broadcast(&worker.condition);
	pthread_mutex_unlock(&worker.mutex);
	if (join)
		pthread_join(worker.thread, NULL);
}
