/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LAUNCH_PROFILE_FORMAT_H
#define LAUNCH_PROFILE_FORMAT_H

#include <stdint.h>

#define DLP_WIRE_MAGIC 0x32504c44U /* "DLP2" */
#define DLP_WIRE_VERSION 2
#define DLP_WIRE_MAX_PACKET 65535U
#define DLP_MAX_PACKAGE 255U
#define DLP_MAX_PATHS 128U
#define DLP_MAX_PATH 4096U
#define DLP_MAX_SIGNERS 16U

#define DLP_OP_PREPARE 1U
#define DLP_OP_BEGIN 2U
#define DLP_OP_FINISH 3U
#define DLP_OP_ABORT 4U
#define DLP_OP_INVALIDATE 5U
#define DLP_OP_OFF 6U

#define DLP_PATH_BASE_APK 1U
#define DLP_PATH_SPLIT_APK 2U
#define DLP_PATH_NATIVE_DIR 3U
#define DLP_PATH_SHARED_FILE 4U
#define DLP_PATH_RESOURCE_DIR 5U

#define DLP_INVALIDATE_PACKAGE 1U
#define DLP_INVALIDATE_USER 2U
#define DLP_INVALIDATE_REASON_UPDATED 1U
#define DLP_INVALIDATE_REASON_REMOVED 2U
#define DLP_INVALIDATE_REASON_DATA_CLEARED 3U
#define DLP_INVALIDATE_REASON_USER_REMOVED 4U

struct dlp_wire_header {
	uint32_t magic;
	uint16_t version;
	uint16_t total_size;
	uint32_t op;
	uint32_t flags;
	uint64_t epoch;
	uint64_t cookie;
	uint32_t user_id;
	uint32_t uid;
	uint64_t metadata_generation;
	uint64_t long_version_code;
	uint32_t package_len;
	uint32_t path_count;
	uint32_t signer_count;
	uint32_t reserved;
};

struct dlp_wire_begin {
	uint32_t tid;
	uint32_t binder_handle;
	uint32_t lease_ms;
	uint32_t reserved;
};

struct dlp_wire_path {
	uint8_t kind;
	uint8_t reserved[3];
	uint16_t length;
	uint16_t reserved2;
};

struct dlp_wire_ack {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t op;
	int32_t status;
	uint64_t epoch;
	uint64_t cookie;
};

#define DLP_PROFILE_MAGIC 0x31504644U /* "DFP1" */
#define DLP_PROFILE_VERSION 1U
#define DLP_PROFILE_FILE "profile.bin"
#define DLP_PROFILE_BACKUP "profile.bin.previous"
#define DLP_BOOT_ID_FILE "boot_id"
#define DLP_PROFILE_TMP_PREFIX "profile.bin.tmp."
#define DLP_PROFILE_MAX_SIZE (1024U * 1024U)
#define DLP_PROFILE_TOTAL_MAX (16U * 1024U * 1024U)
#define DLP_PROFILE_MAX_PACKAGES 64U
#define DLP_PROFILE_DIGEST_COPY 1U
#define DLP_PROFILE_SKIPPED 2U

struct dlp_profile_header {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t total_size;
	uint32_t crc32;
	uint32_t flags;
	uint32_t package_len;
	uint32_t signer_count;
	uint32_t file_count;
	uint32_t range_count;
	uint32_t page_shift;
	uint32_t user_id;
	uint32_t uid;
	uint32_t reserved;
	uint64_t metadata_generation;
	uint64_t long_version_code;
	uint64_t published_ns;
	char boot_id[37];
	char release[128];
	char version_string[128];
};

struct dlp_profile_file {
	uint32_t path_len;
	uint32_t range_count;
	uint64_t dev;
	uint64_t ino;
	uint64_t size;
	int64_t ctime_sec;
	int64_t mtime_sec;
	uint32_t ctime_nsec;
	uint32_t mtime_nsec;
	uint32_t mount_id;
	uint32_t flags;
	uint8_t digest[32];
};

struct dlp_profile_range {
	uint64_t offset;
	uint64_t length;
};

_Static_assert(sizeof(struct dlp_wire_header) == 72, "DLP2 header layout");
_Static_assert(sizeof(struct dlp_wire_begin) == 16, "DLP2 BEGIN layout");
_Static_assert(sizeof(struct dlp_wire_path) == 8, "DLP2 path layout");
_Static_assert(sizeof(struct dlp_wire_ack) == 32, "DLP2 ACK layout");
_Static_assert(sizeof(struct dlp_profile_header) == 376, "profile header layout");
_Static_assert(sizeof(struct dlp_profile_file) == 96, "profile file layout");
_Static_assert(sizeof(struct dlp_profile_range) == 16, "profile range layout");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "DLP wire and profile formats require a little-endian target"
#endif

#endif
