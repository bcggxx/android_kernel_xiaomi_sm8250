/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/dagu_launch_profile.h>

#include "dagu_launch_profile_format.h"
#include "dlp_profile_store.h"
#include "dlp_sha256.h"

#ifdef __ANDROID__
#include <cutils/sockets.h>
#include <cutils/properties.h>
#endif

#define DLP_PROFILE_ROOT "/data/misc/dagu_launch_profile"

struct wire_message {
	struct dlp_wire_header header;
	const uint8_t *packet;
	size_t packet_size;
	struct dlp_metadata metadata;
	struct dlp_wire_begin begin;
};

struct connection {
	pthread_mutex_t lock;
	unsigned int refs;
	int controller;
	int cancelled;
};

struct profile_job {
	struct profile_job *next;
	struct connection *connection;
	uint8_t *packet;
	size_t packet_size;
	int snapshot_fd;
};

struct replay_job {
	struct replay_job *next;
	struct connection *connection;
	uint8_t *packet;
	size_t packet_size;
	uint64_t epoch;
	uint64_t cookie;
	uint64_t generation;
	atomic_int cancelled;
};

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static struct profile_job *queue_head, *queue_tail;
static unsigned int queue_count, queue_running;
static pthread_mutex_t replay_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t replay_cond = PTHREAD_COND_INITIALIZER;
static struct replay_job *replay_head, *replay_tail, *replay_current;
static unsigned int replay_queued;
static int dlp_exit_requested;

static int service_disabled(void)
{
#ifdef __ANDROID__
	char value[PROP_VALUE_MAX] = {0};

	if (property_get("sys.dagu_launch_profile.enabled", value, "0") <= 0)
		return 1;
	return !strcmp(value, "0");
#else
	return 0;
#endif
}

static int valid_package(const char *name, size_t length)
{
	if (!length || length > DLP_MAX_PACKAGE ||
	    (length == 1 && name[0] == '.') ||
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

static int compare_path(const struct dlp_metadata_path *left,
			const struct dlp_metadata_path *right)
{
	size_t length;
	int result;

	if (left->kind != right->kind)
		return left->kind < right->kind ? -1 : 1;
	length = left->length < right->length ? left->length : right->length;
	result = memcmp(left->path, right->path, length);
	if (result)
		return result;
	if (left->length == right->length)
		return 0;
	return left->length < right->length ? -1 : 1;
}

static int parse_metadata(const uint8_t *cursor, const uint8_t *end,
			  struct wire_message *message)
{
	const struct dlp_wire_header *header = &message->header;

	if (header->package_len > (size_t)(end - cursor))
		return -EINVAL;
	message->metadata.package = (const char *)cursor;
	message->metadata.package_len = header->package_len;
	if (!valid_package(message->metadata.package, header->package_len))
		return -EINVAL;
	message->metadata.path_count = header->path_count;
	cursor += header->package_len;
	if (header->signer_count > DLP_MAX_SIGNERS ||
	    (size_t)(end - cursor) < header->signer_count * 32U)
		return -EINVAL;
	message->metadata.signers = (const uint8_t (*)[32])cursor;
	message->metadata.signer_count = header->signer_count;
	for (uint32_t i = 1; i < header->signer_count; i++)
		if (memcmp(message->metadata.signers[i - 1],
			   message->metadata.signers[i], 32) >= 0)
			return -EINVAL;
	cursor += header->signer_count * 32U;
	if (header->path_count > DLP_MAX_PATHS)
		return -EINVAL;
	for (uint32_t i = 0; i < header->path_count; i++) {
		struct dlp_wire_path path;
		struct dlp_metadata_path *parsed = &message->metadata.paths[i];

		if ((size_t)(end - cursor) < sizeof(path))
			return -EINVAL;
		memcpy(&path, cursor, sizeof(path));
		cursor += sizeof(path);
		if (path.reserved[0] || path.reserved[1] || path.reserved[2] ||
		    path.reserved2 || !path.length || path.length >= DLP_MAX_PATH ||
		    path.kind < DLP_PATH_BASE_APK || path.kind > DLP_PATH_RESOURCE_DIR ||
		    (size_t)(end - cursor) < path.length)
			return -EINVAL;
		parsed->kind = path.kind;
		parsed->length = path.length;
		parsed->path = (const char *)cursor;
		cursor += path.length;
		if (memchr(parsed->path, '\0', parsed->length) ||
		    parsed->path[0] != '/')
			return -EINVAL;
		if (i && compare_path(&message->metadata.paths[i - 1], parsed) >= 0)
			return -EINVAL;
	}
	if (cursor != end)
		return -EINVAL;
	message->metadata.user_id = header->user_id;
	message->metadata.uid = header->uid;
	message->metadata.generation = header->metadata_generation;
	message->metadata.long_version_code = header->long_version_code;
	return 0;
}

static int parse_package_only(const uint8_t *cursor, const uint8_t *end,
			      struct wire_message *message, uint8_t op)
{
	const struct dlp_wire_header *header = &message->header;

	if (header->package_len > (size_t)(end - cursor))
		return -EINVAL;
	message->metadata.package = (const char *)cursor;
	message->metadata.package_len = header->package_len;
	if (!valid_package(message->metadata.package, header->package_len))
		return -EINVAL;
	cursor += header->package_len;
	if (op == DLP_OP_BEGIN) {
		if ((size_t)(end - cursor) != sizeof(message->begin))
			return -EINVAL;
		memcpy(&message->begin, cursor, sizeof(message->begin));
		if (message->begin.reserved || !message->begin.lease_ms ||
		    message->begin.lease_ms > 2000)
			return -EINVAL;
		cursor += sizeof(message->begin);
	}
	return cursor == end ? 0 : -EINVAL;
}

static int parse_wire(const uint8_t *packet, size_t packet_size,
		      struct wire_message *message)
{
	uint64_t generation;
	int ret;
	const uint8_t *cursor;
	const uint8_t *end;

	memset(message, 0, sizeof(*message));
	if (packet_size < sizeof(message->header) || packet_size > DLP_WIRE_MAX_PACKET)
		return -EINVAL;
	memcpy(&message->header, packet, sizeof(message->header));
	if (message->header.magic != DLP_WIRE_MAGIC ||
	    message->header.version != DLP_WIRE_VERSION ||
	    message->header.total_size != packet_size ||
	    message->header.reserved || !message->header.epoch)
		return -EINVAL;
	if (message->header.op < DLP_OP_PREPARE ||
	    message->header.op > DLP_OP_OFF)
		return -ENOTTY;
	message->packet = packet;
	message->packet_size = packet_size;
	cursor = packet + sizeof(message->header);
	end = packet + packet_size;
	switch (message->header.op) {
	case DLP_OP_PREPARE:
	case DLP_OP_FINISH:
		if (message->header.flags)
			return -EINVAL;
		ret = parse_metadata(cursor, end, message);
		if (ret)
			return ret;
		if (!message->metadata.signer_count)
			return -EINVAL;
		if (dlp_metadata_generation(&message->metadata, &generation) ||
		    generation != message->header.metadata_generation)
			return -EINVAL;
		return 0;
	case DLP_OP_BEGIN:
		if (message->header.flags || !message->header.metadata_generation ||
		    message->header.signer_count || message->header.path_count)
			return -EINVAL;
		return parse_package_only(cursor, end, message, DLP_OP_BEGIN);
	case DLP_OP_ABORT:
	case DLP_OP_OFF:
		if (message->header.flags || message->header.package_len ||
		    message->header.path_count || message->header.signer_count ||
		    cursor != end)
			return -EINVAL;
		return 0;
	case DLP_OP_INVALIDATE:
		if ((message->header.flags & 0xffff0000U) ||
		    (message->header.flags & 0xffU) < DLP_INVALIDATE_PACKAGE ||
		    (message->header.flags & 0xffU) > DLP_INVALIDATE_USER ||
		    (message->header.flags >> 8) < DLP_INVALIDATE_REASON_UPDATED ||
		    (message->header.flags >> 8) > DLP_INVALIDATE_REASON_USER_REMOVED ||
		    message->header.signer_count || message->header.path_count)
			return -EINVAL;
		if ((message->header.flags & 0xffU) == DLP_INVALIDATE_USER) {
			if (message->header.package_len || cursor != end)
				return -EINVAL;
			return 0;
		}
		return parse_package_only(cursor, end, message, DLP_OP_INVALIDATE);
	default:
		return -ENOTTY;
	}
}

static struct connection *connection_get(struct connection *connection)
{
	pthread_mutex_lock(&connection->lock);
	connection->refs++;
	pthread_mutex_unlock(&connection->lock);
	return connection;
}

static void connection_put(struct connection *connection)
{
	int close_controller = -1;
	int destroy = 0;

	pthread_mutex_lock(&connection->lock);
	if (!--connection->refs) {
		close_controller = connection->controller;
		connection->controller = -1;
		destroy = 1;
	}
	pthread_mutex_unlock(&connection->lock);
	if (close_controller >= 0)
		close(close_controller);
	if (destroy) {
		pthread_mutex_destroy(&connection->lock);
		free(connection);
	}
}

static void connection_cancel(struct connection *connection)
{
	pthread_mutex_lock(&connection->lock);
	connection->cancelled = 1;
	pthread_mutex_unlock(&connection->lock);
}

static int connection_cancelled(void *context)
{
	struct profile_job *job = context;
	int cancelled;

	pthread_mutex_lock(&job->connection->lock);
	cancelled = job->connection->cancelled;
	pthread_mutex_unlock(&job->connection->lock);
	return cancelled;
}

static int replay_cancelled(void *context)
{
	struct replay_job *job = context;
	int connection_cancelled_now;

	if (atomic_load(&job->cancelled))
		return 1;
	pthread_mutex_lock(&job->connection->lock);
	connection_cancelled_now = job->connection->cancelled;
	pthread_mutex_unlock(&job->connection->lock);
	return connection_cancelled_now;
}

static void replay_job_free(struct replay_job *job)
{
	connection_put(job->connection);
	free(job->packet);
	free(job);
}

static void *replay_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct replay_job *job;
		struct wire_message message;
		struct dlp_replay_callbacks callbacks = {
			.cancelled = replay_cancelled,
		};
		struct dlp_replay_result result;
		int ret;

		pthread_mutex_lock(&replay_lock);
		while (!replay_head)
			pthread_cond_wait(&replay_cond, &replay_lock);
		job = replay_head;
		replay_head = job->next;
		if (!replay_head)
			replay_tail = NULL;
		replay_queued--;
		replay_current = job;
		pthread_mutex_unlock(&replay_lock);
		callbacks.context = job;
		memset(&result, 0, sizeof(result));
		ret = parse_wire(job->packet, job->packet_size, &message) ?
			-EINVAL :
			dlp_replay_profile(DLP_PROFILE_ROOT, &message.metadata,
					   &callbacks, &result);
		fprintf(stderr,
			"dagu_launch_profile: replay cookie=%llu ret=%d requested=%llu completed=%llu files=%u ranges=%u short=%u errors=%u invalid=%u budget=%u pressure=%u window=%u cancelled=%u range_limit=%u\n",
			(unsigned long long)job->cookie, ret,
			(unsigned long long)result.read_requested,
			(unsigned long long)result.read_completed,
			result.files_attempted, result.ranges_attempted,
			result.short_reads, result.errors, result.profile_invalid,
			result.budget_stop, result.pressure_stop, result.window_stop,
			result.cancelled, result.range_limit);
		pthread_mutex_lock(&replay_lock);
		replay_current = NULL;
		pthread_cond_broadcast(&replay_cond);
		pthread_mutex_unlock(&replay_lock);
		replay_job_free(job);
	}
	return NULL;
}

static void replay_cancel_all(void)
{
	pthread_mutex_lock(&replay_lock);
	if (replay_current)
		atomic_store(&replay_current->cancelled, 1);
	for (struct replay_job *job = replay_head; job; job = job->next)
		atomic_store(&job->cancelled, 1);
	pthread_mutex_unlock(&replay_lock);
}

static void replay_cancel(uint64_t epoch, uint64_t cookie)
{
	pthread_mutex_lock(&replay_lock);
	if (replay_current && replay_current->epoch == epoch &&
	    replay_current->cookie == cookie)
		atomic_store(&replay_current->cancelled, 1);
	for (struct replay_job *job = replay_head; job; job = job->next)
		if (job->epoch == epoch && job->cookie == cookie)
			atomic_store(&job->cancelled, 1);
	pthread_mutex_unlock(&replay_lock);
}

static void replay_wait_idle(void)
{
	pthread_mutex_lock(&replay_lock);
	while (replay_current || replay_head)
		pthread_cond_wait(&replay_cond, &replay_lock);
	pthread_mutex_unlock(&replay_lock);
}

static int replay_start(struct connection *connection, const uint8_t *packet,
			size_t packet_size, uint64_t epoch, uint64_t cookie,
			uint64_t generation)
{
	struct replay_job *job;
	struct replay_job *dropped = NULL;
	int ret = 0;

	pthread_mutex_lock(&replay_lock);
	if (replay_current && replay_current->epoch == epoch &&
	    replay_current->cookie == cookie &&
	    replay_current->generation == generation) {
		pthread_mutex_unlock(&replay_lock);
		return 0;
	}
	for (struct replay_job *queued = replay_head; queued;
	     queued = queued->next) {
		if (queued->epoch == epoch && queued->cookie == cookie &&
		    queued->generation == generation) {
			pthread_mutex_unlock(&replay_lock);
			return 0;
		}
	}
	if (replay_current)
		atomic_store(&replay_current->cancelled, 1);
	dropped = replay_head;
	replay_head = NULL;
	replay_tail = NULL;
	replay_queued = 0;
	pthread_mutex_unlock(&replay_lock);
	while (dropped) {
		struct replay_job *next = dropped->next;

		replay_job_free(dropped);
		dropped = next;
	}
	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->packet = malloc(packet_size);
	if (!job->packet) {
		free(job);
		return -ENOMEM;
	}
	memcpy(job->packet, packet, packet_size);
	job->packet_size = packet_size;
	job->connection = connection_get(connection);
	job->epoch = epoch;
	job->cookie = cookie;
	job->generation = generation;
	atomic_init(&job->cancelled, 0);
	pthread_mutex_lock(&replay_lock);
	if (replay_tail)
		replay_tail->next = job;
	else
		replay_head = job;
	replay_tail = job;
	replay_queued++;
	pthread_cond_signal(&replay_cond);
	pthread_mutex_unlock(&replay_lock);
	return ret;
}

static int job_snapshot(void *context, int *snapshot_fd)
{
	struct profile_job *job = context;
	int duplicate;

	if (job->snapshot_fd < 0) {
		errno = EBADF;
		return -1;
	}
	duplicate = dup(job->snapshot_fd);
	if (duplicate < 0)
		return -1;
	*snapshot_fd = duplicate;
	return 0;
}

static int job_export(void *context, uint32_t index, int *file_fd)
{
	struct profile_job *job = context;
	struct wire_message message;
	struct dlp_request request = {
		.version = 1,
		.size = sizeof(request),
		.op = DLP_EXPORT_FILE,
		.index = index,
	};
	int ret;

	pthread_mutex_lock(&job->connection->lock);
	if (job->connection->cancelled) {
		pthread_mutex_unlock(&job->connection->lock);
		errno = ECANCELED;
		return -1;
	}
	if (parse_wire(job->packet, job->packet_size, &message)) {
		pthread_mutex_unlock(&job->connection->lock);
		errno = EINVAL;
		return -1;
	}
	request.epoch = message.header.epoch;
	request.cookie = message.header.cookie;
	ret = ioctl(job->connection->controller, DLP_IOCTL, &request);
	pthread_mutex_unlock(&job->connection->lock);
	if (ret < 0)
		return -1;
	*file_fd = ret;
	return 0;
}

static void *profile_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct profile_job *job;
		struct wire_message message;
		struct dlp_snapshot_ops operations = {
			.snapshot = job_snapshot,
			.export_file = job_export,
			.cancelled = connection_cancelled,
		};

		pthread_mutex_lock(&queue_lock);
		while (!queue_head)
			pthread_cond_wait(&queue_cond, &queue_lock);
		job = queue_head;
		queue_head = job->next;
		if (!queue_head)
			queue_tail = NULL;
		queue_count--;
		queue_running++;
		pthread_mutex_unlock(&queue_lock);
		operations.context = job;
		if (!parse_wire(job->packet, job->packet_size, &message) &&
		    !dlp_publish_profile(DLP_PROFILE_ROOT, &message.metadata, &operations))
			fprintf(stderr,
				"dagu_launch_profile: published user=%u package=%.*s generation=%llu\n",
				message.metadata.user_id, message.metadata.package_len,
				message.metadata.package,
				(unsigned long long)message.metadata.generation);
		else
			fprintf(stderr,
				"dagu_launch_profile: publish rejected errno=%d\n", errno);
		connection_put(job->connection);
		if (job->snapshot_fd >= 0)
			close(job->snapshot_fd);
		free(job->packet);
		free(job);
		pthread_mutex_lock(&queue_lock);
		queue_running--;
		pthread_cond_broadcast(&queue_cond);
		pthread_mutex_unlock(&queue_lock);
	}
	return NULL;
}

static int queue_job(struct connection *connection, const uint8_t *packet,
		     size_t packet_size, int snapshot_fd)
{
	struct profile_job *job;
	int ret = 0;

	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->packet = malloc(packet_size);
	if (!job->packet) {
		free(job);
		return -ENOMEM;
	}
	memcpy(job->packet, packet, packet_size);
	job->packet_size = packet_size;
	job->snapshot_fd = snapshot_fd;
	job->connection = connection_get(connection);
	pthread_mutex_lock(&queue_lock);
	if (queue_count || queue_running) {
		ret = -ENOSPC;
	} else {
		if (queue_tail)
			queue_tail->next = job;
		else
			queue_head = job;
		queue_tail = job;
		queue_count++;
		pthread_cond_signal(&queue_cond);
	}
	pthread_mutex_unlock(&queue_lock);
	if (ret) {
		connection_put(job->connection);
		close(job->snapshot_fd);
		free(job->packet);
		free(job);
	}
	return ret;
}

static void wait_jobs(void)
{
	pthread_mutex_lock(&queue_lock);
	while (queue_count || queue_running)
		pthread_cond_wait(&queue_cond, &queue_lock);
	pthread_mutex_unlock(&queue_lock);
}

static int receive(int fd, uint8_t *packet, size_t capacity, size_t *packet_size,
		   int *received_fd)
{
	char control[CMSG_SPACE(sizeof(int) * 8)];
	struct iovec iov = { .iov_base = packet, .iov_len = capacity };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};
	ssize_t size = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
	unsigned int count = 0;

	*received_fd = -1;
	for (struct cmsghdr *header = CMSG_FIRSTHDR(&msg); size >= 0 && header;
	     header = CMSG_NXTHDR(&msg, header)) {
		if (header->cmsg_level != SOL_SOCKET ||
		    header->cmsg_type != SCM_RIGHTS ||
		    header->cmsg_len < CMSG_LEN(0))
			continue;
		size_t bytes = header->cmsg_len - CMSG_LEN(0);
		int *fds = (int *)CMSG_DATA(header);

		for (size_t i = 0; i < bytes / sizeof(int); i++) {
			if (!count++)
				*received_fd = fds[i];
			else
				close(fds[i]);
		}
	}
	if (size <= 0 || (size_t)size > capacity ||
	    (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
		if (*received_fd >= 0)
			close(*received_fd);
		*received_fd = -1;
		return -1;
	}
	*packet_size = (size_t)size;
	if (count > 1) {
		close(*received_fd);
		*received_fd = -1;
		return -1;
	}
	return 0;
}

static int send_ack(int fd, const struct wire_message *message, int32_t status)
{
	struct dlp_wire_ack ack = {
		.magic = DLP_WIRE_MAGIC,
		.version = DLP_WIRE_VERSION,
		.size = sizeof(ack),
		.op = message->header.op,
		.status = status,
		.epoch = message->header.epoch,
		.cookie = message->header.cookie,
	};

	return send(fd, &ack, sizeof(ack), MSG_NOSIGNAL) == sizeof(ack) ? 0 : -1;
}

static int authorized(int fd)
{
	struct ucred cred;
	socklen_t length = sizeof(cred);
	char context[256];

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) || cred.uid != 1000)
		return 0;
	length = sizeof(context) - 1;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERSEC, context, &length) ||
	    length >= sizeof(context))
		return 0;
	context[length] = '\0';
	return !strcmp(context, "u:r:system_server:s0");
}

static struct dlp_request kernel_request(const struct wire_message *message,
					 uint32_t op)
{
	struct dlp_request request = {
		.version = 1,
		.size = sizeof(request),
		.op = op,
		.epoch = message->header.epoch,
		.cookie = message->header.cookie,
	};

	return request;
}

static int invalidate(const struct wire_message *message)
{
	uint8_t kind = message->header.flags & 0xffU;
	char package[DLP_MAX_PACKAGE + 1];

	if (kind == DLP_INVALIDATE_USER)
		return dlp_invalidate_profile(DLP_PROFILE_ROOT, message->header.user_id,
					      NULL);
	if (!message->metadata.package_len ||
	    message->metadata.package_len > DLP_MAX_PACKAGE)
		return -1;
	memcpy(package, message->metadata.package, message->metadata.package_len);
	package[message->metadata.package_len] = '\0';
	return dlp_invalidate_profile(DLP_PROFILE_ROOT, message->header.user_id,
				      package);
}

static void serve(int peer)
{
	struct connection *connection = NULL;
	struct wire_message prepared;
	uint8_t *prepared_packet = NULL;
	uint8_t *packet = NULL;
	int prepared_active = 0, kernel_active = 0, epoch_set = 0;
	struct timeval timeout = { .tv_sec = 1 };
	uint64_t epoch = 0;

	if (!authorized(peer))
		goto out;
	if (setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
	    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
		goto out;
	connection = calloc(1, sizeof(*connection));
	packet = malloc(DLP_WIRE_MAX_PACKET);
	if (!connection || !packet)
		goto out;
	pthread_mutex_init(&connection->lock, NULL);
	connection->refs = 1;
	connection->controller = open("/dev/dagu_launch_profile",
				      O_RDWR | O_CLOEXEC);
	if (connection->controller < 0)
		goto out;
	struct dlp_request enable = {
		.version = 1,
		.size = sizeof(enable),
		.op = DLP_ENABLE,
		.flags = 1,
		.epoch = epoch,
	};
	if (ioctl(connection->controller, DLP_IOCTL, &enable))
		goto out;
	for (;;) {
		struct pollfd poll_fd = { .fd = peer, .events = POLLIN };
		int ready = poll(&poll_fd, 1, 1000);
		size_t packet_size;
		int received_fd, ret;
		struct wire_message message;

		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0 || (poll_fd.revents & (POLLHUP | POLLERR | POLLNVAL)))
			break;
		if (!(poll_fd.revents & POLLIN)) {
			if (service_disabled())
				break;
			continue;
		}
		if (receive(peer, packet, DLP_WIRE_MAX_PACKET, &packet_size,
			    &received_fd))
			break;
		ret = parse_wire(packet, packet_size, &message);
		if (ret) {
			memset(&message, 0, sizeof(message));
			if (packet_size >= sizeof(message.header)) {
				const struct dlp_wire_header *header =
					(const struct dlp_wire_header *)packet;

				message.header = *header;
			}
			if (received_fd >= 0)
				close(received_fd);
			if (send_ack(peer, &message, ret))
				break;
			continue;
		}
		if (!epoch_set) {
			epoch = message.header.epoch;
			enable.epoch = epoch;
			if (ioctl(connection->controller, DLP_IOCTL, &enable)) {
				ret = -errno;
				if (received_fd >= 0)
					close(received_fd);
				if (send_ack(peer, &message, ret))
					break;
				continue;
			}
			epoch_set = 1;
		} else if (message.header.epoch != epoch) {
			if (received_fd >= 0)
				close(received_fd);
			if (send_ack(peer, &message, -ESRCH))
				break;
			continue;
		}
		switch (message.header.op) {
		case DLP_OP_PREPARE:
			if (received_fd >= 0) {
				close(received_fd);
				ret = -EINVAL;
				break;
			}
			if (kernel_active) {
				ret = -EBUSY;
				break;
			}
			free(prepared_packet);
			prepared_packet = malloc(packet_size);
			if (!prepared_packet) {
				ret = -ENOMEM;
				break;
			}
			memcpy(prepared_packet, packet, packet_size);
			if (parse_wire(prepared_packet, packet_size, &prepared)) {
				free(prepared_packet);
				prepared_packet = NULL;
				prepared_active = 0;
				ret = -EINVAL;
				break;
			}
			prepared_active = 1;
			ret = 0;
			if (replay_start(connection, packet, packet_size, epoch,
					 message.header.cookie,
					 message.header.metadata_generation))
				fprintf(stderr,
					"dagu_launch_profile: replay enqueue skipped\n");
			break;
		case DLP_OP_BEGIN:
			if (received_fd < 0 || !prepared_active || kernel_active) {
				ret = received_fd < 0 ? -EINVAL : -EBUSY;
				if (received_fd >= 0)
					close(received_fd);
				break;
			}
			if (message.header.cookie != prepared.header.cookie ||
			    message.header.user_id != prepared.header.user_id ||
			    message.header.uid != prepared.header.uid ||
			    message.header.metadata_generation !=
				    prepared.header.metadata_generation ||
			    message.header.long_version_code !=
				    prepared.header.long_version_code ||
			    message.metadata.package_len != prepared.metadata.package_len ||
			    memcmp(message.metadata.package, prepared.metadata.package,
				   message.metadata.package_len)) {
				close(received_fd);
				ret = -ESTALE;
				break;
			}
			struct dlp_request begin = kernel_request(&message, DLP_BEGIN);

			begin.binder_fd = received_fd;
			begin.binder_handle = message.begin.binder_handle;
			begin.tid = message.begin.tid;
			begin.uid = message.header.uid;
			begin.lease_ms = message.begin.lease_ms;
			ret = ioctl(connection->controller, DLP_IOCTL, &begin) ?
				-errno : 0;
			close(received_fd);
			if (!ret)
				kernel_active = 1;
			break;
		case DLP_OP_FINISH:
			if (received_fd >= 0) {
				close(received_fd);
				ret = -EINVAL;
				break;
			}
			if (!prepared_active || !kernel_active ||
			    message.header.cookie != prepared.header.cookie ||
			    message.header.user_id != prepared.header.user_id ||
			    message.header.uid != prepared.header.uid ||
			    message.header.metadata_generation !=
				    prepared.header.metadata_generation) {
				ret = -ESTALE;
				break;
			}
			struct dlp_request finish = kernel_request(&message, DLP_FINISH);
			struct dlp_request snapshot_request =
				kernel_request(&message, DLP_SNAPSHOT);
			int snapshot_fd = -1;

			ret = ioctl(connection->controller, DLP_IOCTL, &finish) ?
				-errno : 0;
			kernel_active = 0;
			if (!ret)
				snapshot_fd = ioctl(connection->controller, DLP_IOCTL,
						    &snapshot_request);
			if (!ret && snapshot_fd < 0)
				ret = -errno;
			if (!ret)
				ret = queue_job(connection, packet, packet_size,
						snapshot_fd);
			replay_cancel(message.header.epoch, message.header.cookie);
			if (!ret) {
				free(prepared_packet);
				prepared_packet = NULL;
				prepared_active = 0;
			}
			break;
		case DLP_OP_ABORT:
			if (received_fd >= 0) {
				close(received_fd);
				ret = -EINVAL;
				break;
			}
			replay_cancel(message.header.epoch, message.header.cookie);
			if (kernel_active &&
			    message.header.cookie != prepared.header.cookie) {
				ret = -ESRCH;
				break;
			}
			if (kernel_active) {
				struct dlp_request abort = kernel_request(&message, DLP_ABORT);

				ret = ioctl(connection->controller, DLP_IOCTL, &abort) ?
					-errno : 0;
				if (ret == -ESRCH)
					ret = 0;
				if (!ret)
					kernel_active = 0;
			} else {
				ret = prepared_active &&
				      message.header.cookie != prepared.header.cookie ?
				      -ESRCH : 0;
			}
			if (!ret) {
				free(prepared_packet);
				prepared_packet = NULL;
				prepared_active = 0;
			}
			break;
		case DLP_OP_INVALIDATE:
			if (received_fd >= 0) {
				close(received_fd);
				ret = -EINVAL;
				break;
			}
			/* A prior FINISH publication is serialized before invalidation so
			 * the final visible state cannot be repopulated by the worker.
			 */
			wait_jobs();
			replay_cancel_all();
			ret = invalidate(&message) ? -errno : 0;
			break;
		case DLP_OP_OFF:
			if (received_fd >= 0) {
				close(received_fd);
				ret = -EINVAL;
				break;
			}
			wait_jobs();
			replay_cancel_all();
			replay_wait_idle();
			enable.flags = 0;
			enable.epoch = epoch;
			ret = ioctl(connection->controller, DLP_IOCTL, &enable) ?
				-errno : 0;
			kernel_active = 0;
			free(prepared_packet);
			prepared_packet = NULL;
			prepared_active = 0;
			if (send_ack(peer, &message, ret))
				goto out;
			dlp_exit_requested = 1;
			goto out;
		default:
			if (received_fd >= 0)
				close(received_fd);
			ret = -ENOTTY;
			break;
		}
		if (send_ack(peer, &message, ret))
			break;
	}
out:
	if (connection) {
		if (kernel_active) {
			struct dlp_request abort = {
				.version = 1,
				.size = sizeof(abort),
				.op = DLP_ABORT,
			};
			(void)ioctl(connection->controller, DLP_IOCTL, &abort);
		}
		connection_cancel(connection);
		connection_put(connection);
	}
	free(prepared_packet);
	free(packet);
	close(peer);
}

int main(void)
{
	int listener;
	pthread_t worker, replay;

	if (dlp_sweep_profile_store(DLP_PROFILE_ROOT))
		fprintf(stderr, "dagu_launch_profile: profile sweep failed: %s\n",
			strerror(errno));
	if (pthread_create(&worker, NULL, profile_worker, NULL)) {
		fprintf(stderr, "dagu_launch_profile: cannot start profile worker\n");
		return 1;
	}
	pthread_detach(worker);
	if (pthread_create(&replay, NULL, replay_worker, NULL)) {
		fprintf(stderr, "dagu_launch_profile: cannot start replay worker\n");
		return 1;
	}
	pthread_detach(replay);
#ifdef __ANDROID__
	listener = android_get_control_socket("dagu_launch_profile");
#else
	listener = -1;
#endif
	if (listener < 0 || listen(listener, 1)) {
		fprintf(stderr, "dagu_launch_profile: missing init socket\n");
		return 1;
	}
	for (;;) {
		struct pollfd listener_poll = {
			.fd = listener,
			.events = POLLIN,
		};
		int ready;
		int peer;

		if (dlp_exit_requested || service_disabled())
			break;
		ready = poll(&listener_poll, 1, 1000);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			break;
		if (!ready)
			continue;
		if (!(listener_poll.revents & POLLIN))
			break;
		peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		if (peer < 0 && errno == EINTR)
			continue;
		if (peer < 0)
			return 1;
		serve(peer);
	}
	replay_cancel_all();
	replay_wait_idle();
	wait_jobs();
	close(listener);
	return 0;
}
