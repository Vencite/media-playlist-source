/*
 * Benchmark of the production duration probe, run against generated local
 * media files. It starts a minimal headless libobs, loads obs-ffmpeg, and
 * drives one duration inventory from set_paths() to idle, so the measured
 * numbers cover the real probe path (private ffmpeg_source + bounded polling +
 * OBS destruction queue wait) and not just the cache.
 *
 * Usage: duration-probe-benchmark [obs-ffmpeg-plugin] [obs-ffmpeg-data]
 * The defaults target a Linux obs-studio installation; the two arguments or
 * MPS_FFMPEG_PLUGIN / MPS_FFMPEG_DATA override them.
 *
 * The benchmark reports metadata only. It does not show that a probe never
 * starts playback in a running OBS session; that stays a manual test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "duration-inventory.h"

#define CHECK(condition)                                                                                \
	do {                                                                                            \
		if (!(condition)) {                                                                     \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE);                                                             \
		}                                                                                       \
	} while (false)

#define MEDIA_COUNT 3
#define WAIT_TIMEOUT_MS 15000

struct media_expectation {
	char path[512];
	int64_t media_ms;
};

static struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int change_calls;
} benchmark;

static void benchmark_changed(void *opaque)
{
	UNUSED_PARAMETER(opaque);

	pthread_mutex_lock(&benchmark.mutex);
	++benchmark.change_calls;
	pthread_cond_broadcast(&benchmark.cond);
	pthread_mutex_unlock(&benchmark.mutex);
}

static bool write_le16(unsigned char *dst, unsigned int value)
{
	dst[0] = (unsigned char)(value & 0xFF);
	dst[1] = (unsigned char)((value >> 8) & 0xFF);
	return true;
}

static bool write_le32(unsigned char *dst, unsigned int value)
{
	dst[0] = (unsigned char)(value & 0xFF);
	dst[1] = (unsigned char)((value >> 8) & 0xFF);
	dst[2] = (unsigned char)((value >> 16) & 0xFF);
	dst[3] = (unsigned char)((value >> 24) & 0xFF);
	return true;
}

/* Writes 16 bit PCM silence; the WAV demuxer takes the duration from the
 * header, so the sample data only has to be present. */
static bool write_wav(const char *path, unsigned int sample_rate, unsigned int frames)
{
	static const unsigned char silence[4096];
	unsigned char header[44];
	unsigned int data_size = frames * 2;
	unsigned int written = 0;
	FILE *file;

	file = os_fopen(path, "wb");
	if (!file)
		return false;

	memcpy(header, "RIFF", 4);
	write_le32(header + 4, 36 + data_size);
	memcpy(header + 8, "WAVEfmt ", 8);
	write_le32(header + 16, 16);
	write_le16(header + 20, 1);
	write_le16(header + 22, 1);
	write_le32(header + 24, sample_rate);
	write_le32(header + 28, sample_rate * 2);
	write_le16(header + 32, 2);
	write_le16(header + 34, 16);
	memcpy(header + 36, "data", 4);
	write_le32(header + 40, data_size);

	if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
		fclose(file);
		return false;
	}
	while (written < data_size) {
		unsigned int chunk = data_size - written < sizeof(silence) ? data_size - written : sizeof(silence);

		if (fwrite(silence, 1, chunk, file) != chunk) {
			fclose(file);
			return false;
		}
		written += chunk;
	}

	fclose(file);
	return true;
}

static bool write_garbage(const char *path, size_t size)
{
	FILE *file = os_fopen(path, "wb");
	size_t written = 0;

	if (!file)
		return false;
	while (written < size) {
		static const char junk[] = "not a media file\n";
		unsigned int chunk = size - written < sizeof(junk) - 1 ? (unsigned int)(size - written)
								       : (unsigned int)(sizeof(junk) - 1);

		if (fwrite(junk, 1, chunk, file) != chunk) {
			fclose(file);
			return false;
		}
		written += chunk;
	}
	fclose(file);
	return true;
}

static bool load_ffmpeg_plugin(int argc, char **argv)
{
	const char *plugin = argc > 1 ? argv[1] : getenv("MPS_FFMPEG_PLUGIN");
	const char *data = argc > 2 ? argv[2] : getenv("MPS_FFMPEG_DATA");
	obs_module_t *module = NULL;

	if (!plugin || !*plugin)
		plugin = "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-ffmpeg.so";
	if (!data || !*data)
		data = "/usr/share/obs/obs-plugins/obs-ffmpeg";

	if (obs_open_module(&module, plugin, data) != MODULE_SUCCESS) {
		fprintf(stderr, "could not open %s\n", plugin);
		return false;
	}
	if (!obs_init_module(module)) {
		fprintf(stderr, "could not initialize %s\n", plugin);
		return false;
	}
	obs_post_load_modules();
	return true;
}

int main(int argc, char **argv)
{
	struct media_expectation media[MEDIA_COUNT];
	const char *paths[MEDIA_COUNT + 3];
	char directory[512];
	char corrupt_path[512];
	char missing_path[512];
	const char *root;
	struct mps_duration_inventory *inventory;
	int64_t total_media_ms = 0;
	uint64_t start;
	double first_pass_ms;
	double cached_pass_ms;
	int failures = 0;

	root = getenv("TMPDIR");
	if (!root || !*root)
		root = getenv("TEMP");
	if (!root || !*root)
		root = getenv("TMP");
	if (!root || !*root)
		root = ".";

	CHECK(snprintf(directory, sizeof(directory), "%s/mps-duration-benchmark-%llu", root,
		       (unsigned long long)os_gettime_ns()) < (int)sizeof(directory));
	CHECK(os_mkdirs(directory) == 0);

	CHECK(snprintf(media[0].path, sizeof(media[0].path), "%s/short.wav", directory) > 0);
	CHECK(snprintf(media[1].path, sizeof(media[1].path), "%s/medium.wav", directory) > 0);
	CHECK(snprintf(media[2].path, sizeof(media[2].path), "%s/long.wav", directory) > 0);
	CHECK(snprintf(corrupt_path, sizeof(corrupt_path), "%s/corrupt.mp4", directory) > 0);
	CHECK(snprintf(missing_path, sizeof(missing_path), "%s/missing.mp4", directory) > 0);

	/* 8000 Hz 16 bit mono: 800 frames = 100 ms, 8000 = 1000 ms, 20000 = 2500 ms */
	CHECK(write_wav(media[0].path, 8000, 800));
	CHECK(write_wav(media[1].path, 8000, 8000));
	CHECK(write_wav(media[2].path, 8000, 20000));
	media[0].media_ms = 100;
	media[1].media_ms = 1000;
	media[2].media_ms = 2500;
	CHECK(write_garbage(corrupt_path, 64 * 1024));

	pthread_mutex_init(&benchmark.mutex, NULL);
	pthread_cond_init(&benchmark.cond, NULL);

	CHECK(obs_startup("en-US", directory, NULL));
	CHECK(load_ffmpeg_plugin(argc, argv));

	paths[0] = media[0].path;
	paths[1] = media[1].path;
	paths[2] = media[2].path;
	paths[3] = corrupt_path;
	paths[4] = missing_path;
	paths[5] = "https://example.com/media.mp4";

	mps_duration_inventory_start(benchmark_changed, NULL);
	inventory = mps_duration_inventory_create();
	CHECK(inventory != NULL);

	start = os_gettime_ns();
	mps_duration_inventory_set_paths(inventory, paths, MEDIA_COUNT + 3);
	CHECK(mps_duration_inventory_wait_idle(inventory, WAIT_TIMEOUT_MS));
	first_pass_ms = (double)(os_gettime_ns() - start) / 1000000.0;

	printf("probe pass: %.2f ms for %d paths\n", first_pass_ms, MEDIA_COUNT + 3);
	for (size_t i = 0; i < MEDIA_COUNT; i++) {
		int64_t media_ms = 0;
		bool known = mps_duration_inventory_get(inventory, media[i].path, &media_ms);

		total_media_ms += media_ms;
		printf("  %-12s expected %5lld ms, got %5lld ms %s\n", strrchr(media[i].path, '/') + 1,
		       (long long)media[i].media_ms, (long long)media_ms,
		       known && media_ms == media[i].media_ms ? "ok" : "MISMATCH");
		if (!known || media_ms != media[i].media_ms)
			++failures;
	}

	{
		int64_t media_ms = 0;
		bool known = mps_duration_inventory_get(inventory, corrupt_path, &media_ms);

		printf("  %-12s expected unknown, got %s\n", "corrupt.mp4", known ? "duration" : "unknown");
		if (known)
			++failures;
	}
	{
		int64_t media_ms = 0;
		bool known = mps_duration_inventory_get(inventory, missing_path, &media_ms);

		printf("  %-12s expected unknown, got %s\n", "missing.mp4", known ? "duration" : "unknown");
		if (known)
			++failures;
	}
	{
		int64_t media_ms = 0;
		bool known = mps_duration_inventory_get(inventory, paths[5], &media_ms);

		printf("  %-12s expected unknown, got %s\n", "url", known ? "duration" : "unknown");
		if (known)
			++failures;
	}

	start = os_gettime_ns();
	mps_duration_inventory_set_paths(inventory, paths, MEDIA_COUNT + 3);
	CHECK(mps_duration_inventory_wait_idle(inventory, WAIT_TIMEOUT_MS));
	cached_pass_ms = (double)(os_gettime_ns() - start) / 1000000.0;

	printf("cached pass: %.2f ms (identity recheck only)\n", cached_pass_ms);
	printf("total reported duration: %lld ms, changes: %d\n", (long long)total_media_ms, benchmark.change_calls);

	mps_duration_inventory_destroy(inventory);
	mps_duration_inventory_stop();
	obs_shutdown();

	for (size_t i = 0; i < MEDIA_COUNT; i++)
		os_unlink(media[i].path);
	os_unlink(corrupt_path);
	os_rmdir(directory);

	printf("%s\n", failures ? "benchmark FAILED" : "benchmark ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
