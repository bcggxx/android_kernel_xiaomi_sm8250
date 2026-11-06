/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "dlp_profile_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <linux/launch_profile.h>

#include "dlp_sha256.h"

#define SNAPSHOT_HEADER_SIZE 32U
#define SNAPSHOT_RECORD_SIZE 24U
#define DLP_SNAPSHOT_RANGES 4096U
#define SNAPSHOT_MAX_SIZE (SNAPSHOT_HEADER_SIZE + DLP_SNAPSHOT_RANGES * SNAPSHOT_RECORD_SIZE)
#define MOUNT_MAX_ENTRIES 512U

static int enforce_quota(const char *root, uint64_t add_size,
			 const char *replace_package, uint32_t replace_user);

struct snapshot_range {
	uint32_t index;
	uint64_t offset;
	uint64_t length;
};

struct output_range {
	uint64_t offset;
	uint64_t length;
};

struct output_file {
	struct dlp_profile_file identity;
	char *path;
	struct output_range *ranges;
};

struct mount_entry {
	uint32_t id;
	char mountpoint[PATH_MAX];
	int read_only;
};

struct mount_table {
	struct mount_entry entries[MOUNT_MAX_ENTRIES];
	size_t count;
};

struct quota_entry {
	char user[16];
	char package[DLP_MAX_PACKAGE + 1];
	uint64_t size;
	int64_t mtime;
	uint32_t mtime_nsec;
};

struct replay_file {
	struct dlp_profile_file identity;
	char *path;
	uint32_t range_start;
};

struct replay_profile {
	struct dlp_profile_header header;
	char package[DLP_MAX_PACKAGE + 1];
	uint8_t signers[DLP_MAX_SIGNERS][32];
	struct replay_file files[DLP_MAX_PATHS];
	struct dlp_profile_range ranges[DLP_SNAPSHOT_RANGES];
	uint32_t file_count;
	uint32_t range_count;
};

static void put_u16(uint8_t **cursor, uint16_t value)
{
	(*cursor)[0] = (uint8_t)value;
	(*cursor)[1] = (uint8_t)(value >> 8);
	*cursor += 2;
}

static void put_u32(uint8_t **cursor, uint32_t value)
{
	for (unsigned int i = 0; i < 4; i++)
		(*cursor)[i] = (uint8_t)(value >> (i * 8));
	*cursor += 4;
}

static void put_u64(uint8_t **cursor, uint64_t value)
{
	for (unsigned int i = 0; i < 8; i++)
		(*cursor)[i] = (uint8_t)(value >> (i * 8));
	*cursor += 8;
}

static int checked_add(size_t *value, size_t add)
{
	if (*value > SIZE_MAX - add)
		return -1;
	*value += add;
	return 0;
}

static int checked_mul(size_t value, size_t count, size_t *result)
{
	if (value && count > SIZE_MAX / value)
		return -1;
	*result = value * count;
	return 0;
}

static int valid_name(const char *name, size_t length)
{
	if (!length || length > DLP_MAX_PACKAGE)
		return 0;
	if ((length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	for (size_t i = 0; i < length; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static int valid_user_name(const char *name, size_t length)
{
	char *end;
	unsigned long value;

	if (!length || length > 10)
		return 0;
	for (size_t i = 0; i < length; i++)
		if (name[i] < '0' || name[i] > '9')
			return 0;
	value = strtoul(name, &end, 10);
	return *end == '\0' && value <= UINT32_MAX;
}

static int valid_path(const char *path, size_t length)
{
	if (!length || length >= DLP_MAX_PATH || path[0] != '/')
		return 0;
	if (memchr(path, '\0', length))
		return 0;
	for (size_t i = 0; i < length; i++) {
		if ((unsigned char)path[i] < 0x20)
			return 0;
		if (path[i] == '/' && i + 1 < length && path[i + 1] == '/')
			return 0;
	}
	if (length && path[length - 1] == '/')
		return 0;
	return 1;
}

static int is_static_readonly_path(const char *path)
{
	static const char *const prefixes[] = {
		"/system/", "/system_ext/", "/vendor/", "/product/",
		"/odm/", "/apex/",
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if (!strncmp(path, prefixes[i], strlen(prefixes[i])))
			return 1;
	return 0;
}

int dlp_metadata_generation(const struct dlp_metadata *metadata,
			    uint64_t *generation)
{
	struct dlp_sha256_ctx hash;
	uint8_t digest[32], encoded[8];
	uint8_t *cursor;
	uint64_t value = 0;

	if (!metadata || !generation || !valid_name(metadata->package,
			metadata->package_len) || metadata->signer_count > DLP_MAX_SIGNERS ||
	    metadata->path_count > DLP_MAX_PATHS)
		return -1;
	dlp_sha256_init(&hash);
	cursor = encoded;
	put_u32(&cursor, metadata->user_id);
	dlp_sha256_update(&hash, encoded, 4);
	cursor = encoded;
	put_u32(&cursor, metadata->uid);
	dlp_sha256_update(&hash, encoded, 4);
	cursor = encoded;
	put_u64(&cursor, metadata->long_version_code);
	dlp_sha256_update(&hash, encoded, 8);
	cursor = encoded;
	put_u16(&cursor, metadata->package_len);
	dlp_sha256_update(&hash, encoded, 2);
	dlp_sha256_update(&hash, metadata->package, metadata->package_len);
	encoded[0] = (uint8_t)metadata->signer_count;
	dlp_sha256_update(&hash, encoded, 1);
	for (uint32_t i = 0; i < metadata->signer_count; i++)
		dlp_sha256_update(&hash, metadata->signers[i], 32);
	cursor = encoded;
	put_u16(&cursor, (uint16_t)metadata->path_count);
	dlp_sha256_update(&hash, encoded, 2);
	for (uint32_t i = 0; i < metadata->path_count; i++) {
		const struct dlp_metadata_path *path = &metadata->paths[i];

		if (!path->length || path->length >= DLP_MAX_PATH ||
		    !valid_path(path->path, path->length))
			return -1;
		encoded[0] = path->kind;
		dlp_sha256_update(&hash, encoded, 1);
		cursor = encoded;
		put_u16(&cursor, path->length);
		dlp_sha256_update(&hash, encoded, 2);
		dlp_sha256_update(&hash, path->path, path->length);
	}
	dlp_sha256_final(&hash, digest);
	for (unsigned int i = 0; i < 8; i++)
		value = (value << 8) | digest[i];
	*generation = value ? value : 1;
	return 0;
}

static const struct dlp_metadata_path *allowed_path(
	const struct dlp_metadata *metadata, const char *path)
{
	const struct dlp_metadata_path *best = NULL;
	size_t best_length = 0;

	for (uint32_t i = 0; i < metadata->path_count; i++) {
		const struct dlp_metadata_path *entry = &metadata->paths[i];

		if (entry->kind == DLP_PATH_BASE_APK ||
		    entry->kind == DLP_PATH_SPLIT_APK ||
		    entry->kind == DLP_PATH_SHARED_FILE) {
			if (entry->length == strlen(path) &&
			    !memcmp(entry->path, path, entry->length) &&
			    entry->length > best_length) {
				best = entry;
				best_length = entry->length;
			}
		} else if (entry->kind == DLP_PATH_NATIVE_DIR ||
			   entry->kind == DLP_PATH_RESOURCE_DIR) {
			size_t length = strlen(path);

			if (length > entry->length &&
			    !strncmp(entry->path, path, entry->length) &&
			    path[entry->length] == '/' && entry->length > best_length) {
				best = entry;
				best_length = entry->length;
			}
		}
	}
	return best;
}

static int same_file_identity(const struct stat *left, const struct stat *right)
{
	return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
		left->st_size == right->st_size &&
		left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
		left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
		left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
		left->st_ctim.tv_nsec == right->st_ctim.tv_nsec &&
		left->st_mode == right->st_mode && left->st_uid == right->st_uid;
}

static int trusted_metadata_file(const struct stat *st, uint32_t app_uid,
				 const char *path)
{
	if (!S_ISREG(st->st_mode) || st->st_size < 0 ||
	    (st->st_mode & (S_IWGRP | S_IWOTH)))
		return 0;
	if (st->st_uid == app_uid && !is_static_readonly_path(path))
		return 0;
	return 1;
}

static int trusted_directory(const struct stat *st, uint32_t app_uid,
			     const char *path)
{
	if (!S_ISDIR(st->st_mode) || (st->st_mode & (S_IWGRP | S_IWOTH)))
		return 0;
	if (st->st_uid == app_uid && !is_static_readonly_path(path))
		return 0;
	return 1;
}

static int safe_open_path(const char *path, uint32_t app_uid, struct stat *result)
{
	char *copy = NULL, *cursor, *slash;
	int current = -1, next;
	size_t length = strlen(path);
	int ret = -1;

	if (!valid_path(path, length))
		goto out;
	copy = strdup(path + 1);
	if (!copy)
		goto out;
	current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (current < 0)
		goto out;
	cursor = copy;
	for (;;) {
		struct stat st;
		int final;

		slash = strchr(cursor, '/');
		if (slash)
			*slash = '\0';
		if (!strcmp(cursor, ".") || !strcmp(cursor, "..") || !*cursor) {
			errno = EINVAL;
			goto out;
		}
		final = slash == NULL;
		next = openat(current, cursor, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
			      (final ? 0 : O_DIRECTORY));
		if (next < 0)
			goto out;
		if (fstat(next, &st)) {
			close(next);
			goto out;
		}
		if (final) {
			if (!trusted_metadata_file(&st, app_uid, path)) {
				close(next);
				errno = EACCES;
				goto out;
			}
		} else if (!trusted_directory(&st, app_uid, path)) {
			close(next);
			errno = EACCES;
			goto out;
		}
		close(current);
		current = next;
		if (final)
			break;
		cursor = slash + 1;
	}
	if (fstat(current, result)) {
		close(current);
		current = -1;
		goto out;
	}
	ret = current;
	current = -1;
out:
	if (current >= 0)
		close(current);
	free(copy);
	return ret;
}

static void unescape_mount_field(char *field)
{
	char *source = field, *target = field;

	while (*source) {
		if (source[0] == '\\' && source[1] >= '0' && source[1] <= '7' &&
		    source[2] >= '0' && source[2] <= '7' &&
		    source[3] >= '0' && source[3] <= '7') {
			*target++ = (char)(((source[1] - '0') << 6) |
					   ((source[2] - '0') << 3) |
					   (source[3] - '0'));
			source += 4;
		} else {
			*target++ = *source++;
		}
	}
	*target = '\0';
}

static int mount_options_contain(const char *options, const char *wanted)
{
	size_t wanted_length = strlen(wanted);

	while (*options) {
		const char *end = strchr(options, ',');
		size_t length = end ? (size_t)(end - options) : strlen(options);

		if (length == wanted_length &&
		    !memcmp(options, wanted, wanted_length))
			return 1;
		if (!end)
			break;
		options = end + 1;
	}
	return 0;
}

static int mount_table_load(struct mount_table *table)
{
	FILE *file;
	char line[8192];

	memset(table, 0, sizeof(*table));
	file = fopen("/proc/self/mountinfo", "re");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		char *save = NULL, *token;
		unsigned long id;
		unsigned int index = 0;
		char *mountpoint = NULL, *options = NULL;

		token = strtok_r(line, " ", &save);
		if (!token)
			continue;
		id = strtoul(token, NULL, 10);
		while ((token = strtok_r(NULL, " ", &save))) {
			index++;
			if (index == 4) {
				mountpoint = token;
			} else if (index == 5) {
				options = token;
				break;
			}
		}
		if (!mountpoint || !options || !id ||
		    table->count == MOUNT_MAX_ENTRIES)
			continue;
		unescape_mount_field(mountpoint);
		table->entries[table->count].id = (uint32_t)id;
		table->entries[table->count].read_only =
			mount_options_contain(options, "ro");
		snprintf(table->entries[table->count].mountpoint,
			 sizeof(table->entries[table->count].mountpoint), "%s",
			 mountpoint);
		table->count++;
	}
	fclose(file);
	return table->count ? 0 : -1;
}

static const struct mount_entry *mount_for_path(const struct mount_table *table,
						const char *path)
{
	const struct mount_entry *best = NULL;
	size_t best_length = 0;

	for (size_t i = 0; i < table->count; i++) {
		const struct mount_entry *entry = &table->entries[i];
		size_t length = strlen(entry->mountpoint);

		if (length < best_length)
			continue;
		if (entry->mountpoint[0] == '/' && !strcmp(entry->mountpoint, "/")) {
			if (!best) {
				best = entry;
				best_length = length;
			}
		} else if (!strncmp(path, entry->mountpoint, length) &&
			   (path[length] == '/' || path[length] == '\0')) {
			best = entry;
			best_length = length;
		}
	}
	return best;
}

static uint32_t mount_id_for_path(const struct mount_table *table, const char *path)
{
	const struct mount_entry *entry = mount_for_path(table, path);

	return entry ? entry->id : 0;
}

static int page_shift_runtime(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	if (page_size <= 0 || (page_size & (page_size - 1)))
		return -1;
	return __builtin_ctzl((unsigned long)page_size);
}

static int read_boot_identity(const char *root, struct dlp_profile_header *header)
{
	struct utsname name;
	int fd;
	ssize_t size;
	char boot_id[38] = {0};
	char path[PATH_MAX];

	if (uname(&name) || strlen(name.release) >= sizeof(header->release) ||
	    strlen(name.version) >= sizeof(header->version_string))
		return -1;
	memset(header->boot_id, 0, sizeof(header->boot_id));
	memset(header->release, 0, sizeof(header->release));
	memset(header->version_string, 0, sizeof(header->version_string));
	snprintf(path, sizeof(path), "%s/%s", root, DLP_BOOT_ID_FILE);
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size = read(fd, boot_id, sizeof(boot_id) - 1);
	close(fd);
	if (size <= 0)
		return -1;
	for (ssize_t i = 0; i < size; i++)
		if (boot_id[i] == '\n')
			boot_id[i] = '\0';
	if (strlen(boot_id) != 36)
		return -1;
	memcpy(header->boot_id, boot_id, strlen(boot_id));
	memcpy(header->release, name.release, strlen(name.release));
	memcpy(header->version_string, name.version, strlen(name.version));
	return 0;
}

static uint32_t crc32_bytes(uint32_t crc, const uint8_t *data, size_t length)
{
	crc = ~crc;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

static int validate_profile_payload(const uint8_t *buffer,
				    const struct dlp_profile_header *header)
{
	const uint8_t *cursor = buffer + sizeof(*header);
	const uint8_t *end = buffer + header->total_size;
	uint64_t file_sizes[DLP_MAX_PATHS];
	uint32_t file_ranges[DLP_MAX_PATHS];
	uint32_t total_ranges = 0;

	if (!header->signer_count ||
	    (header->flags & ~(uint32_t)DLP_PROFILE_SKIPPED) ||
	    (size_t)(end - cursor) < header->package_len ||
	    !valid_name((const char *)cursor, header->package_len))
		return -1;
	cursor += header->package_len;
	if ((size_t)(end - cursor) < header->signer_count * 32U)
		return -1;
	cursor += header->signer_count * 32U;
	for (uint32_t i = 0; i < header->file_count; i++) {
		struct dlp_profile_file file;

		if ((size_t)(end - cursor) < sizeof(file))
			return -1;
		memcpy(&file, cursor, sizeof(file));
		cursor += sizeof(file);
		if (!file.path_len || file.path_len >= DLP_MAX_PATH ||
		    !file.size || !file.mount_id ||
		    (file.flags & ~(uint32_t)DLP_PROFILE_DIGEST_COPY) ||
		    (size_t)(end - cursor) < file.path_len ||
		    memchr(cursor, '\0', file.path_len) || cursor[0] != '/')
			return -1;
		cursor += file.path_len;
		if (file.range_count > DLP_SNAPSHOT_RANGES - total_ranges)
			return -1;
		total_ranges += file.range_count;
		file_sizes[i] = file.size;
		file_ranges[i] = file.range_count;
	}
	if (total_ranges != header->range_count)
		return -1;
	for (uint32_t i = 0; i < header->file_count; i++) {
		uint64_t previous_end = 0;
		uint64_t mask = (1ULL << header->page_shift) - 1;

		for (uint32_t j = 0; j < file_ranges[i]; j++) {
			struct dlp_profile_range range;

			if ((size_t)(end - cursor) < sizeof(range))
				return -1;
			memcpy(&range, cursor, sizeof(range));
			cursor += sizeof(range);
			if (!range.length || (range.offset & mask) ||
			    range.offset > file_sizes[i] ||
			    range.length > file_sizes[i] - range.offset ||
			    (j && range.offset <= previous_end))
				return -1;
			previous_end = range.offset + range.length;
		}
	}
	return cursor == end ? 0 : -1;
}

int dlp_read_profile(const char *path, struct dlp_profile_header *header,
		     uint8_t **owned)
{
	struct stat st;
	uint8_t *buffer = NULL;
	uint32_t stored_crc;
	int fd = -1;
	int ret = -1;
	size_t done = 0;

	if (owned)
		*owned = NULL;
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		goto out;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(*header) ||
	    st.st_size > (off_t)DLP_PROFILE_MAX_SIZE) {
		errno = EINVAL;
		goto out;
	}
	buffer = malloc((size_t)st.st_size);
	if (!buffer)
		goto out;
	while (done < (size_t)st.st_size) {
		ssize_t size = read(fd, buffer + done, (size_t)st.st_size - done);

		if (size < 0 && errno == EINTR)
			continue;
		if (size <= 0) {
			errno = EINVAL;
			goto out;
		}
		done += (size_t)size;
	}
	memcpy(header, buffer, sizeof(*header));
	if (header->magic != DLP_PROFILE_MAGIC ||
	    header->version != DLP_PROFILE_VERSION ||
	    header->header_size != sizeof(*header) ||
	    header->total_size != (uint32_t)st.st_size ||
	    header->signer_count > DLP_MAX_SIGNERS ||
	    header->file_count > DLP_MAX_PATHS ||
	    header->range_count > DLP_SNAPSHOT_RANGES ||
	    header->package_len > DLP_MAX_PACKAGE ||
	    header->page_shift < 12 || header->page_shift > 16 ||
	    header->reserved) {
		errno = EINVAL;
		goto out;
	}
	stored_crc = header->crc32;
	((struct dlp_profile_header *)buffer)->crc32 = 0;
	if (crc32_bytes(0, buffer, done) != stored_crc) {
		errno = EINVAL;
		goto out;
	}
	((struct dlp_profile_header *)buffer)->crc32 = stored_crc;
	if (validate_profile_payload(buffer, header)) {
		errno = EINVAL;
		goto out;
	}
	if (owned) {
		*owned = buffer;
		buffer = NULL;
	}
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return ret;
}

static void replay_profile_clear(struct replay_profile *profile)
{
	for (uint32_t i = 0; i < profile->file_count; i++)
		free(profile->files[i].path);
	profile->file_count = 0;
}

static int replay_profile_parse(const uint8_t *buffer,
				const struct dlp_profile_header *header,
				struct replay_profile *profile)
{
	const uint8_t *cursor = buffer + sizeof(*header);
	const uint8_t *end = buffer + header->total_size;
	uint32_t range_offset = 0;

	memset(profile, 0, sizeof(*profile));
	profile->header = *header;
	memcpy(profile->package, cursor, header->package_len);
	profile->package[header->package_len] = '\0';
	cursor += header->package_len;
	memcpy(profile->signers, cursor, header->signer_count * 32U);
	cursor += header->signer_count * 32U;
	for (uint32_t i = 0; i < header->file_count; i++) {
		struct replay_file *file = &profile->files[i];

		memcpy(&file->identity, cursor, sizeof(file->identity));
		cursor += sizeof(file->identity);
		file->path = malloc(file->identity.path_len + 1U);
		if (!file->path)
			goto fail;
		memcpy(file->path, cursor, file->identity.path_len);
		file->path[file->identity.path_len] = '\0';
		profile->file_count = i + 1;
		cursor += file->identity.path_len;
		file->range_start = range_offset;
		if (file->identity.range_count >
		    DLP_SNAPSHOT_RANGES - range_offset)
			goto fail;
		range_offset += file->identity.range_count;
	}
	if (range_offset != header->range_count ||
	    (size_t)(end - cursor) !=
		    (size_t)range_offset * sizeof(struct dlp_profile_range))
		goto fail;
	memcpy(profile->ranges, cursor,
	       (size_t)range_offset * sizeof(struct dlp_profile_range));
	profile->range_count = range_offset;
	return 0;
fail:
	replay_profile_clear(profile);
	return -1;
}

static int replay_metadata_matches(const struct replay_profile *profile,
				   const struct dlp_metadata *metadata)
{
	const struct dlp_profile_header *header = &profile->header;

	return header->user_id == metadata->user_id &&
		header->uid == metadata->uid &&
		header->metadata_generation == metadata->generation &&
		header->long_version_code == metadata->long_version_code &&
		header->package_len == metadata->package_len &&
		header->signer_count == metadata->signer_count &&
		!memcmp(profile->package, metadata->package, metadata->package_len) &&
		!memcmp(profile->signers, metadata->signers,
			metadata->signer_count * 32U);
}

static int replay_profile_load(const char *root,
			       const struct dlp_metadata *metadata,
			       struct replay_profile *profile)
{
	struct dlp_profile_header current, header;
	uint8_t *buffer = NULL;
	char *path = NULL;
	int ret = -1;

	if (asprintf(&path, "%s/%u/%.*s/%s", root, metadata->user_id,
		     metadata->package_len, metadata->package,
		     DLP_PROFILE_FILE) < 0)
		return -1;
	if (dlp_read_profile(path, &header, &buffer))
		goto out;
	if (replay_profile_parse(buffer, &header, profile))
		goto out;
	if (read_boot_identity(root, &current) ||
	    strcmp(header.boot_id, current.boot_id) ||
	    strcmp(header.release, current.release) ||
	    strcmp(header.version_string, current.version_string) ||
	    !replay_metadata_matches(profile, metadata)) {
		errno = ESTALE;
		replay_profile_clear(profile);
		goto out;
	}
	ret = 0;
out:
	free(path);
	free(buffer);
	return ret;
}

static int replay_file_identity_matches(const struct dlp_profile_file *identity,
					const struct stat *st,
					uint32_t mount_id)
{
	return identity->dev == (uint64_t)st->st_dev &&
		identity->ino == (uint64_t)st->st_ino &&
		identity->size == (uint64_t)st->st_size &&
		identity->ctime_sec == st->st_ctim.tv_sec &&
		identity->ctime_nsec == (uint32_t)st->st_ctim.tv_nsec &&
		identity->mtime_sec == st->st_mtim.tv_sec &&
		identity->mtime_nsec == (uint32_t)st->st_mtim.tv_nsec &&
		identity->mount_id == mount_id;
}

static int replay_memory_pressure(void)
{
	struct sysinfo info;
	uint64_t total, free_pages;

	if (sysinfo(&info) || !info.totalram)
		return 0;
	total = (uint64_t)info.totalram * info.mem_unit;
	free_pages = (uint64_t)info.freeram * info.mem_unit;
	return free_pages < total / 50U;
}

int dlp_replay_profile(const char *root, const struct dlp_metadata *metadata,
		       const struct dlp_replay_callbacks *callbacks,
		       struct dlp_replay_result *result)
{
	struct replay_profile profile;
	struct mount_table mounts;
	struct timespec started;
	uint8_t *scratch = NULL;
	uint64_t read_attempts = 0;
	int window_started = 0;
	int ret = -EINVAL;

	if (!root || !metadata || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	memset(&profile, 0, sizeof(profile));
	if (replay_profile_load(root, metadata, &profile) ||
	    mount_table_load(&mounts)) {
		result->profile_invalid = 1;
		ret = -ESTALE;
		goto out;
	}
	scratch = malloc(128U * 1024U);
	if (!scratch) {
		ret = -ENOMEM;
		goto out;
	}
	ret = 0;
	for (uint32_t i = 0; i < profile.file_count; i++) {
		struct replay_file *file = &profile.files[i];
		const struct dlp_metadata_path *trusted =
			allowed_path(metadata, file->path);
		struct stat selected, after;
		int fd = -1;

		if (callbacks && callbacks->cancelled &&
		    callbacks->cancelled(callbacks->context)) {
			result->cancelled++;
			ret = -ECANCELED;
			goto out;
		}
		if (result->ranges_attempted >= 1024) {
			result->range_limit = 1;
			break;
		}
		if (!trusted ||
		    file->identity.mount_id != mount_id_for_path(&mounts, file->path)) {
			result->profile_invalid++;
			ret = -ESTALE;
			goto out;
		}
		fd = safe_open_path(file->path, metadata->uid, &selected);
		if (fd < 0 ||
		    !replay_file_identity_matches(&file->identity, &selected,
						 file->identity.mount_id)) {
			if (fd >= 0)
				close(fd);
			result->profile_invalid++;
			ret = -ESTALE;
			goto out;
		}
		if (file->identity.flags & DLP_PROFILE_DIGEST_COPY) {
			uint8_t digest[32];

			if (dlp_sha256_fd(fd, digest) ||
			    memcmp(digest, file->identity.digest, 32)) {
				close(fd);
				result->profile_invalid++;
				ret = -ESTALE;
				goto out;
			}
		}
		if (fstat(fd, &after) ||
		    !replay_file_identity_matches(&file->identity, &after,
						 file->identity.mount_id)) {
			close(fd);
			result->profile_invalid++;
			ret = -ESTALE;
			goto out;
		}
		result->files_attempted++;
		for (uint32_t j = 0; j < file->identity.range_count; j++) {
			const struct dlp_profile_range *range =
				&profile.ranges[file->range_start + j];
			uint64_t offset = range->offset;
			uint64_t remaining = range->length;

			if (result->ranges_attempted >= 1024) {
				result->range_limit = 1;
				break;
			}
			result->ranges_attempted++;
			while (remaining) {
				struct timespec now;
				uint64_t elapsed;
				size_t requested = remaining > 128U * 1024U ?
					128U * 1024U : (size_t)remaining;
				ssize_t completed;

				if (callbacks && callbacks->cancelled &&
				    callbacks->cancelled(callbacks->context)) {
					result->cancelled++;
					close(fd);
					ret = -ECANCELED;
					goto out;
				}
				if (!window_started) {
					if (clock_gettime(CLOCK_BOOTTIME, &started)) {
						close(fd);
						ret = -errno;
						goto out;
					}
					window_started = 1;
				} else {
					if (clock_gettime(CLOCK_BOOTTIME, &now)) {
						close(fd);
						ret = -errno;
						goto out;
					}
					elapsed = (uint64_t)(now.tv_sec - started.tv_sec) *
						1000000000ULL + now.tv_nsec;
					if (now.tv_nsec >= started.tv_nsec)
						elapsed -= (uint64_t)started.tv_nsec;
					else
						elapsed += 1000000000ULL -
							(uint64_t)started.tv_nsec;
					if (elapsed >= 500000000ULL) {
						result->window_stop++;
						close(fd);
						goto out;
					}
				}
				if (replay_memory_pressure()) {
					result->pressure_stop++;
					close(fd);
					goto out;
				}
				if (read_attempts >= 16U * 1024U * 1024U) {
					result->budget_stop++;
					close(fd);
					goto out;
				}
				if (requested > 16U * 1024U * 1024U -
				    read_attempts)
					requested = 16U * 1024U * 1024U -
						read_attempts;
				result->read_requested += requested;
				read_attempts += requested;
				do {
					completed = pread(fd, scratch, requested, offset);
				} while (completed < 0 && errno == EINTR);
				if (completed < 0) {
					result->errors++;
					close(fd);
					ret = -errno;
					goto out;
				}
				if (!completed) {
					result->short_reads++;
					close(fd);
					goto out;
				}
				result->read_completed += (uint64_t)completed;
				offset += (uint64_t)completed;
				remaining -= (uint64_t)completed;
				if ((size_t)completed < requested) {
					result->short_reads++;
					close(fd);
					goto out;
				}
				if (read_attempts == 16U * 1024U * 1024U) {
					result->budget_stop++;
					close(fd);
					goto out;
				}
			}
			if (result->range_limit)
				break;
		}
		close(fd);
		if (result->range_limit)
			break;
	}
out:
	free(scratch);
	replay_profile_clear(&profile);
	return ret;
}

static int read_snapshot(int fd, struct snapshot_range **ranges_out,
			 uint32_t *range_count_out, uint32_t *file_count_out,
			 uint32_t *shift_out)
{
	struct dlp_snapshot_header header;
	uint8_t *buffer = NULL;
	struct snapshot_range *ranges = NULL;
	uint32_t total_size, count, number, shift, flags;
	uint64_t bytes = 0;
	uint64_t previous_end = 0;
	uint32_t previous_index = UINT32_MAX;
	int ret = -1;

	if (pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
		errno = EINVAL;
		goto out;
	}
	total_size = header.size;
	count = header.nr_files;
	number = header.nr_ranges;
	shift = header.page_shift;
	flags = header.flags;
	if (header.magic != 0x31504c44U || header.version != 1 ||
	    total_size < SNAPSHOT_HEADER_SIZE ||
	    total_size > SNAPSHOT_MAX_SIZE ||
	    total_size != SNAPSHOT_HEADER_SIZE + number * SNAPSHOT_RECORD_SIZE ||
	    count > DLP_MAX_PATHS || number > DLP_SNAPSHOT_RANGES ||
	    (flags & ~(uint32_t)DLP_TRUNCATED) ||
	    (flags & DLP_TRUNCATED) ||
	    shift != (uint32_t)page_shift_runtime()) {
		errno = EINVAL;
		goto out;
	}
	buffer = malloc(total_size);
	if (!buffer)
		goto out;
	if (pread(fd, buffer, total_size, 0) != (ssize_t)total_size) {
		errno = EINVAL;
		goto out;
	}
	ranges = calloc(number ? number : 1, sizeof(*ranges));
	if (!ranges)
		goto out;
	for (uint32_t i = 0; i < number; i++) {
		const uint8_t *record = buffer + SNAPSHOT_HEADER_SIZE +
			i * SNAPSHOT_RECORD_SIZE;
		uint32_t reserved;
		uint64_t mask = (1ULL << shift) - 1;

		memcpy(&ranges[i].index, record, 4);
		memcpy(&reserved, record + 4, 4);
		memcpy(&ranges[i].offset, record + 8, 8);
		memcpy(&ranges[i].length, record + 16, 8);
		if (ranges[i].index >= count || reserved || !ranges[i].length ||
		    ranges[i].offset > UINT64_MAX - ranges[i].length ||
		    ((ranges[i].offset | ranges[i].length) & mask) ||
		    ranges[i].length > 32ULL * 1024 * 1024 ||
		    bytes > 32ULL * 1024 * 1024 - ranges[i].length ||
		    (i && (ranges[i].index < previous_index ||
			   (ranges[i].index == previous_index &&
			    ranges[i].offset < previous_end)))) {
			errno = EINVAL;
			goto out;
		}
		bytes += ranges[i].length;
		previous_index = ranges[i].index;
		previous_end = ranges[i].offset + ranges[i].length;
	}
	*ranges_out = ranges;
	*range_count_out = number;
	*file_count_out = count;
	*shift_out = shift;
	ranges = NULL;
	ret = 0;
out:
	free(buffer);
	free(ranges);
	return ret;
}

static char *exported_path(int fd)
{
	char link[64], path[PATH_MAX + 32];
	ssize_t size;

	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	size = readlink(link, path, sizeof(path) - 1);
	if (size <= 0 || size >= (ssize_t)sizeof(path))
		return NULL;
	path[size] = '\0';
	if (strlen(" (deleted)") < (size_t)size &&
	    !strcmp(path + size - strlen(" (deleted)"), " (deleted)"))
		return NULL;
	if (!valid_path(path, (size_t)size))
		return NULL;
	return strdup(path);
}

static void free_output_files(struct output_file *files, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		free(files[i].path);
		free(files[i].ranges);
	}
}

static int build_profile(const struct dlp_metadata *metadata,
			 const struct output_file *files, uint32_t file_count,
			 uint32_t range_count, uint32_t shift, uint32_t flags,
			 const char *root,
			 uint8_t **profile_out, size_t *profile_size_out)
{
	struct dlp_profile_header header;
	uint8_t *profile = NULL, *cursor;
	size_t size = sizeof(header);
	size_t range_bytes;
	uint32_t stored_crc;
	struct timespec published;

	if (checked_add(&size, metadata->package_len) ||
	    checked_mul(32, metadata->signer_count, &range_bytes) ||
	    checked_add(&size, range_bytes))
		return -1;
	for (uint32_t i = 0; i < file_count; i++) {
		if (checked_add(&size, files[i].identity.path_len) ||
		    checked_add(&size, sizeof(files[i].identity)))
			return -1;
	}
	if (checked_mul(sizeof(struct dlp_profile_range), range_count,
			&range_bytes) || checked_add(&size, range_bytes))
		return -1;
	if (size > DLP_PROFILE_MAX_SIZE) {
		errno = EFBIG;
		return -1;
	}
	profile = calloc(1, size);
	if (!profile)
		return -1;
	memset(&header, 0, sizeof(header));
	header.magic = DLP_PROFILE_MAGIC;
	header.version = DLP_PROFILE_VERSION;
	header.header_size = sizeof(header);
	header.total_size = (uint32_t)size;
	header.flags = flags;
	header.package_len = metadata->package_len;
	header.signer_count = metadata->signer_count;
	header.file_count = file_count;
	header.range_count = range_count;
	header.page_shift = shift;
	header.user_id = metadata->user_id;
	header.uid = metadata->uid;
	header.metadata_generation = metadata->generation;
	header.long_version_code = metadata->long_version_code;
	if (clock_gettime(CLOCK_REALTIME, &published))
		goto fail;
	header.published_ns = (uint64_t)published.tv_sec * 1000000000ULL +
		(uint64_t)published.tv_nsec;
	if (read_boot_identity(root, &header))
		goto fail;
	memcpy(profile, &header, sizeof(header));
	cursor = profile + sizeof(header);
	memcpy(cursor, metadata->package, metadata->package_len);
	cursor += metadata->package_len;
	for (uint32_t i = 0; i < metadata->signer_count; i++) {
		memcpy(cursor, metadata->signers[i], 32);
		cursor += 32;
	}
	for (uint32_t i = 0; i < file_count; i++) {
		memcpy(cursor, &files[i].identity, sizeof(files[i].identity));
		cursor += sizeof(files[i].identity);
		memcpy(cursor, files[i].path, files[i].identity.path_len);
		cursor += files[i].identity.path_len;
	}
	for (uint32_t i = 0; i < file_count; i++) {
		for (uint32_t j = 0; j < files[i].identity.range_count; j++) {
			struct dlp_profile_range range = {
				.offset = files[i].ranges[j].offset,
				.length = files[i].ranges[j].length,
			};

			memcpy(cursor, &range, sizeof(range));
			cursor += sizeof(range);
		}
	}
	stored_crc = crc32_bytes(0, profile, size);
	((struct dlp_profile_header *)profile)->crc32 = stored_crc;
	*profile_out = profile;
	*profile_size_out = size;
	return 0;
fail:
	free(profile);
	return -1;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t written = write(fd, data + done, size - done);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		done += (size_t)written;
	}
	return 0;
}

static int mkdir_openat(int parent, const char *name, mode_t mode, int *result)
{
	int fd;

	if (mkdirat(parent, name, mode) && errno != EEXIST)
		return -1;
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	*result = fd;
	return 0;
}

static int atomic_store_profile(const char *root,
				const struct dlp_metadata *metadata,
				const uint8_t *profile, size_t profile_size)
{
	char user[16], package[DLP_MAX_PACKAGE + 1], temporary[64];
	int root_fd = -1, user_fd = -1, package_fd = -1, temporary_fd = -1;
	int ret = -1;
	int backup_linked = 0, renamed = 0;
	struct timespec now;

	temporary[0] = '\0';
	memcpy(package, metadata->package, metadata->package_len);
	package[metadata->package_len] = '\0';
	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root_fd < 0)
		goto out;
	snprintf(user, sizeof(user), "%u", metadata->user_id);
	if (mkdir_openat(root_fd, user, 0700, &user_fd) ||
	    mkdir_openat(user_fd, package, 0700, &package_fd))
		goto out;
	if (clock_gettime(CLOCK_REALTIME, &now))
		goto out;
	snprintf(temporary, sizeof(temporary), DLP_PROFILE_TMP_PREFIX "%ld.%lld.%ld",
		 (long)getpid(), (long long)now.tv_sec, now.tv_nsec);
	temporary_fd = openat(package_fd, temporary,
			      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			      0600);
	if (temporary_fd < 0)
		goto out;
	if (write_all(temporary_fd, profile, profile_size) ||
	    fsync(temporary_fd))
		goto out;
	if (close(temporary_fd))
		goto out;
	temporary_fd = -1;
	struct stat old_stat;

	if (!fstatat(package_fd, DLP_PROFILE_FILE, &old_stat,
		     AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(old_stat.st_mode)) {
			errno = EINVAL;
			goto out;
		}
		if (unlinkat(package_fd, DLP_PROFILE_BACKUP, 0) &&
		    errno != ENOENT)
			goto out;
		if (linkat(package_fd, DLP_PROFILE_FILE, package_fd,
			   DLP_PROFILE_BACKUP, 0))
			goto out;
		backup_linked = 1;
	} else if (errno != ENOENT) {
		goto out;
	}
	if (renameat(package_fd, temporary, package_fd, DLP_PROFILE_FILE))
		goto out;
	renamed = 1;
	if (fsync(package_fd)) {
		if (backup_linked)
			(void)renameat(package_fd, DLP_PROFILE_BACKUP, package_fd,
				       DLP_PROFILE_FILE);
		else
			(void)unlinkat(package_fd, DLP_PROFILE_FILE, 0);
		(void)fsync(package_fd);
		goto out;
	}
	if (backup_linked)
		(void)unlinkat(package_fd, DLP_PROFILE_BACKUP, 0);
	(void)fsync(package_fd);
	ret = 0;
out:
	if (temporary_fd >= 0)
		close(temporary_fd);
	if (ret && package_fd >= 0 && temporary[0])
		unlinkat(package_fd, temporary, 0);
	if (ret && !renamed && backup_linked && package_fd >= 0)
		unlinkat(package_fd, DLP_PROFILE_BACKUP, 0);
	if (package_fd >= 0)
		close(package_fd);
	if (user_fd >= 0)
		close(user_fd);
	if (root_fd >= 0)
		close(root_fd);
	return ret;
}

int dlp_publish_profile(const char *root, const struct dlp_metadata *metadata,
			const struct dlp_snapshot_ops *operations)
{
	struct snapshot_range *ranges = NULL;
	struct output_file files[DLP_MAX_PATHS];
	struct mount_table mounts;
	struct stat exported_stat;
	uint64_t generation;
	uint8_t *profile = NULL;
	int snapshot_fd = -1;
	int ret = -1;
	uint32_t range_count, file_count, shift, output_count = 0;
	uint32_t output_ranges = 0, range_cursor = 0, range_base;
	uint32_t flags = 0;
	size_t profile_size;

	memset(files, 0, sizeof(files));
	if (!root || !metadata || !operations || !operations->snapshot ||
	    !operations->export_file || !valid_name(metadata->package,
	    metadata->package_len) || dlp_metadata_generation(metadata, &generation) ||
	    generation != metadata->generation ||
	    mount_table_load(&mounts)) {
		errno = EINVAL;
		goto out;
	}
	if (operations->snapshot(operations->context, &snapshot_fd) ||
	    read_snapshot(snapshot_fd, &ranges, &range_count, &file_count, &shift))
		goto out;
	if (operations->cancelled && operations->cancelled(operations->context)) {
		errno = ECANCELED;
		goto out;
	}
	for (uint32_t index = 0; index < file_count; index++) {
		struct output_file *output;
		struct dlp_profile_file *identity;
		const struct dlp_metadata_path *trusted;
		struct stat safe_stat, after_stat;
		char *path = NULL;
		int exported_fd = -1, safe_fd = -1;
		uint32_t first = range_cursor;

		while (range_cursor < range_count && ranges[range_cursor].index < index)
			range_cursor++;
		if (range_cursor == range_count || ranges[range_cursor].index != index)
			continue;
		while (range_cursor < range_count && ranges[range_cursor].index == index)
			range_cursor++;
		range_base = first;
		if (operations->export_file(operations->context, index, &exported_fd)) {
			if (errno == EACCES || errno == ESTALE) {
				flags |= DLP_PROFILE_SKIPPED;
				continue;
			}
			goto out;
		}
		if (fstat(exported_fd, &exported_stat) ||
		    !S_ISREG(exported_stat.st_mode) ||
		    (fcntl(exported_fd, F_GETFL) & O_ACCMODE) != O_RDONLY) {
			close(exported_fd);
			flags |= DLP_PROFILE_SKIPPED;
			continue;
		}
		path = exported_path(exported_fd);
		trusted = path ? allowed_path(metadata, path) : NULL;
		if (!trusted ||
		    (is_static_readonly_path(path) &&
		     (!mount_for_path(&mounts, path) ||
		      !mount_for_path(&mounts, path)->read_only))) {
			close(exported_fd);
			free(path);
			flags |= DLP_PROFILE_SKIPPED;
			continue;
		}
		safe_fd = safe_open_path(path, metadata->uid, &safe_stat);
		if (safe_fd < 0 || !same_file_identity(&exported_stat, &safe_stat)) {
			if (safe_fd >= 0)
				close(safe_fd);
			close(exported_fd);
			free(path);
			flags |= DLP_PROFILE_SKIPPED;
			continue;
		}
		output = &files[output_count];
		identity = &output->identity;
		output->path = path;
		identity->path_len = (uint32_t)strlen(path);
		identity->dev = safe_stat.st_dev;
		identity->ino = safe_stat.st_ino;
		identity->size = safe_stat.st_size;
		identity->ctime_sec = safe_stat.st_ctim.tv_sec;
		identity->ctime_nsec = safe_stat.st_ctim.tv_nsec;
		identity->mtime_sec = safe_stat.st_mtim.tv_sec;
		identity->mtime_nsec = safe_stat.st_mtim.tv_nsec;
		identity->mount_id = mount_id_for_path(&mounts, path);
		if (!identity->mount_id ||
		    (trusted->kind == DLP_PATH_BASE_APK ||
		     trusted->kind == DLP_PATH_SPLIT_APK ||
		     trusted->kind == DLP_PATH_NATIVE_DIR)) {
			if (dlp_sha256_fd(safe_fd, identity->digest)) {
				close(safe_fd);
				close(exported_fd);
				free(path);
				flags |= DLP_PROFILE_SKIPPED;
				continue;
			}
			identity->flags = DLP_PROFILE_DIGEST_COPY;
		}
		output->ranges = calloc(range_cursor - range_base,
					 sizeof(*output->ranges));
		if (!output->ranges) {
			close(safe_fd);
			close(exported_fd);
			free(path);
			goto out;
		}
		for (uint32_t i = range_base; i < range_cursor; i++) {
			uint64_t end;

			if (ranges[i].offset >= (uint64_t)safe_stat.st_size)
				continue;
			end = ranges[i].offset + ranges[i].length;
			if (end > (uint64_t)safe_stat.st_size)
				end = safe_stat.st_size;
			if (end <= ranges[i].offset)
				continue;
			output->ranges[identity->range_count].offset = ranges[i].offset;
			output->ranges[identity->range_count].length =
				end - ranges[i].offset;
			identity->range_count++;
		}
		if (!identity->range_count || fstat(safe_fd, &after_stat) ||
		    !same_file_identity(&safe_stat, &after_stat)) {
			close(safe_fd);
			close(exported_fd);
			free(output->ranges);
			output->ranges = NULL;
			free(path);
			memset(output, 0, sizeof(*output));
			if (errno == ECANCELED)
				goto out;
			flags |= DLP_PROFILE_SKIPPED;
			continue;
		}
		if (operations->cancelled &&
		    operations->cancelled(operations->context)) {
			close(safe_fd);
			close(exported_fd);
			free(output->ranges);
			output->ranges = NULL;
			free(path);
			memset(output, 0, sizeof(*output));
			errno = ECANCELED;
			goto out;
		}
		output_ranges += identity->range_count;
		output_count++;
		close(safe_fd);
		close(exported_fd);
	}
	if (!output_count || build_profile(metadata, files, output_count,
					   output_ranges, shift, flags, root, &profile,
					   &profile_size))
		goto out;
	if (operations->cancelled && operations->cancelled(operations->context)) {
		errno = ECANCELED;
		goto out;
	}
	char package[DLP_MAX_PACKAGE + 1];

	memcpy(package, metadata->package, metadata->package_len);
	package[metadata->package_len] = '\0';
	if (enforce_quota(root, profile_size, package, metadata->user_id) ||
	    atomic_store_profile(root, metadata, profile, profile_size))
		goto out;
	ret = 0;
out:
	if (snapshot_fd >= 0)
		close(snapshot_fd);
	free(profile);
	free_output_files(files, output_count);
	free(ranges);
	return ret;
}

static int open_child_dir(int parent, const char *name)
{
	return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int remove_package_at(int user_fd, const char *package)
{
	DIR *directory;
	int package_fd = open_child_dir(user_fd, package);
	int ret = 0;

	if (package_fd < 0)
		return errno == ENOENT ? 0 : -1;
	directory = fdopendir(dup(package_fd));
	if (directory) {
		struct dirent *entry;

		while ((entry = readdir(directory))) {
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;
			if (!strcmp(entry->d_name, DLP_PROFILE_FILE) ||
			    !strcmp(entry->d_name, DLP_PROFILE_BACKUP) ||
			    !strncmp(entry->d_name, DLP_PROFILE_TMP_PREFIX,
				     strlen(DLP_PROFILE_TMP_PREFIX))) {
				if (unlinkat(package_fd, entry->d_name, 0) &&
				    errno != ENOENT)
					ret = -1;
			}
		}
		closedir(directory);
	} else {
		ret = -1;
	}
	close(package_fd);
	if (unlinkat(user_fd, package, AT_REMOVEDIR) && errno != ENOENT &&
	    errno != ENOTEMPTY)
		ret = -1;
	return ret;
}

int dlp_invalidate_profile(const char *root, uint32_t user_id,
			   const char *package)
{
	char user[16];
	int root_fd = -1, user_fd = -1;
	int ret = 0;

	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root_fd < 0)
		return errno == ENOENT ? 0 : -1;
	snprintf(user, sizeof(user), "%u", user_id);
	user_fd = open_child_dir(root_fd, user);
	if (user_fd < 0) {
		if (errno == ENOENT)
			ret = 0;
		else
			ret = -1;
		goto out;
	}
	if (package && *package) {
		if (!valid_name(package, strlen(package))) {
			errno = EINVAL;
			ret = -1;
			goto out;
		}
		if (remove_package_at(user_fd, package))
			ret = -1;
	} else {
		DIR *directory = fdopendir(dup(user_fd));

		if (!directory) {
			ret = -1;
			goto out;
		}
		struct dirent *entry;

		while ((entry = readdir(directory))) {
			if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
				if (remove_package_at(user_fd, entry->d_name))
					ret = -1;
		}
		closedir(directory);
	}
out:
	if (user_fd >= 0)
		close(user_fd);
	if (root_fd >= 0)
		close(root_fd);
	return ret;
}

static int quota_compare(const void *left, const void *right)
{
	const struct quota_entry *a = left;
	const struct quota_entry *b = right;

	if (a->mtime != b->mtime)
		return a->mtime < b->mtime ? -1 : 1;
	if (a->mtime_nsec != b->mtime_nsec)
		return a->mtime_nsec < b->mtime_nsec ? -1 : 1;
	if (strcmp(a->user, b->user))
		return strcmp(a->user, b->user);
	return strcmp(a->package, b->package);
}

static int append_quota(struct quota_entry **entries, size_t *capacity,
			size_t *count, const struct quota_entry *entry)
{
	if (*count == *capacity) {
		size_t new_capacity = *capacity ? *capacity * 2 : 64;
		struct quota_entry *grown;

		if (new_capacity < *capacity ||
		    new_capacity > SIZE_MAX / sizeof(**entries))
			return -1;
		grown = realloc(*entries, new_capacity * sizeof(**entries));
		if (!grown)
			return -1;
		*entries = grown;
		*capacity = new_capacity;
	}
	(*entries)[(*count)++] = *entry;
	return 0;
}

static int collect_quota(const char *root, struct quota_entry **entries_out,
			 size_t *count, uint64_t *total)
{
	DIR *users_dir = NULL;
	struct quota_entry *entries = NULL;
	size_t capacity = 0;
	int root_fd = -1;

	*count = 0;
	*total = 0;
	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root_fd < 0)
		return errno == ENOENT ? 0 : -1;
	users_dir = fdopendir(dup(root_fd));
	if (!users_dir)
		goto fail;
	struct dirent *user_entry;

	while ((user_entry = readdir(users_dir))) {
		DIR *packages_dir;
		int user_fd;

		size_t user_length = strlen(user_entry->d_name);

		if (!valid_user_name(user_entry->d_name, user_length))
			continue;
		user_fd = open_child_dir(root_fd, user_entry->d_name);
		if (user_fd < 0)
			continue;
		packages_dir = fdopendir(dup(user_fd));
		if (!packages_dir) {
			close(user_fd);
			goto fail;
		}
		struct dirent *package_entry;

		while ((package_entry = readdir(packages_dir))) {
			struct stat st;
			int package_fd;

			if (!valid_name(package_entry->d_name,
					strlen(package_entry->d_name)))
				continue;
			package_fd = open_child_dir(user_fd, package_entry->d_name);
			if (package_fd < 0)
				continue;
			if (!fstatat(package_fd, DLP_PROFILE_FILE, &st,
				     AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) &&
			    st.st_size >= 0) {
				struct quota_entry entry = {
					.size = st.st_size,
					.mtime = st.st_mtim.tv_sec,
					.mtime_nsec = (uint32_t)st.st_mtim.tv_nsec,
				};

				memcpy(entry.user, user_entry->d_name, user_length);
				entry.user[user_length] = '\0';
				snprintf(entry.package, sizeof(entry.package), "%s",
					 package_entry->d_name);
				if (append_quota(&entries, &capacity, count, &entry)) {
					close(package_fd);
					goto fail;
				}
				if (*total > UINT64_MAX - (uint64_t)st.st_size) {
					close(package_fd);
					goto fail;
				}
				*total += (uint64_t)st.st_size;
			}
			close(package_fd);
		}
		closedir(packages_dir);
		close(user_fd);
	}
	closedir(users_dir);
	close(root_fd);
	*entries_out = entries;
	return 0;
fail:
	if (users_dir)
		closedir(users_dir);
	if (root_fd >= 0)
		close(root_fd);
	free(entries);
	return -1;
}

static int enforce_quota(const char *root, uint64_t add_size,
			 const char *replace_package, uint32_t replace_user)
{
	struct quota_entry *entries = NULL;
	size_t count, remaining;
	uint64_t total;
	int ret = 0;

	if (collect_quota(root, &entries, &count, &total))
		return -1;
	remaining = count;
	if (replace_package) {
		for (size_t i = 0; i < count; i++) {
			if (entries[i].user[0] &&
			    strtoul(entries[i].user, NULL, 10) == replace_user &&
			    !strcmp(entries[i].package, replace_package)) {
				if (total >= entries[i].size)
					total -= entries[i].size;
				remaining--;
				break;
			}
		}
	}
	if (total > UINT64_MAX - add_size)
		goto fail;
	total += add_size;
	if (add_size)
		remaining++;
	if (count)
		qsort(entries, count, sizeof(entries[0]), quota_compare);
	for (size_t i = 0; i < count &&
	     (remaining > DLP_PROFILE_MAX_PACKAGES ||
	      total > DLP_PROFILE_TOTAL_MAX); i++) {
		uint32_t user_id = (uint32_t)strtoul(entries[i].user, NULL, 10);

		if (replace_package &&
		    user_id == replace_user &&
		    !strcmp(entries[i].package, replace_package))
			continue;
		if (dlp_invalidate_profile(root, user_id, entries[i].package))
			goto fail;
		if (total >= entries[i].size)
			total -= entries[i].size;
		remaining--;
	}
	if (remaining > DLP_PROFILE_MAX_PACKAGES ||
	    total > DLP_PROFILE_TOTAL_MAX) {
		errno = ENOSPC;
		goto fail;
	}
	goto out;
fail:
	ret = -1;
out:
	free(entries);
	return ret;
}

int dlp_sweep_profile_store(const char *root)
{
	struct dlp_profile_header current, stored;
	struct utsname name;
	uint64_t unused;
	int user_fd, root_fd;
	DIR *users;

	if (read_boot_identity(root, &current) || uname(&name))
		return -1;
	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root_fd < 0)
		return errno == ENOENT ? 0 : -1;
	users = fdopendir(dup(root_fd));
	if (!users) {
		close(root_fd);
		return -1;
	}
	struct dirent *user_entry;
	while ((user_entry = readdir(users))) {
		DIR *packages;
		char user[16];

		size_t user_length = strlen(user_entry->d_name);

		if (!valid_user_name(user_entry->d_name, user_length))
			continue;
		memcpy(user, user_entry->d_name, user_length);
		user[user_length] = '\0';
		user_fd = open_child_dir(root_fd, user);
		if (user_fd < 0)
			continue;
		packages = fdopendir(dup(user_fd));
		if (!packages) {
			close(user_fd);
			continue;
		}
		struct dirent *package_entry;
		while ((package_entry = readdir(packages))) {
			char path[PATH_MAX];
			DIR *profile_dir;
			int package_fd;

			if (!valid_name(package_entry->d_name,
					strlen(package_entry->d_name)))
				continue;
			package_fd = open_child_dir(user_fd, package_entry->d_name);
			if (package_fd < 0)
				continue;
			profile_dir = fdopendir(dup(package_fd));
			if (profile_dir) {
				struct dirent *entry;

				while ((entry = readdir(profile_dir))) {
					if (!strncmp(entry->d_name, DLP_PROFILE_TMP_PREFIX,
						     strlen(DLP_PROFILE_TMP_PREFIX)) ||
					    !strcmp(entry->d_name, DLP_PROFILE_BACKUP))
						unlinkat(package_fd, entry->d_name, 0);
				}
				closedir(profile_dir);
			}
			snprintf(path, sizeof(path), "%s/%s/%s/%s", root, user,
				 package_entry->d_name, DLP_PROFILE_FILE);
			if (!dlp_read_profile(path, &stored, NULL) &&
			    (strcmp(stored.boot_id, current.boot_id) ||
			     strcmp(stored.release, current.release) ||
			     strcmp(stored.version_string, current.version_string)))
				unlinkat(package_fd, DLP_PROFILE_FILE, 0);
			close(package_fd);
		}
		closedir(packages);
		close(user_fd);
	}
	closedir(users);
	close(root_fd);
	(void)unused;
	return enforce_quota(root, 0, NULL, 0);
}
