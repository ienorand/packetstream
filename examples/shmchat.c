/**
 * \file src/examples/shmchat.c
 * \brief example app which uses 'packetstream' to send data across processes
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007-2008
 * For conditions of distribution and use, see copyright notice in packetstream.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <packetstream.h>

int main(int argc, char *argv[])
{
	if ((argc < 2) | (argc > 3))
		goto usage;

	int shmid = PS_SHM_CREATE;
	char line[80];
	size_t len;
	ps_bufferattr_t attr;
	ps_buffer_t buffer;
	ps_packet_t packet;

	ps_bufferattr_init(&attr);
	ps_bufferattr_setflags(&attr, PS_BUFFER_PSHARED);
	ps_bufferattr_setsize(&attr, 1024);

	if (!strcmp(argv[1], "-s")) {
		if (ps_buffer_init(&buffer, &attr)) {
			printf("ps_buffer_init() failed\n");
			return 1;
		}

		ps_buffer_getshmid(&buffer, &shmid);

		printf("shmid = %d\n", (int) shmid);

		ps_packet_init(&packet, &buffer);

		do {
			ps_packet_open(&packet, PS_PACKET_READ);
			ps_packet_getsize(&packet, &len);

			if (len > sizeof(line)) {
				printf("too long message (%d bytes)\n", (int) len);
				ps_packet_close(&packet);
				break;
			}

			ps_packet_read(&packet, line, len);
			ps_packet_close(&packet);

			printf("%s", line);
		} while (strcmp(line, "/quit\n"));

		ps_packet_destroy(&packet);
		ps_buffer_destroy(&buffer);
	} else if (!strcmp(argv[1], "-c")) {
		if (argc < 3)
			goto usage;

		shmid = atoi(argv[2]);
		ps_bufferattr_setshmid(&attr, shmid);

		if (ps_buffer_init(&buffer, &attr)) {
			printf("ps_buffer_init() failed\n");
			return 1;
		}

		ps_packet_init(&packet, &buffer);

		do {
			fgets(line, sizeof(line), stdin);

			ps_packet_open(&packet, PS_PACKET_WRITE);
			ps_packet_write(&packet, line, strlen(line) + 1); /* including \0 */
			ps_packet_close(&packet);
		} while (strcmp(line, "/quit\n"));

		ps_packet_destroy(&packet);
	} else
		goto usage;

	ps_bufferattr_destroy(&attr);

	printf("done\n");

	return 0;
usage:
	printf("Usage: %s [-s] [-c shmid]\n", argv[0]);
	return 1;
}
