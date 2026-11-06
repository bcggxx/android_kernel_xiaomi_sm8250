// SPDX-License-Identifier: GPL-2.0
/* Read-only access to a fixture containing repeated bytes 0..255. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int verify(const unsigned char *data, size_t len, uint64_t offset)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (data[i] != (unsigned char)(offset + i)) {
			fprintf(stderr, "data mismatch at %llu\n",
				(unsigned long long)(offset + i));
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char buffer[8192], *mapping;
	struct stat st;
	uint64_t offset = 0;
	ssize_t count;
	int fd, error, mapped, mixed;

	if (argc != 3 || (strcmp(argv[1], "read") &&
	    strcmp(argv[1], "mmap") && strcmp(argv[1], "mixed") &&
	    strcmp(argv[1], "sequential") && strcmp(argv[1], "random"))) {
		fprintf(stderr, "usage: %s read|mmap|mixed|sequential|random FILE\n",
			argv[0]);
		return 2;
	}
	fd = open(argv[2], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (fstat(fd, &st)) {
		perror("fstat");
		close(fd);
		return 1;
	}
	if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "invalid file size: %s\n", argv[2]);
		close(fd);
		return 1;
	}
	mixed = !strcmp(argv[1], "mixed");
	mapped = mixed || !strcmp(argv[1], "mmap");
	if (!strcmp(argv[1], "sequential") || !strcmp(argv[1], "random")) {
		error = posix_fadvise(fd, 0, 0, !strcmp(argv[1], "random") ?
				      POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
		if (error) {
			fprintf(stderr, "fadvise: %s\n", strerror(error));
			return 1;
		}
	}
	if (!mapped || mixed) {
		do {
			count = read(fd, buffer, sizeof(buffer));
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0 || verify(buffer, count, offset))
				return 1;
			offset += count;
			if (mixed)
				break;
		} while (count);
		if (!mixed && offset != (uint64_t)st.st_size)
			return 1;
	}
	if (mapped && st.st_size) {
		mapping = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (mapping == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		if (verify(mapping, st.st_size, 0) || munmap(mapping, st.st_size))
			return 1;
	}
	if (close(fd))
		return 1;
	printf("PASS mode=%s bytes=%llu\n", argv[1],
	       (unsigned long long)st.st_size);
	return 0;
}
