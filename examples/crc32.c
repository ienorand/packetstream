/**
 * \file examples/crc32.c
 * \brief example app which uses 'packetstream' to distribute CRC32 calculation jobs
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007-2008
 * For conditions of distribution and use, see copyright notice in packetstream.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>

#include <packetstream.h>

#define WRITER_COUNT 1
#define READER_COUNT 2
#define DATA_MIN 0
#define DATA_MAX (1024 * 1024 * 10)
#define BUFFER_SIZE (1024 * 1024 * 50)
#define STATS_SEC 5

unsigned int crc_table[256];

unsigned int calcCRC32(char *data, size_t size)
{
	register unsigned long crc = 0xffffffff;
	size_t i;
	for (i = 0; i < size; i++)
		crc = ((crc >> 8) & 0x00ffffff) ^ crc_table[(crc ^ *data++) & 0xff];

	return (unsigned int) (crc ^ 0xffffffff);
}

void *writer_thread(void *addr)
{
	ps_buffer_t *buffer = (ps_buffer_t *) addr;
	ps_packet_t packet;
	ps_packet_init(&packet, buffer);
	char *temp = (char *) malloc(DATA_MAX);
	size_t size;
	int ret;

	while (1) {
		size = DATA_MIN + rand() % (DATA_MAX - DATA_MIN);

		if ((ret = ps_packet_open(&packet, PS_PACKET_WRITE))) {
			if (ret != EINTR)
				printf("writer_thread(): ps_packet_open() failed\n");
			break;
		}

		ps_packet_setsize(&packet, size);
		ps_packet_write(&packet, temp, size);
		ps_packet_close(&packet);
	}

	ps_packet_destroy(&packet);

	return NULL;
}

void *reader_thread(void *addr)
{
	ps_buffer_t *buffer = (ps_buffer_t *) addr;
	ps_packet_t packet;
	ps_packet_init(&packet, buffer);
	size_t size;
	char *dma;
	int ret;

	while (1) {
		if ((ret = ps_packet_open(&packet, PS_PACKET_READ))) {
			if (ret != EINTR)
				printf("reader_thread(): ps_packet_open() failed\n");
			break;
		}

		if (ps_packet_getsize(&packet, &size))
			break;

		if ((size < DATA_MIN) | (size > DATA_MAX)) {
			printf("reader_thread(): corruption detected, packet.pos = %d\n", (int) packet.pos);
			break;
		}

		if (ps_packet_dma(&packet, (void *) &dma, size, PS_ACCEPT_FAKE_DMA))
			break;

		calcCRC32(dma, size);

		ps_packet_close(&packet);
	}

	ps_packet_destroy(&packet);

	return NULL;
}

int main(int argc, char *argv[])
{
	register unsigned long crc;
	unsigned long poly = 0xEDB88320L;
	int i, j;
	ps_buffer_t buffer;
	ps_bufferattr_t bufferattr;
	ps_stats_t stats;
	int writer_count = WRITER_COUNT;
	int reader_count = READER_COUNT;
	pthread_t writer_thread_t[writer_count], reader_thread_t[reader_count];
	pthread_attr_t attr;

	srand(1);

	for (i = 0; i < 256; i++) {
		crc = i;
		for (j = 8; j > 0; j--) {
			if (crc & 1)
				crc = (crc >> 1) ^ poly;
			else
				crc >>= 1;
		}
		crc_table[i] = crc;
	}

	ps_bufferattr_init(&bufferattr);
	ps_bufferattr_setflags(&bufferattr, PS_BUFFER_STATS);
	ps_bufferattr_setsize(&bufferattr, BUFFER_SIZE);

	if (ps_buffer_init(&buffer, &bufferattr)) {
		printf("ps_buffer_create() failed\n");
		return 1;
	}

	ps_bufferattr_destroy(&bufferattr);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	for (i = 0; i < writer_count; i++)
		pthread_create(&writer_thread_t[i], &attr, writer_thread, (void *) &buffer);

	for (i = 0; i < reader_count; i++)
		pthread_create(&reader_thread_t[i], &attr, reader_thread, (void *) &buffer);

	pthread_attr_destroy(&attr);

	usleep(STATS_SEC * 1000000);

	ps_buffer_cancel(&buffer);

	for (i = 0; i < writer_count; i++)
		pthread_join(writer_thread_t[i], NULL);

	for (i = 0; i < reader_count; i++)
		pthread_join(reader_thread_t[i], NULL);

	ps_buffer_stats(&buffer, &stats);
	ps_stats_text(&stats, stdout);

	ps_buffer_destroy(&buffer);

	return 0;
}
