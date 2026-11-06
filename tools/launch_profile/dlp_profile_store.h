/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DLP_PROFILE_STORE_H
#define DLP_PROFILE_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "launch_profile_format.h"

struct dlp_metadata_path {
	uint8_t kind;
	uint16_t length;
	const char *path;
};

struct dlp_metadata {
	uint32_t user_id;
	uint32_t uid;
	uint64_t generation;
	uint64_t long_version_code;
	const char *package;
	uint16_t package_len;
	uint32_t signer_count;
	const uint8_t (*signers)[32];
	uint32_t path_count;
	struct dlp_metadata_path paths[DLP_MAX_PATHS];
};

struct dlp_snapshot_ops {
	int (*snapshot)(void *context, int *snapshot_fd);
	int (*export_file)(void *context, uint32_t index, int *file_fd);
	int (*cancelled)(void *context);
	void *context;
};

struct dlp_replay_callbacks {
	int (*cancelled)(void *context);
	void *context;
};

struct dlp_replay_result {
	uint64_t read_requested;
	uint64_t read_completed;
	uint32_t files_attempted;
	uint32_t ranges_attempted;
	uint32_t short_reads;
	uint32_t errors;
	uint32_t profile_invalid;
	uint32_t budget_stop;
	uint32_t pressure_stop;
	uint32_t window_stop;
	uint32_t cancelled;
	uint32_t range_limit;
};

int dlp_metadata_generation(const struct dlp_metadata *metadata,
			    uint64_t *generation);
int dlp_publish_profile(const char *root, const struct dlp_metadata *metadata,
			const struct dlp_snapshot_ops *operations);
int dlp_invalidate_profile(const char *root, uint32_t user_id,
			   const char *package);
int dlp_sweep_profile_store(const char *root);
int dlp_read_profile(const char *path, struct dlp_profile_header *header,
		     uint8_t **owned);
int dlp_replay_profile(const char *root, const struct dlp_metadata *metadata,
		       const struct dlp_replay_callbacks *callbacks,
		       struct dlp_replay_result *result);

#endif
