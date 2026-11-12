/* SPDX-License-Identifier: Apache-2.0 */
/* Exercises the production DLP2 parser and snapshot-to-profile store seam. */
#define main helper_daemon_main
#include "../launch_profile_helper.c"
#undef main

#include <assert.h>
#include <limits.h>
#include <sys/mman.h>

struct fake_snapshot {
	int snapshot;
	const char *base_path;
	int truncated;
};

static int fake_snapshot_open(void *context, int *fd)
{
	struct fake_snapshot *fake = context;
	struct dlp_snapshot_header header = {
		.magic = 0x31504c44,
		.version = 1,
		.size = sizeof(header) + sizeof(struct dlp_snapshot_range),
		.nr_files = 1,
		.nr_ranges = 1,
		.flags = fake->truncated ? DLP_TRUNCATED : 0,
		.page_shift = 12,
	};
	struct dlp_snapshot_range range = {
		.file_index = 0,
		.offset = 0,
		.length = 8192,
	};
	int output = dup(fake->snapshot);

	assert(output >= 0);
	assert(pwrite(output, &header, sizeof(header), 0) == sizeof(header));
	assert(pwrite(output, &range, sizeof(range), sizeof(header)) == sizeof(range));
	assert(ftruncate(output, sizeof(header) + sizeof(range)) == 0);
	*fd = output;
	return 0;
}

static int fake_export(void *context, uint32_t index, int *fd)
{
	struct fake_snapshot *fake = context;

	assert(index == 0);
	*fd = open(fake->base_path, O_RDONLY | O_CLOEXEC);
	return *fd < 0 ? -1 : 0;
}

static int fake_cancelled(void *context)
{
	(void)context;
	return 0;
}

static int replay_cancel_immediately(void *context)
{
	(void)context;
	return 1;
}

static void metadata_generation_test(struct dlp_metadata *metadata)
{
	uint64_t generation, altered;
	uint8_t (*signers)[32] = (uint8_t (*)[32])metadata->signers;

	assert(dlp_metadata_generation(metadata, &generation) == 0);
	metadata->generation = generation;
	signers[0][0] ^= 1;
	assert(dlp_metadata_generation(metadata, &altered) == 0 && altered != generation);
	signers[0][0] ^= 1;
}

static void verify_profile(const char *path, const struct dlp_metadata *metadata,
			   const char *base_path, const uint8_t expected_digest[32],
			   uint64_t expected_size)
{
	struct dlp_profile_header header;
	uint8_t *profile = NULL;
	struct dlp_profile_file file;
	const uint8_t *cursor;
	struct dlp_profile_range range;

	assert(dlp_read_profile(path, &header, &profile) == 0);
	assert(header.metadata_generation == metadata->generation);
	assert(header.user_id == metadata->user_id && header.uid == metadata->uid);
	assert(header.file_count == 1 && header.range_count == 1);
	assert(header.package_len == metadata->package_len);
	assert(!memcmp(profile + sizeof(header), metadata->package, metadata->package_len));
	cursor = profile + sizeof(header) + metadata->package_len +
		metadata->signer_count * 32U;
	memcpy(&file, cursor, sizeof(file));
	cursor += sizeof(file);
	assert(file.path_len == strlen(base_path));
	assert(!memcmp(cursor, base_path, file.path_len));
	cursor += file.path_len;
	memcpy(&range, cursor, sizeof(range));
	assert(range.offset == 0 && range.length == expected_size);
	assert(file.range_count == 1 && file.size == expected_size);
	assert((file.flags & DLP_PROFILE_DIGEST_COPY) != 0);
	assert(!memcmp(file.digest, expected_digest, 32));
	free(profile);
}

static void parse_wire_test(const struct dlp_metadata *metadata)
{
	size_t size = sizeof(struct dlp_wire_header) + metadata->package_len +
		metadata->signer_count * 32U + sizeof(struct dlp_wire_path) +
		metadata->paths[0].length;
	uint8_t *packet = calloc(1, size);
	struct dlp_wire_header header = {
		.magic = DLP_WIRE_MAGIC,
		.version = DLP_WIRE_VERSION,
		.total_size = size,
		.op = DLP_OP_PREPARE,
		.epoch = 7,
		.cookie = 9,
		.user_id = metadata->user_id,
		.uid = metadata->uid,
		.metadata_generation = metadata->generation,
		.long_version_code = metadata->long_version_code,
		.package_len = metadata->package_len,
		.path_count = 1,
		.signer_count = 1,
	};
	struct dlp_wire_path path = {
		.kind = DLP_PATH_BASE_APK,
		.length = metadata->paths[0].length,
	};
	struct wire_message message;
	uint8_t *cursor;

	assert(packet);
	memcpy(packet, &header, sizeof(header));
	cursor = packet + sizeof(header);
	memcpy(cursor, metadata->package, metadata->package_len);
	cursor += metadata->package_len;
	memcpy(cursor, metadata->signers[0], 32);
	cursor += 32;
	memcpy(cursor, &path, sizeof(path));
	cursor += sizeof(path);
	memcpy(cursor, metadata->paths[0].path, metadata->paths[0].length);
	assert(parse_wire(packet, size, &message) == 0);
	assert(message.header.op == DLP_OP_PREPARE);
	assert(message.metadata.generation == metadata->generation);
	packet[sizeof(header) + metadata->package_len] ^= 1;
	assert(parse_wire(packet, size, &message) == -EINVAL);
	free(packet);
}

int main(void)
{
	char root_name[] = "dlp-profile-root.XXXXXX";
	char tree_name[] = "dlp-profile-tree.XXXXXX";
	const char *home = getenv("HOME");
	char root[PATH_MAX], tree[PATH_MAX];
	char *base_path = NULL, *boot_path = NULL, *profile_path = NULL;
	char *read_root = NULL, *user_path = NULL;
	char *root_template = NULL, *tree_template = NULL;
	struct dlp_metadata metadata;
	struct fake_snapshot fake = { .truncated = 0 };
	struct dlp_snapshot_ops operations = {
		.snapshot = fake_snapshot_open,
		.export_file = fake_export,
		.cancelled = fake_cancelled,
		.context = &fake,
	};
	uint8_t signer[32], expected_digest[32];
	uint8_t content[5000];
	uint64_t generation;
	int fd;

	assert(home && *home);
	assert(asprintf(&root_template, "%s/%s", home, root_name) >= 0);
	assert(asprintf(&tree_template, "%s/%s", home, tree_name) >= 0);
	assert(mkdtemp(root_template));
	assert(mkdtemp(tree_template));
	assert(realpath(root_template, root));
	assert(realpath(tree_template, tree));
	assert(asprintf(&read_root, "%s/store", root) >= 0);
	assert(mkdir(read_root, 0700) == 0);
	assert(asprintf(&boot_path, "%s/%s", read_root, DLP_BOOT_ID_FILE) >= 0);
	fd = open(boot_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0);
	assert(write(fd, "01234567-89ab-cdef-0123-456789abcdef", 36) == 36);
	assert(close(fd) == 0);
	assert(asprintf(&base_path, "%s/base.apk", tree) >= 0);
	fd = open(base_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	assert(fd >= 0);
	for (size_t i = 0; i < sizeof(content); i++)
		content[i] = (uint8_t)(i * 17U + 3U);
	assert(write(fd, content, sizeof(content)) == sizeof(content));
	assert(close(fd) == 0);
	fd = open(base_path, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	assert(dlp_sha256_fd(fd, expected_digest) == 0);
	close(fd);
	memset(&metadata, 0, sizeof(metadata));
	metadata.user_id = 0;
	metadata.uid = 10123;
	metadata.long_version_code = 42;
	metadata.package = "com.example.publish";
	metadata.package_len = strlen(metadata.package);
	for (size_t i = 0; i < sizeof(signer); i++)
		signer[i] = (uint8_t)(255U - i);
	metadata.signers = (const uint8_t (*)[32])&signer;
	metadata.signer_count = 1;
	metadata.path_count = 1;
	metadata.paths[0].kind = DLP_PATH_BASE_APK;
	metadata.paths[0].length = strlen(base_path);
	metadata.paths[0].path = base_path;
	metadata_generation_test(&metadata);
	assert(dlp_metadata_generation(&metadata, &generation) == 0);
	metadata.generation = generation;
	parse_wire_test(&metadata);
	fake.snapshot = memfd_create("dlp-profile-test", MFD_CLOEXEC);
	assert(fake.snapshot >= 0);
	fake.base_path = base_path;
	int published = dlp_publish_profile(read_root, &metadata, &operations);
	if (published) fprintf(stderr, "publish failed errno=%d (%s)\n",
			       errno, strerror(errno));
	assert(published == 0);
	assert(asprintf(&profile_path, "%s/%u/%s/%s", read_root,
			metadata.user_id, metadata.package, DLP_PROFILE_FILE) >= 0);
	verify_profile(profile_path, &metadata, base_path, expected_digest,
		       sizeof(content));
	struct dlp_replay_result replay;
	struct dlp_replay_callbacks callbacks = {
		.cancelled = replay_cancel_immediately,
	};
	assert(dlp_replay_profile(read_root, &metadata, NULL, &replay) == 0);
	fprintf(stderr, "replay requested=%llu completed=%llu files=%u ranges=%u\n",
		(unsigned long long)replay.read_requested,
		(unsigned long long)replay.read_completed,
		replay.files_attempted, replay.ranges_attempted);
	assert(replay.files_attempted == 1 && replay.ranges_attempted == 1);
	assert(replay.read_requested == sizeof(content));
	assert(replay.read_completed == sizeof(content));
	assert(dlp_replay_profile(read_root, &metadata, &callbacks, &replay) ==
		       -ECANCELED);
	assert(replay.read_completed == 0 && replay.cancelled == 1);

	/* Truncated snapshots and altered metadata generations never replace it. */
	uint8_t old_digest[32], new_digest[32];
	fd = open(profile_path, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0 && dlp_sha256_fd(fd, old_digest) == 0);
	close(fd);
	fake.truncated = 1;
	assert(dlp_publish_profile(read_root, &metadata, &operations) != 0);
	fake.truncated = 0;
	((uint8_t (*)[32])metadata.signers)[0][0] ^= 1;
	assert(dlp_publish_profile(read_root, &metadata, &operations) != 0);
	assert(dlp_replay_profile(read_root, &metadata, NULL, &replay) ==
		       -ESTALE);
	assert(replay.profile_invalid == 1 && replay.read_completed == 0);
	((uint8_t (*)[32])metadata.signers)[0][0] ^= 1;
	fd = open(profile_path, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0 && dlp_sha256_fd(fd, new_digest) == 0);
	close(fd);
	assert(!memcmp(old_digest, new_digest, 32));

	assert(dlp_invalidate_profile(read_root, metadata.user_id,
				      metadata.package) == 0);
	assert(access(profile_path, F_OK) != 0 && errno == ENOENT);

	close(fake.snapshot);
	assert(asprintf(&user_path, "%s/%u", read_root, metadata.user_id) >= 0);
	assert(rmdir(user_path) == 0);
	assert(unlink(boot_path) == 0);
	assert(rmdir(read_root) == 0);
	assert(rmdir(root) == 0);
	assert(unlink(base_path) == 0);
	assert(rmdir(tree) == 0);
	free(base_path);
	free(boot_path);
	free(profile_path);
	free(read_root);
	free(root_template);
	free(tree_template);
	free(user_path);
	puts("production DLP2/profile publish+replay: EOF trim, digest, atomic reject, bounded read, cancel, invalidate passed");
	return 0;
}
