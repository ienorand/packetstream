/**
 * \file src/packetstream.h
 * \brief interface of the 'packetstream' thread-safe ring buffer
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007-2008
 */

/* packetstream.h -- interface of the 'packetstream' thread-safe ring buffer
  version 0.1.4, March 9th, 2008

  Copyright (C) 2007-2008 Pyry Haulos

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Pyry Haulos <pyry.haulos@gmail.com>
*/

/*
 TODO
  dynamic buffer size
*/

#ifndef _PACKETSTREAM_H
#define _PACKETSTREAM_H

#ifdef WIN32
# define __PS_PUBLIC __declspec (dllexport)
#else
# define __PS_PUBLIC __attribute__ ((visibility ("default")))
#endif

#include <stddef.h>
#include <stdio.h>

#ifdef WIN32
# define IPC_PRIVATE 0
#else
# include <sys/ipc.h>
# define __PS_SHM
# define __PS_STATS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  \defgroup packetstream packetstream
 *  packetstream is basically a solution to the Producer-Consumer problem with a fixed
 *  size ring buffer and variable item (=packet) size. This implementation follows classic
 *  solution using semaphores with a few exceptions:
 *
 *  1. Since the maximum amount of items the buffer can hold is not known, semaphore
 *     "empty" (=read_packets) is only decreased when producer needs to remove
 *     a consumed item to have space for a new one.
 *
 *  2. Multiple producers 1..n can simultaneously write to the buffer providing that
 *     there is enough free space and all producers 1..n-1 have set a size for
 *     their items.
 *
 *  3. Multiple consumers 1..n can simultaneously read from the buffer if there is
 *     1..n ready items.
 *  \{
 */

/**
 *  \defgroup bufferattr buffer attributes
 *  Buffer attribute object (ps_bufferattr_t) holds information about
 *  which kind of buffer to create with ps_buffer_create(). Same buffer
 *  attribute object can be used for creating multiple buffers.
 */

/**
 *  \defgroup buffer buffer
 *  Buffer holds data and buffer state information. Functions are
 *  thread-safe only if stated so.
 */

/**
 *  \defgroup packet packet
 *  Packets are the main read/write interface to the buffer. All operations
 *  are thread-safe with respect to buffer as long as same thread does not
 *  try to open multiple packets.
 */

/**
 *  \defgroup stats statistics
 */

/**
 * \addtogroup buffer
 *  \{
 */
/** buffer is initialized */
#define PS_BUFFER_READY          1
/** buffer is shared across processes */
#define PS_BUFFER_PSHARED        2
/** collect statistics */
#define PS_BUFFER_STATS          4
/** buffer is in cancelled state */
#define PS_BUFFER_CANCELLED      8

/**  \} */

/**
 * \addtogroup packet
 *  \{
 */

/** read mode */
#define PS_PACKET_READ           1
/** write mode */
#define PS_PACKET_WRITE          2
/** packet has constant size set */
#define PS_PACKET_SIZE_SET       4
/** fail if can't proceed immediately */
#define PS_PACKET_TRY            8

/** accept fake dma */
#define PS_ACCEPT_FAKE_DMA       1

/**  \} */

/**
 * \addtogroup bufferattr
 *  \{
 */

/** default buffer size */
#define PS_DEFAULT_SIZE    1048576

/** special shmid which forces buffer to create new shm area */
#define PS_SHM_CREATE  IPC_PRIVATE

/**  \} */

typedef int ps_flags_t;

/**
 * \ingroup stats
 * \brief buffer statistics
 */
typedef struct {
	/** number of packets read */
	size_t read_packets;
	/** number of packets written */
	size_t written_packets;
	/** amount of data read */
	size_t read_bytes;
	/** amount of data written */
	size_t written_bytes;
	/** time in microseconds consumer has waited for ready item */
	unsigned long read_wait_usec;
	/** time in microseconds producer has waited for free space */
	unsigned long write_wait_usec;
	/** time in microseconds since buffer was created */
	unsigned long utime;
} ps_stats_t;

/**
 * \ingroup bufferattr
 * \brief buffer attributes
 */
typedef struct {
	/** flags */
	ps_flags_t flags;
	/** buffer size in bytes */
	size_t size;
	/** shared memory id */
	int shmid;
	/** shared memory permission mask */
	int shmmode;
} ps_bufferattr_t;

/**
 * \ingroup buffer
 * \brief buffer
 */
typedef struct {
	/** pointer to internal buffer state (ps_state_s) */
	void *state;
	/** pointer to buffer data area */
	unsigned char *buffer;
	/** pointer to stats or NULL if PS_BUFFER_STATS is not set */
	ps_stats_t *stats;
	/** shared memory id */
	int shmid;
	/** time in microseconds when consumer entered waiting mode last time */
	unsigned long read_wait_start;
	/** time in microseconds when producer entered waiting mode last time */
	unsigned long write_wait_start;
} ps_buffer_t;

/**
 * \ingroup packet
 * \brief packet
 */
typedef struct {
	/** flags */
	ps_flags_t flags;
	/** pointer to buffer this packet is associated to */
	ps_buffer_t *buffer;
	/** position in buffer */
	size_t buffer_pos;
	/** current read/write position in packet data area */
	size_t pos;
	/** reserved size in buffer */
	size_t reserved;
	/** pointer to packet header */
	void *header;
	/** fake dma object linked list */
	void *fake_dma;
} ps_packet_t;

/**
 * \addtogroup bufferattr
 *  \{
 */

/**
 * \brief initialize buffer attribute object
 * \param attr buffer attribute object to initialize
 * \return 0 on success or EINVAL if attr is NULL
 */
__PS_PUBLIC int ps_bufferattr_init(ps_bufferattr_t *attr);
/**
 * \brief destroy buffer attribute object
 * \param attr buffer attribute object to destroy
 * \return 0 on success or EINVAL if attr is NULL
 */
__PS_PUBLIC int ps_bufferattr_destroy(ps_bufferattr_t *attr);
/**
 * \brief set buffer size
 * \param attr buffer attribute object
 * \param size buffer size
 * \return 0 on success or EINVAL if attr is NULL or size is too small
 */
__PS_PUBLIC int ps_bufferattr_setsize(ps_bufferattr_t *attr, size_t size);
/**
 * \brief set buffer flags
 * \param attr buffer attribute object
 * \param flags valid flags are PS_BUFFER_PSHARED and PS_BUFFER_STATS
 * \return 0 on success or EINVAL if attr is NULL or flags are not valid
 */
__PS_PUBLIC int ps_bufferattr_setflags(ps_bufferattr_t *attr, ps_flags_t flags);
/**
 * \brief set buffer shared memory id
 * \param attr buffer attribute object
 * \param id shared memory id or PS_SHM_CREATE to create a new
 * \return 0 on success or EINVAL if attr is NULL
 */
__PS_PUBLIC int ps_bufferattr_setshmid(ps_bufferattr_t *attr, int id);
/**
 * \brief set buffer shared memory mode
 * \param attr buffer attribute object
 * \param mode octal permission mode
 * \return 0 on success or EINVAL if attr is NULL or mode is not valid
 */
__PS_PUBLIC int ps_bufferattr_setshmmode(ps_bufferattr_t *attr, int mode);

/**  \} */

/**
 * \addtogroup buffer
 *  \{
 */

/**
 * \brief initialize buffer
 * \param buffer buffer to initialize
 * \param attr initalized buffer attribute object
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_buffer_init(ps_buffer_t *buffer, ps_bufferattr_t *attr);
/**
 * \brief destory buffer
 * \param buffer buffer to destroy
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_buffer_destroy(ps_buffer_t *buffer);
/**
 * \brief cancel buffer
 *
 * This sets PS_BUFFER_CANCELLED flag and triggers all waiting calls
 * to wake up and return EINTR. All subsequent calls to buffer except
 * ps_buffer_destroy() return immediately with EINTR. This is
 * thread-safe function.
 * \param buffer buffer to cancel
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_buffer_cancel(ps_buffer_t *buffer);
/**
 * \brief acquire a copy of buffer statistics
 *
 * If PS_BUFFER_STATS was not defined when creating buffer, this call
 * always returns ENOTSUP. Thread-safe, but no synchronization is performed,
 * so returned statistics are not always correct.
 * \param buffer buffer
 * \param stats returned statisticts
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_buffer_stats(ps_buffer_t *buffer, ps_stats_t *stats);
/**
 * \brief get buffer shared memory id
 *
 * Returns active shared memory id. This is thread-safe function.
 * \param buffer buffer
 * \param shmid returned shared memory id
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_buffer_getshmid(ps_buffer_t *buffer, int *shmid);

/**  \} */

/**
 * \addtogroup packet
 *  \{
 */

/**
 * \brief initialize packet
 *
 * \param packet packet to initialized
 * \param buffer buffer to bind this packet to
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_init(ps_packet_t *packet, ps_buffer_t *buffer);
/**
 * \brief destroy packet
 * \param packet packet to destroy
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_destroy(ps_packet_t *packet);
/**
 * \brief open packet
 *
 * Flags should define PS_PACKET_WRITE or PS_PACKET_READ, not both.
 * PS_PACKET_WRITE opens packet in write mode and PS_PACKET_READ in
 * read mode. If PS_PACKET_TRY is specified, all calls return EBUSY
 * instead of blocking if waiting for other threads is necessary.
 * \param packet packet
 * \param flags PS_PACKET_WRITE or PS_PACKET_READ, possibly PS_PACKET_TRY
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_open(ps_packet_t *packet, ps_flags_t flags);
/**
 * \brief close packet
 * \param packet packet to close
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_close(ps_packet_t *packet);
/**
 * \brief cancel packet
 *
 * Discards all data in packet and frees area in buffer. Only
 * possible if packet is open in write mode and constant size
 * for it (ps_packet_setsize()) is not defined.
 * \param packet packet to cancel
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_cancel(ps_packet_t *packet);
/**
 * \brief tell current read/write position in packet
 * \param packet packet
 * \param pos returned position
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_tell(ps_packet_t *packet, size_t *pos);
/**
 * \brief seek to given read/write position
 *
 * If packet is open in write mode and given position is greater
 * than packet size, data is allocated up to the position.
 * \param packet packet
 * \param pos position to seek to
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_seek(ps_packet_t *packet, size_t pos);
/**
 * \brief get packet size
 * \param packet packet
 * \param size returned size
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_getsize(ps_packet_t *packet, size_t *size);
/**
 * \brief set packet size
 *
 * This sets size of an packet opened in write mode to a constant value.
 * After this it is possible to open a new packet in write mode and
 * all subsequent write calls to this packet are limited to the reserved
 * size.
 *
 * All data (including dma) that has been written outside the new size
 * is discarded.
 * \param packet packet
 * \param size constant size for packet
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_setsize(ps_packet_t *packet, size_t size);
/**
 * \brief read data from packet
 *
 * Reads size bytes of data from packet and moves current read/write
 * position by size bytes.
 * \param packet packet
 * \param dest destination memory area
 * \param size bytes to read
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_read(ps_packet_t *packet, void *dest, size_t size);
/**
 * \brief write data to packet
 *
 * Writes size bytes of data to packet and moves current read/write
 * position by size bytes.
 * \param packet packet
 * \param src source memory area
 * \param size bytes to write
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_write(ps_packet_t *packet, void *src, size_t size);
/**
 * \brief acquire direct memory access to packet
 *
 * Returns pointer to buffer data area with at least size bytes of
 * contiguous data and moves current read/write position by size bytes.
 *
 * Since buffer is circular, packet data area may span over buffer boundary
 * so this function is not guaranteed to succeed. However if PS_ACCEPT_FAKE_DMA
 * flag is given, this function returns fake dma address, which behaves
 * like normal direct memory access area with exception that data is actually
 * written to buffer when packet is closed.
 * \param packet packet
 * \param mem returned direct memory access pointer
 * \param size dma area size
 * \param flags flags, currently only PS_ACCEPT_FAKE_DMA is specified
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_packet_dma(ps_packet_t *packet, void **mem, size_t size, ps_flags_t flags);

/**  \} */

/**
 * \ingroup stats
 * \brief write nicely formatted statistics to given stream
 * \param stats statistics
 * \param stream stream
 * \return 0 on success otherwise an error code
 */
__PS_PUBLIC int ps_stats_text(ps_stats_t *stats, FILE *stream);

/**  \} */

#ifdef __cplusplus
}
#endif

#endif
