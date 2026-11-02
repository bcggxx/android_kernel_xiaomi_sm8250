// SPDX-License-Identifier: GPL-2.0
/* One bounded pread or one mmap touch for a known-byte fixture. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EXTENT (16U * 1024U * 1024U)
#define MAX_REQUEST (1U * 1024U * 1024U)

#define HINT_NONE 0
#define HINT_SEQUENTIAL 1
#define HINT_RANDOM 2

struct residency {
	uint64_t resident;
	uint64_t pages;
};

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s read|mmap FILE OFFSET LENGTH [--dontneed] "
		"[--sequential|--random] [--fd-random (mmap only)]\n",
		program);
}

static const char *hint_name(int hint)
{
	switch (hint) {
	case HINT_SEQUENTIAL:
		return "sequential";
	case HINT_RANDOM:
		return "random";
	default:
		return "none";
	}
}

static const char *hint_api(int hint, int mmap_mode)
{
	if (hint == HINT_NONE)
		return "none";
	return mmap_mode ? "madvise" : "posix_fadvise";
}

static void print_hint(int hint, int mmap_mode, int applied)
{
	const char *status = hint == HINT_NONE ? "not-applicable" :
		(applied ? "applied" : "not-applied");

	printf(" hint=%s hint_api=%s hint_status=%s", hint_name(hint),
	       hint_api(hint, mmap_mode), status);
}

static int advise_read_pattern(int fd, uint64_t offset, uint64_t length,
			       int hint)
{
	int advice = hint == HINT_SEQUENTIAL ? POSIX_FADV_SEQUENTIAL :
		POSIX_FADV_RANDOM;

	return posix_fadvise(fd, (off_t)offset, (off_t)length, advice);
}

static int advise_mapping_pattern(void *mapping, size_t length, int hint)
{
	int advice = hint == HINT_SEQUENTIAL ? MADV_SEQUENTIAL : MADV_RANDOM;

	return madvise(mapping, length, advice);
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text[0] || text[0] == '-')
		return -1;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end)
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static uint64_t available_bytes(uint64_t offset, uint64_t length,
				uint64_t file_size)
{
	uint64_t available;

	if (offset >= file_size)
		return 0;
	available = file_size - offset;
	return length < available ? length : available;
}

/* Count resident pages intersecting [offset, offset + length). */
static int count_resident(int fd, uint64_t offset, uint64_t length,
			  size_t page_size, struct residency *result)
{
	uint64_t aligned_offset, delta, span, rounded, pages;
	unsigned char *vector;
	void *mapping;
	size_t i, map_length;
	int error;

	result->resident = 0;
	result->pages = 0;
	if (!length)
		return 0;

	aligned_offset = offset - offset % page_size;
	delta = offset - aligned_offset;
	span = delta + length;
	if (span > UINT64_MAX - (page_size - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	rounded = (span + page_size - 1) / page_size;
	if (rounded > SIZE_MAX / page_size || rounded > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	pages = rounded;
	map_length = (size_t)pages * page_size;
	vector = calloc((size_t)pages, sizeof(*vector));
	if (!vector)
		return -1;
	mapping = mmap(NULL, map_length, PROT_READ, MAP_SHARED, fd,
		       (off_t)aligned_offset);
	if (mapping == MAP_FAILED) {
		free(vector);
		return -1;
	}
	if (mincore(mapping, map_length, vector)) {
		error = errno;
		munmap(mapping, map_length);
		free(vector);
		errno = error;
		return -1;
	}
	for (i = 0; i < (size_t)pages; i++) {
		if (vector[i] & 1)
			result->resident++;
	}
	result->pages = pages;
	error = munmap(mapping, map_length);
	free(vector);
	if (error)
		return -1;
	return 0;
}

static int advise_drop_fixture(int fd)
{
	return posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static void print_residency(const char *name, const struct residency *value)
{
	printf(" %s=%" PRIu64 "/%" PRIu64, name, value->resident,
	       value->pages);
}

static uint64_t elapsed_nanoseconds(const struct timespec *start,
				    const struct timespec *end)
{
	int64_t seconds = end->tv_sec - start->tv_sec;
	int64_t nanoseconds = end->tv_nsec - start->tv_nsec;

	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000LL;
	}
	return (uint64_t)seconds * 1000000000ULL + (uint64_t)nanoseconds;
}

int main(int argc, char **argv)
{
	const char *mode, *path;
	struct stat status;
	struct residency request_before, request_after;
	struct residency file_before, file_after;
	uint64_t offset, length, file_size, effective_length;
	uint64_t page_offset = 0, mmap_length = 0;
	unsigned char *buffer, *mapping;
	volatile unsigned char *touch;
	unsigned char touched_byte = 0;
	struct timespec started, finished;
	uint64_t elapsed_ns = 0;
	ssize_t count = -1;
	long page_size_value;
	size_t page_size;
	int fd, mmap_mode, dontneed = 0, cache_error = 0;
	int hint = HINT_NONE, hint_applied = 0, dontneed_seen = 0;
	int operation_error = 0, data_ok = 1;
	int fd_random = 0;
	int mincore_request_error, mincore_file_error, mincore_error;
	const char *cold, *data_status = "ok", *operation = "complete";
	const char *final_status = "pass";

	if (argc < 5) {
		usage(argv[0]);
		return 2;
	}
	mode = argv[1];
	path = argv[2];
	if (strcmp(mode, "read") && strcmp(mode, "mmap")) {
		usage(argv[0]);
		return 2;
	}
	mmap_mode = !strcmp(mode, "mmap");
	if (parse_u64(argv[3], &offset) || parse_u64(argv[4], &length) ||
	    !length || length > MAX_REQUEST) {
		fprintf(stderr, "invalid request: require 1..1 MiB length and "
			"unsigned decimal offset/length\n");
		return 2;
	}
	if (offset > MAX_EXTENT || length > MAX_EXTENT - offset) {
		fprintf(stderr, "request range exceeds 16 MiB bound\n");
		return 2;
	}
	for (int i = 5; i < argc; i++) {
		if (!strcmp(argv[i], "--dontneed")) {
			if (dontneed_seen++) {
				fprintf(stderr, "duplicate --dontneed option\n");
				return 2;
			}
			dontneed = 1;
		} else if (!strcmp(argv[i], "--fd-random")) {
			if (!mmap_mode || fd_random++) {
				fprintf(stderr, "--fd-random requires mmap, once\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--sequential") ||
			   !strcmp(argv[i], "--random")) {
			if (hint != HINT_NONE) {
				fprintf(stderr, "--sequential and --random are "
					"mutually exclusive\n");
				return 2;
			}
			hint = !strcmp(argv[i], "--sequential") ?
				HINT_SEQUENTIAL : HINT_RANDOM;
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		fprintf(stderr, "cannot determine system page size\n");
		return 1;
	}
	page_size = (size_t)page_size_value;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (fstat(fd, &status)) {
		perror("fstat");
		close(fd);
		return 1;
	}
	if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uint64_t)status.st_size > MAX_EXTENT) {
		fprintf(stderr, "fixture must be a regular file no larger than 16 MiB\n");
		close(fd);
		return 2;
	}
	file_size = (uint64_t)status.st_size;
	effective_length = available_bytes(offset, length, file_size);
	if (mmap_mode && (offset >= file_size || length > file_size - offset)) {
		fprintf(stderr, "mmap range crosses EOF or starts at EOF\n");
		close(fd);
		return 2;
	}
	if (dontneed) {
		cache_error = advise_drop_fixture(fd);
		if (count_resident(fd, 0, file_size, page_size, &file_before)) {
			perror("mincore before fixture");
			close(fd);
			return 1;
		}
		if (count_resident(fd, offset, effective_length, page_size,
				   &request_before)) {
			perror("mincore before request");
			close(fd);
			return 1;
		}
		if (cache_error || !file_before.pages || file_before.resident ||
		    request_before.resident) {
			cold = cache_error ? "unknown" : "not-confirmed";
			printf("mode=%s request_offset=%" PRIu64
			       " request_length=%" PRIu64
			       " cache_prep=dontneed cold=%s operation=skipped",
			       mode, offset, length, cold);
			print_hint(hint, mmap_mode, hint_applied);
			print_residency("mincore_file_before", &file_before);
			print_residency("mincore_request_before", &request_before);
			printf(" mincore_file_after=not-run"
			       " mincore_request_after=not-run status=%s\n",
			       cache_error ? "cache-prep-error" :
			       "not-cold");
			if (cache_error)
				fprintf(stderr, "posix_fadvise(DONTNEED): %s\n",
					strerror(cache_error));
			close(fd);
			return 3;
		}
		cold = "confirmed";
	} else {
		if (count_resident(fd, 0, file_size, page_size, &file_before)) {
			perror("mincore before fixture");
			close(fd);
			return 1;
		}
		if (count_resident(fd, offset, effective_length, page_size,
				   &request_before)) {
			perror("mincore before request");
			close(fd);
			return 1;
		}
		cold = "unclaimed";
	}

	if (fd_random) {
		cache_error = posix_fadvise(fd, (off_t)offset, (off_t)length,
					   POSIX_FADV_RANDOM);
		if (cache_error) {
			fprintf(stderr, "posix_fadvise(random): %s\n",
				strerror(cache_error));
			close(fd);
			return 1;
		}
	}

	if (hint != HINT_NONE && !mmap_mode) {
		cache_error = advise_read_pattern(fd, offset, length, hint);
		if (cache_error) {
			fprintf(stderr, "posix_fadvise(%s): %s\n", hint_name(hint),
				strerror(cache_error));
			close(fd);
			return 1;
		}
		hint_applied = 1;
	}

	if (mmap_mode) {
		page_offset = offset - offset % page_size;
		mmap_length = offset - page_offset + length;
		mapping = mmap(NULL, (size_t)mmap_length, PROT_READ, MAP_PRIVATE,
			       fd, (off_t)page_offset);
		if (mapping == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return 1;
		}
		if (hint != HINT_NONE) {
			if (advise_mapping_pattern(mapping, (size_t)mmap_length,
					   hint)) {
				perror("madvise");
				munmap(mapping, (size_t)mmap_length);
				close(fd);
				return 1;
			}
			hint_applied = 1;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &started)) {
			perror("clock_gettime before mmap touch");
			munmap(mapping, (size_t)mmap_length);
			close(fd);
			return 1;
		}
		touch = &mapping[offset - page_offset];
		touched_byte = *touch;
		if (clock_gettime(CLOCK_MONOTONIC, &finished)) {
			perror("clock_gettime after mmap touch");
			munmap(mapping, (size_t)mmap_length);
			close(fd);
			return 1;
		}
		elapsed_ns = elapsed_nanoseconds(&started, &finished);
		if (touched_byte != (unsigned char)offset) {
			data_ok = 0;
			data_status = "bad";
			final_status = "data-mismatch";
		}
		if (munmap(mapping, (size_t)mmap_length)) {
			perror("munmap workload mapping");
			operation_error = 1;
			final_status = "munmap-error";
		}
	} else {
		buffer = malloc((size_t)length);
		if (!buffer) {
			perror("malloc");
			close(fd);
			return 1;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &started)) {
			perror("clock_gettime before pread");
			free(buffer);
			close(fd);
			return 1;
		}
		do {
			count = pread(fd, buffer, (size_t)length, (off_t)offset);
		} while (count < 0 && errno == EINTR);
		if (clock_gettime(CLOCK_MONOTONIC, &finished)) {
			perror("clock_gettime after pread");
			free(buffer);
			close(fd);
			return 1;
		}
		elapsed_ns = elapsed_nanoseconds(&started, &finished);
		if (count < 0) {
			perror("pread");
			free(buffer);
			close(fd);
			return 1;
		}
		if ((uint64_t)count != effective_length) {
			operation_error = 1;
			data_status = "short";
			final_status = "short-read";
		}
		for (uint64_t i = 0; i < (uint64_t)count; i++) {
			if (buffer[i] != (unsigned char)(offset + i)) {
				data_ok = 0;
				data_status = "bad";
				final_status = "data-mismatch";
				break;
			}
		}
		free(buffer);
	}

	mincore_request_error = count_resident(fd, offset, effective_length,
					      page_size, &request_after);
	if (mincore_request_error)
		perror("mincore after request");
	mincore_file_error = count_resident(fd, 0, file_size, page_size,
					   &file_after);
	if (mincore_file_error)
		perror("mincore after fixture");
	mincore_error = mincore_request_error || mincore_file_error;
	if (mincore_error) {
		final_status = "mincore-error";
	}
	if (close(fd) && !operation_error) {
		perror("close");
		operation_error = 1;
		final_status = "close-error";
	}

	printf("mode=%s file_size=%" PRIu64 " request_offset=%" PRIu64
	       " request_length=%" PRIu64 " elapsed_ns=%" PRIu64
	       " cache_prep=%s cold=%s",
	       mode, file_size, offset, length, elapsed_ns,
	       dontneed ? "dontneed" : "none", cold);
	print_hint(hint, mmap_mode, hint_applied);
	printf(" fd_random=%s", fd_random ? "applied" : "none");
	if (mmap_mode) {
		printf(" mmap_offset=%" PRIu64 " mmap_length=%" PRIu64
		       " touch_offset=%" PRIu64 " touched_byte=%u data=%s",
		       page_offset, mmap_length, offset, touched_byte,
		       data_ok ? "ok" : "bad");
	} else {
		printf(" pread_offset=%" PRIu64 " pread_length=%" PRIu64
		       " pread_return=%" PRId64 " data=%s",
		       offset, length, (int64_t)count, data_status);
	}
	print_residency("mincore_file_before", &file_before);
	if (mincore_file_error)
		printf(" mincore_file_after=error");
	else
		print_residency("mincore_file_after", &file_after);
	print_residency("mincore_request_before", &request_before);
	if (mincore_request_error)
		printf(" mincore_request_after=error");
	else
		print_residency("mincore_request_after", &request_after);
	printf(" operation=%s status=%s\n", operation, final_status);
	return operation_error || !data_ok || mincore_error ||
	       strcmp(final_status, "pass") ? 1 : 0;
}
