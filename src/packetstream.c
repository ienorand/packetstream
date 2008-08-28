/**
 * \file src/packetstream.c
 * \brief thread-safe ring buffer
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007-2008
 * For conditions of distribution and use, see copyright notice in packetstream.h
 */

#include "packetstream.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef __PS_SHM
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

/**
 * \addtogroup packetstream
 *  \{
 */

/* TODO check that buffer or packet is not NULL */
#define __PS_BUFFER_CHECK(buffer) \
	{ \
		int __ps_check_int = ps_buffer_check(buffer); \
		if (__ps_check_int) { return __ps_check_int; } \
	}
#define __PS_BUFFER_VARS(buffer) \
	struct ps_state_s *state = (struct ps_state_s *) buffer->state;
#define __PS_BUFFER(buffer) \
	__PS_BUFFER_VARS(buffer) \
	__PS_BUFFER_CHECK(buffer)
#define __PS_PACKET_CHECK(packet) \
	{ \
		int __ps_check_int = ps_packet_check(packet); \
		if (__ps_check_int) { return __ps_check_int; } \
	}
#define __PS_PACKET_VARS(packets) \
	ps_buffer_t *buffer = packet->buffer; \
	struct ps_state_s *state = (struct ps_state_s *) buffer->state; \
	struct ps_packet_header_s *header = (struct ps_packet_header_s *) packet->header;
#define __PS_PACKET(packet) \
	__PS_PACKET_VARS(packet) \
	__PS_PACKET_CHECK(packet)
#define __PS_CHECK_CANCEL_READ(state) \
	if (state->flags & PS_BUFFER_CANCELLED) { \
		pthread_mutex_unlock(&state->read_mutex); \
		return EINTR; \
	}
#define __PS_CHECK_CANCEL_WRITE(state) \
	if (state->flags & PS_BUFFER_CANCELLED) { \
		pthread_mutex_unlock(&state->write_mutex); \
		return EINTR; \
	}

/**
 * \ingroup buffer
 * \brief internal buffer state
 */
struct ps_state_s {
	/** flags */
	ps_flags_t flags;
	/** buffer size */
	size_t size;
	/** position of the first packet opened for reading or next
	 *  packet to be read if there is no open (read) packets */
	size_t read_pos;
	/** position of the first packet opened for writing or next
	 * packet to be written if there is no open (write) packets */
	size_t write_pos;
	/** position of the next packet to be read */
	size_t read_next;
	/** position of the next packet to be written */
	size_t write_next;
	/** the first written (possibly also read) packet that has
	 * not been free'd */
	size_t read_first;
	/** free bytes */
	long free_bytes;
	/** mutex for ps_buffer_openread() */
	pthread_mutex_t read_mutex;
	/** mutex for ps_buffer_openwrite()...ps_buffer_setsize() */
	pthread_mutex_t write_mutex;
	/** mutex for ps_buffer_closeread() */
	pthread_mutex_t read_close_mutex;
	/** mutex for ps_buffer_closewrite() */
	pthread_mutex_t write_close_mutex;
	/** number of consumed packets */
	sem_t read_packets;
	/** number of produced packets */
	sem_t written_packets;
#ifndef WIN32
	/** absolute time (since EPOCH) when this buffer was created */
	struct timeval create_time;
#endif
};

/**
 * \ingroup packet
 * \brief packet header
 */
struct ps_packet_header_s {
	/** flags */
	ps_flags_t flags;
	/** packet size (excluding header) in bytes */
	size_t size;
};

/**
 * \addtogroup packet
 *  \{
 */

/**
 * \brief fake dma object
 */
struct ps_fake_dma_s {
	/** temporary memory area */
	void *mem;
	/** dma area size */
	size_t mem_size;
	/** dma area in use size */
	size_t size;
	/** position in packet */
	size_t pos;
	/** is this free */
	int free;
	/** next item in list */
	struct ps_fake_dma_s *next;
};

/** packet is written to buffer */
#define PS_PACKET_HEADER_WRITTEN 1
/** packet is read from buffer */
#define PS_PACKET_HEADER_READ    2

/**  \} */

__inline__ static int ps_packet_check(ps_packet_t *packet);
__inline__ static int ps_buffer_check(ps_buffer_t *buffer);

int ps_packet_openread(ps_packet_t *packet, ps_flags_t flags);
int ps_packet_openwrite(ps_packet_t *packet, ps_flags_t flags);

int ps_packet_closeread(ps_packet_t *packet);
int ps_packet_closewrite(ps_packet_t *packet);

int ps_packet_reserve(ps_packet_t *packet, size_t len);

int ps_packet_fakedma_alloc(ps_packet_t *packet, struct ps_fake_dma_s **fake_dma, size_t size);
int ps_packet_fakedma_free(ps_packet_t *packet, struct ps_fake_dma_s *fake_dma);
int ps_packet_fakedma_cut(ps_packet_t *packet, size_t size);
int ps_packet_fakedma_commitall(ps_packet_t *packet);
int ps_packet_fakedma_freeall(ps_packet_t *packet);

unsigned long ps_buffer_utime(ps_buffer_t *buffer);

int ps_buffer_init(ps_buffer_t *buffer, ps_bufferattr_t *attr)
{
	/* 12.35 neon-green midgets will rip out your lungs and laugh at you
	   if you dare to assume that this is a thread-safe function !!! */

	struct ps_state_s *state;
	size_t stats_size = 0;
	int shared = 0;
	ps_flags_t flags = attr->flags;
	int shmid = attr->shmid;
	pthread_mutexattr_t mutexattr;

	if (buffer == NULL)
		return EINVAL;

	memset(buffer, 0, sizeof(ps_buffer_t));

	pthread_mutexattr_init(&mutexattr);

#ifdef __PS_SHM
	if (flags & PS_BUFFER_PSHARED) {
		shared = 1;
		pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED);

		if (flags & PS_BUFFER_STATS)
			stats_size = sizeof(ps_stats_t);

		if (attr->shmid == PS_SHM_CREATE)
			shmid = shmget(IPC_PRIVATE, attr->size + sizeof(struct ps_state_s) + stats_size, IPC_CREAT | IPC_EXCL | attr->shmmode);
		else
			flags |= PS_BUFFER_READY;

		if (shmid == -1)
			return errno;

		buffer->state = shmat(shmid, NULL, 0);

		if (buffer->state == (void *) (-1))
			return errno;

		buffer->buffer = &((unsigned char *) buffer->state)[sizeof(struct ps_state_s) + stats_size];
		if (flags & PS_BUFFER_STATS)
			buffer->stats = (ps_stats_t *) &((unsigned char *) buffer->state)[sizeof(struct ps_state_s)];
	} else {
#endif
		buffer->state = malloc(sizeof(struct ps_state_s));
		buffer->buffer = malloc(attr->size);
		if (flags & PS_BUFFER_STATS)
			buffer->stats = (ps_stats_t *) malloc(sizeof(ps_stats_t));
#ifdef __PS_SHM
	}
#endif

	if ((buffer->buffer == NULL) | (buffer->state == NULL))
		return ENOMEM;

	if ((flags & PS_BUFFER_STATS) && (buffer->stats == NULL))
		return ENOMEM;

	if (flags & PS_BUFFER_READY)
		return 0;

	memset(buffer->buffer, 0, attr->size);
	memset(buffer->state, 0, sizeof(struct ps_state_s));
	if (flags & PS_BUFFER_STATS)
		memset(buffer->stats, 0, sizeof(ps_stats_t));

	state = (struct ps_state_s *) buffer->state;

	state->size = attr->size;
	state->flags = flags;
	state->free_bytes = attr->size - sizeof(struct ps_packet_header_s);
	buffer->shmid = shmid;

	/* TODO should we check for errors? */
	pthread_mutex_init(&state->read_mutex, &mutexattr);
	pthread_mutex_init(&state->write_mutex, &mutexattr);

	pthread_mutex_init(&state->read_close_mutex, &mutexattr);
	pthread_mutex_init(&state->write_close_mutex, &mutexattr);

	sem_init(&state->read_packets, shared, 0);
	sem_init(&state->written_packets, shared, 0);

	pthread_mutexattr_destroy(&mutexattr);

	gettimeofday(&state->create_time, NULL);

	state->flags |= PS_BUFFER_READY;

	return 0;
}

int ps_buffer_destroy(ps_buffer_t *buffer)
{
	__PS_BUFFER_VARS(buffer)

	/* TODO make sure there is no open packets
	        and free stuff only if there is 0 active
	        progs/threads using this buffer */

	pthread_mutex_destroy(&state->read_mutex);
	pthread_mutex_destroy(&state->write_mutex);

	pthread_mutex_destroy(&state->read_close_mutex);
	pthread_mutex_destroy(&state->write_close_mutex);

	sem_destroy(&state->read_packets);
	sem_destroy(&state->written_packets);

	if (state->flags & PS_BUFFER_PSHARED) {
		shmdt(buffer->state);
		shmctl(buffer->shmid, IPC_RMID, 0);
	} else {
		if (state->flags & PS_BUFFER_STATS)
			free(buffer->stats);
		free(buffer->buffer);
		free(state);
	}

	return 0;
}

int ps_packet_init(ps_packet_t *packet, ps_buffer_t *buffer)
{
	__PS_BUFFER_CHECK(buffer)
	packet->buffer = buffer;
	packet->fake_dma = NULL;
	return 0;
}

int ps_packet_destroy(ps_packet_t *packet)
{
	struct ps_fake_dma_s *fake_dma, *del;

	fake_dma = (struct ps_fake_dma_s *) packet->fake_dma;
	while (fake_dma != NULL) {
		del = fake_dma;
		fake_dma = (struct ps_fake_dma_s *) fake_dma->next;
		if (del->mem)
			free(del->mem);
		free(del);
	}
	packet->fake_dma = NULL;

	packet->buffer = NULL;
	return 0;
}

int ps_buffer_stats(ps_buffer_t *buffer, ps_stats_t *stats)
{
	__PS_BUFFER_VARS(buffer)

	if (!(state->flags & PS_BUFFER_STATS))
		return ENOTSUP;

	memcpy(stats, buffer->stats, sizeof(ps_stats_t));
	stats->utime = ps_buffer_utime(buffer);

	return 0;
}

int ps_packet_open(ps_packet_t *packet, ps_flags_t flags)
{
	__PS_BUFFER_CHECK(packet->buffer)

	if (!(flags & PS_PACKET_READ || flags & PS_PACKET_WRITE))
		return EINVAL;

	if (flags & PS_PACKET_READ)
		return ps_packet_openread(packet, flags);
	else
		return ps_packet_openwrite(packet, flags);
}

int ps_packet_openread(ps_packet_t *packet, ps_flags_t flags)
{
	__PS_BUFFER_VARS(packet->buffer)
	ps_buffer_t *buffer = packet->buffer;
	struct ps_packet_header_s *header;

	if (flags & PS_PACKET_TRY) {
		if (pthread_mutex_trylock(&state->read_mutex))
			return EBUSY;
	} else if (pthread_mutex_lock(&state->read_mutex))
		return EINVAL;
	__PS_CHECK_CANCEL_READ(state)

	if (state->flags & PS_BUFFER_STATS)
		buffer->read_wait_start = ps_buffer_utime(buffer);

	if (flags & PS_PACKET_TRY) {
		if (sem_trywait(&state->written_packets)) {
			pthread_mutex_unlock(&state->read_mutex);
			return EBUSY;
		}
	} else if (sem_wait(&state->written_packets)) {
		pthread_mutex_unlock(&state->read_mutex);
		return EINVAL;
	}
	__PS_CHECK_CANCEL_READ(state)

	if (state->flags & PS_BUFFER_STATS)
		buffer->stats->read_wait_usec += ps_buffer_utime(buffer) - buffer->read_wait_start;

	packet->flags = flags & ~PS_PACKET_TRY;
	packet->buffer_pos = state->read_next;
	packet->header = &buffer->buffer[packet->buffer_pos];
	packet->pos = 0;

	header = (struct ps_packet_header_s *) packet->header;

	state->read_next = (sizeof(struct ps_packet_header_s) + state->read_next + header->size) % state->size;
	if (state->read_next + sizeof(struct ps_packet_header_s) > state->size)
		state->read_next = 0;

	pthread_mutex_unlock(&state->read_mutex);

	return 0;
}

int ps_packet_openwrite(ps_packet_t *packet, ps_flags_t flags)
{
	__PS_BUFFER_VARS(packet->buffer)
	ps_buffer_t *buffer = packet->buffer;
	struct ps_packet_header_s *header;

	if (flags & PS_PACKET_TRY) {
		if (pthread_mutex_trylock(&state->write_mutex))
			return EBUSY;
	} else if (pthread_mutex_lock(&state->write_mutex))
		return EINVAL;
	__PS_CHECK_CANCEL_WRITE(state)

	/* next header is already free, NULL & reserved */
	packet->reserved = 0;

	packet->flags = flags;
	packet->buffer_pos = state->write_next;
	packet->header = &buffer->buffer[packet->buffer_pos];
	packet->pos = 0;

	header = (struct ps_packet_header_s *) packet->header;
	header->flags = 0;
	header->size = 0;

	return 0;
}

int ps_packet_setsize(ps_packet_t *packet, size_t size)
{
	int ret;
	size_t res = 0;
	__PS_PACKET(packet)

	if ((!(packet->flags & PS_PACKET_WRITE)) | (packet->flags & PS_PACKET_SIZE_SET))
		return EINVAL;

	if (size + sizeof(struct ps_packet_header_s) * 2 > state->size)
		return ENOBUFS;

	if ((ret = ps_packet_reserve(packet, size)))
		return ret;

	header->size = size;
	packet->flags |= PS_PACKET_SIZE_SET;
	packet->flags &= ~PS_PACKET_TRY;

	state->write_next = (sizeof(struct ps_packet_header_s) + state->write_next + header->size) % state->size;
	if (state->write_next + sizeof(struct ps_packet_header_s) > state->size) {
		res = state->size - state->write_next;
		state->write_next = 0;
	}

	/* we must set next header NULL */
	if ((ret = ps_packet_reserve(packet, sizeof(struct ps_packet_header_s) + size + res)))
		return ret;
	memset(&buffer->buffer[state->write_next], 0, sizeof(struct ps_packet_header_s));

	/* free bytes */
	state->free_bytes += packet->reserved - (size + sizeof(struct ps_packet_header_s) + res);

	pthread_mutex_unlock(&state->write_mutex);

	/* cut fakedma */
	if ((ret = ps_packet_fakedma_cut(packet, size)))
		return ret; /* if this fails... */

	return 0;
}

int ps_packet_close(ps_packet_t *packet)
{
	__PS_PACKET_CHECK(packet)

	packet->flags &= ~PS_PACKET_TRY; /* too late to cancel */

	if (packet->flags & PS_PACKET_READ)
		return ps_packet_closeread(packet);
	else
		return ps_packet_closewrite(packet);
}

int ps_packet_cancel(ps_packet_t *packet)
{
	__PS_PACKET(packet)

	if (!(packet->flags & PS_PACKET_WRITE))
		return EINVAL;
	if (packet->flags & PS_PACKET_SIZE_SET)
		return EINVAL;

	state->free_bytes += packet->reserved; /* correct? */
	memset(header, 0, sizeof(struct ps_packet_header_s));
	pthread_mutex_unlock(&state->write_mutex);

	ps_packet_fakedma_freeall(packet);

	packet->header = NULL;
	packet->flags = 0;

	return 0;
}

/* NOTE len is absolute packet size, not added to current reserved */
int ps_packet_reserve(ps_packet_t *packet, size_t len)
{
	__PS_PACKET_VARS(packet)

	if (len <= packet->reserved)
		return 0;

	state->free_bytes -= len - packet->reserved;
	while (state->free_bytes < 0) {
		/* "consume" next free (=read) packet */
		if (state->flags & PS_BUFFER_STATS)
			buffer->write_wait_start = ps_buffer_utime(buffer);

		if (packet->flags & PS_PACKET_TRY) {
			if (sem_trywait(&state->read_packets)) {
				state->free_bytes += len - packet->reserved;
				return EBUSY;
			}
		} else if (sem_wait(&state->read_packets))
			return EINVAL;
		__PS_CHECK_CANCEL_WRITE(state)

		if (state->flags & PS_BUFFER_STATS)
			buffer->stats->write_wait_usec += ps_buffer_utime(buffer) - buffer->write_wait_start;

		do {
			header = (struct ps_packet_header_s *) &buffer->buffer[state->read_first];

			state->free_bytes += sizeof(struct ps_packet_header_s) + header->size;
			state->read_first = (state->read_first + sizeof(struct ps_packet_header_s) + header->size) % state->size;
			if (state->read_first + sizeof(struct ps_packet_header_s) > state->size) {
				state->free_bytes += state->size - state->read_first;
				state->read_first = 0;
			}
		} while (!sem_trywait(&state->read_packets));
	}

	packet->reserved = len;

	return 0;
}

int ps_packet_closeread(ps_packet_t *packet)
{
	__PS_PACKET_VARS(packet)
	int ret;
	size_t pos;

	if ((ret = pthread_mutex_lock(&state->read_close_mutex)))
		return ret;

	if (state->flags & PS_BUFFER_STATS) {
		buffer->stats->read_packets++;
		buffer->stats->read_bytes += header->size;
	}

	header->flags |= PS_PACKET_HEADER_READ;

	if (state->read_pos == packet->buffer_pos) {
		pos = packet->buffer_pos;

		do {
			pos = (pos + sizeof(struct ps_packet_header_s) + header->size) % state->size;
			if (pos + sizeof(struct ps_packet_header_s) > state->size)
				pos = 0;

			if (sem_post(&state->read_packets))
				return EINVAL;

			header = (struct ps_packet_header_s *) &buffer->buffer[pos];
		} while (header->flags & PS_PACKET_HEADER_READ);

		state->read_pos = pos;
	}

	pthread_mutex_unlock(&state->read_close_mutex);

	ps_packet_fakedma_freeall(packet);

	packet->header = NULL;
	packet->flags = 0;

	return 0;
}

int ps_packet_closewrite(ps_packet_t *packet)
{
	__PS_PACKET_VARS(packet)
	size_t pos;
	int ret;

	if (!(packet->flags & PS_PACKET_SIZE_SET)) {
		if ((ret = ps_packet_setsize(packet, header->size)))
			return ret;
	}

	if ((ret = ps_packet_fakedma_commitall(packet)))
		return ret;

	if ((ret = pthread_mutex_lock(&state->write_close_mutex)))
		return ret;

	if (state->flags & PS_BUFFER_STATS) {
		buffer->stats->written_packets++;
		buffer->stats->written_bytes += header->size;
	}

	header->flags |= PS_PACKET_HEADER_WRITTEN;

	if (state->write_pos == packet->buffer_pos) {
		pos = packet->buffer_pos;

		do {
			pos = (pos + sizeof(struct ps_packet_header_s) + header->size) % state->size;
			if (pos + sizeof(struct ps_packet_header_s) > state->size)
				pos = 0;

			if (sem_post(&state->written_packets))
				return EINVAL;

			header = (struct ps_packet_header_s *) &buffer->buffer[pos];
		} while (header->flags & PS_PACKET_HEADER_WRITTEN);

		state->write_pos = pos;
	}

	pthread_mutex_unlock(&state->write_close_mutex);

	packet->header = NULL;
	packet->flags = 0;

	return 0;
}

int ps_packet_getsize(ps_packet_t *packet, size_t *size)
{
	__PS_PACKET_CHECK(packet)
	*size = ((struct ps_packet_header_s *) packet->header)->size;
	return 0;
}

int ps_packet_read(ps_packet_t *packet, void *dest, size_t size)
{
	size_t offs, rlen = size;
	__PS_PACKET(packet)

	if (packet->pos + size > header->size)
		return EINVAL;

	offs = (packet->buffer_pos + sizeof(struct ps_packet_header_s) + packet->pos) % state->size;
	if (offs + size > state->size) {
		memcpy(dest, &buffer->buffer[offs], state->size - offs);

		rlen -= state->size - offs;
		offs = 0;
		dest = (void *) &((unsigned char *) dest)[size - rlen];
	}

	memcpy(dest, &buffer->buffer[offs], rlen);

	packet->pos += size;

	return 0;
}

int ps_packet_write(ps_packet_t *packet, void *src, size_t size)
{
	int ret;
	size_t offs, rlen = size;
	__PS_PACKET(packet)

	if (packet->flags & PS_PACKET_SIZE_SET) {
		if (packet->pos + size > header->size)
			return EINVAL;
	} else {
		if (packet->pos + size + sizeof(struct ps_packet_header_s) * 2 > state->size)
			return ENOBUFS;

		if ((ret = ps_packet_reserve(packet, packet->pos + size)))
			return ret;
	}

	offs = (packet->buffer_pos + sizeof(struct ps_packet_header_s) + packet->pos) % state->size;
	if (offs + size > state->size) {
		memcpy(&buffer->buffer[offs], src, state->size - offs);

		rlen -= state->size - offs;
		offs = 0;
		src = (void *) &((unsigned char *) src)[size - rlen];
	}

	memcpy(&buffer->buffer[offs], src, rlen);

	packet->pos += size;
	if (packet->pos > header->size)
		header->size = packet->pos;

	return 0;
}

int ps_packet_dma(ps_packet_t *packet, void **mem, size_t size, ps_flags_t flags)
{
	int ret;
	struct ps_fake_dma_s *fake_dma;
	size_t offs;
	__PS_PACKET(packet)

	if ((packet->flags & PS_PACKET_SIZE_SET) | (packet->flags & PS_PACKET_READ)) {
		if (packet->pos + size > header->size)
			return EINVAL;
	} else if (packet->pos + size + sizeof(struct ps_packet_header_s) * 2 > state->size)
		return ENOBUFS;

	offs = (packet->buffer_pos + sizeof(struct ps_packet_header_s) + packet->pos) % state->size;

	if (offs + size <= state->size) {
		/* real stuff */
		if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE)) {
			if ((ret = ps_packet_reserve(packet, packet->pos + size)))
				return ret;
		}
		*mem = &buffer->buffer[offs];

		packet->pos += size;
		if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE) && (packet->pos > header->size))
			header->size = packet->pos;

		return 0;
	}

	if (!(flags & PS_ACCEPT_FAKE_DMA))
		return EAGAIN;

	/* we can't give real so lets fake it */
	if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE)) {
		if ((ret = ps_packet_reserve(packet, packet->pos + size)))
			return ret;
	}

	if ((ret = ps_packet_fakedma_alloc(packet, &fake_dma, size)))
		return ret;

	fake_dma->size = size;
	fake_dma->pos = packet->pos;

	if (packet->flags & PS_PACKET_READ) {
		if ((ret = ps_packet_read(packet, fake_dma->mem, size))) {
			ps_packet_fakedma_free(packet, fake_dma);
			return ret;
		}
	}

	*mem = fake_dma->mem;

	packet->pos += size;
	if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE) && (packet->pos > header->size))
		header->size = packet->pos;

	return 0;
}

int ps_packet_tell(ps_packet_t *packet, size_t *pos)
{
	__PS_PACKET_CHECK(packet)
	return packet->pos;
}

int ps_packet_seek(ps_packet_t *packet, size_t pos)
{
	int ret;
	__PS_PACKET(packet)

	if ((packet->flags & PS_PACKET_SIZE_SET) | (packet->flags & PS_PACKET_READ)) {
		if (pos > header->size)
			return EINVAL;
	}

	if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE)) {
		if (pos + sizeof(struct ps_packet_header_s) > state->size)
			return EINVAL;

		if ((ret = ps_packet_reserve(packet, pos)))
			return ret;
	}

	packet->pos = pos;
	if ((!(packet->flags & PS_PACKET_SIZE_SET)) && (packet->flags & PS_PACKET_WRITE) && (packet->pos > header->size))
		header->size = packet->pos;

	return 0;
}

int ps_packet_fakedma_alloc(ps_packet_t *packet, struct ps_fake_dma_s **fake_dma, size_t size)
{
	struct ps_fake_dma_s *find = (struct ps_fake_dma_s *) packet->fake_dma;
	while (find != NULL) {
		if (find->free)
			break;
		find = find->next;
	}

	if (!find) {
		if (!(find = (struct ps_fake_dma_s *) malloc(sizeof(struct ps_fake_dma_s))))
			return ENOMEM;
		memset(find, 0, sizeof(struct ps_fake_dma_s));

		find->mem_size = size;
		find->mem = malloc(find->mem_size);
		find->free = 1;
		find->next = packet->fake_dma;
		packet->fake_dma = find;
	}

	if (find->mem_size < size) {
		find->mem_size = size;
		if (find->mem)
			find->mem = realloc(find->mem, find->mem_size);
		else
			find->mem = malloc(find->mem_size);
	}

	if (!find->mem)
		return ENOMEM;

	find->free = 0;
	find->size = size;
	*fake_dma = find;
	return 0;
}

int ps_packet_fakedma_free(ps_packet_t *packet, struct ps_fake_dma_s *fake_dma)
{
	fake_dma->free = 1;
	return 0;
}

int ps_packet_fakedma_commitall(ps_packet_t *packet)
{
	int ret;
	struct ps_fake_dma_s *fake_dma, *del;

	fake_dma = (struct ps_fake_dma_s *) packet->fake_dma;
	while (fake_dma != NULL) {

		del = fake_dma;
		fake_dma = (struct ps_fake_dma_s *) fake_dma->next;

		if (!del->free) {
			if ((ret = ps_packet_seek(packet, del->pos)))
				return ret;
			if ((ret = ps_packet_write(packet, del->mem, del->size)))
				return ret;
			ps_packet_fakedma_free(packet, del);
		}
	}

	return 0;
}

int ps_packet_fakedma_cut(ps_packet_t *packet, size_t size)
{
	struct ps_fake_dma_s *fake_dma = (struct ps_fake_dma_s *) packet->fake_dma;

	while (fake_dma != NULL) {
		if (fake_dma->pos > size)
			ps_packet_fakedma_free(packet, fake_dma);
		else if (fake_dma->pos + fake_dma->size > size)
			fake_dma->size = size - fake_dma->pos;

		fake_dma = fake_dma->next;
	}
	return 0;
}

int ps_packet_fakedma_freeall(ps_packet_t *packet)
{
	struct ps_fake_dma_s *fake_dma, *del;

	fake_dma = (struct ps_fake_dma_s *) packet->fake_dma;
	while (fake_dma != NULL) {
		del = fake_dma;
		fake_dma = (struct ps_fake_dma_s *) fake_dma->next;

		if (!del->free)
			ps_packet_fakedma_free(packet, del);
	}

	return 0;
}

int ps_buffer_getshmid(ps_buffer_t *buffer, int *shmid)
{
	__PS_BUFFER_CHECK(buffer)
	*shmid = buffer->shmid;
	return 0;
}

int ps_buffer_cancel(ps_buffer_t *buffer)
{
	__PS_BUFFER(buffer)

	state->flags |= PS_BUFFER_CANCELLED;

	sem_post(&state->read_packets);
	sem_post(&state->written_packets);

	pthread_mutex_unlock(&state->read_mutex);
	pthread_mutex_unlock(&state->write_mutex);

	return 0;
}

int ps_buffer_check(ps_buffer_t *buffer)
{
	if (buffer == NULL)
		return EINVAL;

	if (!(((struct ps_state_s *) buffer->state)->flags & PS_BUFFER_READY))
		return EINVAL;

	if (((struct ps_state_s *) buffer->state)->flags & PS_BUFFER_CANCELLED)
		return EINTR;

	return 0;
}

int ps_packet_check(ps_packet_t *packet)
{
	int ret;

	if (packet == NULL)
		return EINVAL;

	if (!((packet->flags & PS_PACKET_READ) || (packet->flags & PS_PACKET_WRITE)))
		return EINVAL;

	if ((ret = ps_buffer_check(packet->buffer)))
		return ret;

	return 0;
}

int ps_bufferattr_init(ps_bufferattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;

	attr->shmid = PS_SHM_CREATE;
	attr->size = PS_DEFAULT_SIZE;
	attr->flags = 0;
	attr->shmmode = 0600;

	return 0;
}

int ps_bufferattr_destroy(ps_bufferattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(ps_bufferattr_t));

	return 0;
}

int ps_bufferattr_setsize(ps_bufferattr_t *attr, size_t size)
{
	if (attr == NULL)
		return EINVAL;

	if (size < sizeof(struct ps_packet_header_s) * 2)
		return EINVAL;

	attr->size = size;

	return 0;
}

int ps_bufferattr_setflags(ps_bufferattr_t *attr, ps_flags_t flags)
{
	if (attr == NULL)
		return EINVAL;

	if ((flags & PS_BUFFER_READY) | (flags & PS_BUFFER_CANCELLED))
		return EINVAL;

#ifndef __PS_SHM
	if (flags & PS_BUFFER_PSHARED)
		return ENOTSUP;
#endif

#ifndef __PS_STATS
	if (flags & PS_BUFFFER_STATS)
		return ENOTSUP;
#endif

	attr->flags = flags;

	return 0;
}

int ps_bufferattr_setshmid(ps_bufferattr_t *attr, int id)
{
#ifdef __PS_SHM
	if (attr == NULL)
		return EINVAL;

	attr->shmid = id;

	return 0;
#else
	return ENOTSUP;
#endif
}

int ps_bufferattr_setshmmode(ps_bufferattr_t *attr, int mode)
{
#ifdef __PS_SHM
	if (attr == NULL)
		return EINVAL;

	attr->shmmode = mode;

	return 0;
#else
	return ENOTSUP;
#endif
}


unsigned long ps_buffer_utime(ps_buffer_t *buffer)
{
#ifdef __PS_STATS
	__PS_BUFFER_VARS(buffer)
	struct timeval tv;

	gettimeofday(&tv, NULL);

	tv.tv_sec -= state->create_time.tv_sec;
	tv.tv_usec -= state->create_time.tv_usec;

	if (tv.tv_usec < 0) {
		tv.tv_sec--;
		tv.tv_usec += 1000000;
	}

	return (unsigned long) tv.tv_sec * 1000000 + (unsigned long) tv.tv_usec;
#else
	return 0;
#endif
}


void ps_stats_text_hbytes(size_t bytes, FILE *stream)
{
	if (bytes >= 1024 * 1024 * 1024)
		fprintf(stream, "%.2f GiB\n", (float) bytes / (float) (1024 * 1024 * 1024));
	else if (bytes >= 1024 * 1024)
		fprintf(stream, "%.2f MiB\n", (float) bytes / (float) (1024 * 1024));
	else if (bytes >= 1024)
		fprintf(stream, "%.2f KiB\n", (float) bytes / 1024.0f);
	else
		fprintf(stream, "%d B\n", (int) bytes);
}

void ps_stats_text_hfloat(float val, FILE *stream)
{
	if (val >= 1000000000.0f)
		fprintf(stream, "%.2f G\n", val / 1000000000.0f);
	else if (val >= 1000000.0f)
		fprintf(stream, "%.2f M\n", val / 1000000.0f);
	else if (val >= 1000.0f)
		fprintf(stream, "%.2f K\n", val / 1000.0f);
	else
		fprintf(stream, "%.2f\n", val);
}

void ps_stats_text_hnum(size_t num, FILE *stream)
{
	if (num >= 1000000000)
		fprintf(stream, "%.2f G\n", (float) num / 1000000000.0f);
	else if (num >= 1000000)
		fprintf(stream, "%.2f M\n", (float) num / 1000000.0f);
	else if (num >= 1000)
		fprintf(stream, "%.2f K\n", (float) num / 1000.0f);
	else
		fprintf(stream, "%d\n", (int) num);
}


int ps_stats_text(ps_stats_t *stats, FILE *stream)
{
	float secs = ((float) stats->utime) / 1000000.0f;

	/* fprintf(stream, "ps_stats_text()\n"); */
	fprintf(stream, " run time    : %f secs\n", secs);

	if ((stats->utime > 0) && (secs >= 0.5f)) {
		fprintf(stream, " averages\n");
		fprintf(stream, "  written\n");
		fprintf(stream, "   packets   : ");
		ps_stats_text_hfloat((float) stats->written_packets / secs, stream);
		fprintf(stream, "   bytes     : ");
		ps_stats_text_hbytes(stats->written_bytes / (size_t) (secs + 0.5f), stream);
		fprintf(stream, "   %% waited  : %.2f %%\n", 100.0f * ((float) stats->write_wait_usec / (float) stats->utime));
		fprintf(stream, "  read\n");
		fprintf(stream, "   packets   : "); 
		ps_stats_text_hfloat((float) stats->read_packets / secs, stream);
		fprintf(stream, "   bytes     : "); 
		ps_stats_text_hbytes(stats->read_bytes / (size_t) (secs + 0.5f), stream);
		fprintf(stream, "   %% waited  : %.2f %%\n", 100.0f * ((float) stats->read_wait_usec / (float) stats->utime));
	}

	fprintf(stream, " totals\n");
	fprintf(stream, "  written\n");
	fprintf(stream, "   packets   : ");
	ps_stats_text_hnum(stats->written_packets, stream);
	fprintf(stream, "   bytes     : ");
	ps_stats_text_hbytes(stats->written_bytes, stream);
	fprintf(stream, "  read\n");
	fprintf(stream, "   packets   : ");
	ps_stats_text_hnum(stats->read_packets, stream);
	fprintf(stream, "   bytes     : ");
	ps_stats_text_hbytes(stats->read_bytes, stream);

	return 0;
}

/**  \} */
