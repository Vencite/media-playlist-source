#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <util/bmem.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

struct mps_file_metadata {
	int64_t size;
	int64_t mtime_sec;
	int64_t mtime_nsec;
};

/* Return -1 on failure, 0 for non-files, 1 for a regular file. */
static inline int mps_file_metadata_read(const char *path, struct mps_file_metadata *entry)
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
	entry->mtime_sec = (int64_t)(stamp.QuadPart / UINT64_C(10000000)) - INT64_C(11644473600);
	entry->mtime_nsec = (int64_t)(stamp.QuadPart % UINT64_C(10000000)) * 100;
#else
	struct stat info;
	if (os_stat(path, &info) != 0)
		return -1;
	if (!S_ISREG(info.st_mode))
		return 0;
	entry->size = (int64_t)info.st_size;
	entry->mtime_sec = (int64_t)info.st_mtime;
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
