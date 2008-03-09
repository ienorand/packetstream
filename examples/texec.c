/**
 * \file examples/texec.c
 * \brief example app which uses 'packetstream' to execute commands in parallel
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007-2008
 * For conditions of distribution and use, see copyright notice in packetstream.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>

#include <packetstream.h>

struct texec_shared_s {
	int buffer_shmid;
	sem_t finished;
};

struct texec_shared_s *shm_get_shared(int *shmid)
{
	struct texec_shared_s *shared;

	if (*shmid == IPC_PRIVATE) {
		*shmid = shmget(IPC_PRIVATE, sizeof(struct texec_shared_s), IPC_CREAT | IPC_EXCL | 0600);
		shared = (struct texec_shared_s *) shmat(*shmid, NULL, 0);

		sem_init(&shared->finished, 1, 0);
		shared->buffer_shmid = PS_SHM_CREATE;
	} else
		shared = (struct texec_shared_s *) shmat(*shmid, NULL, 0);

	return shared;
}

void send_kill(ps_buffer_t *buffer)
{
	ps_packet_t packet;
	const char kill_cmd = 'k';

	ps_packet_init(&packet, buffer);

	ps_packet_open(&packet, PS_PACKET_WRITE);
	ps_packet_write(&packet, (void *) &kill_cmd, 1);
	ps_packet_close(&packet);

	ps_packet_destroy(&packet);
}

void send_command(ps_buffer_t *buffer, char *command)
{
	ps_packet_t packet;
	const char exec_cmd = 'e';

	ps_packet_init(&packet, buffer);

	ps_packet_open(&packet, PS_PACKET_WRITE);
	ps_packet_write(&packet, (void *) &exec_cmd, 1);
	ps_packet_write(&packet, command, strlen(command) + 1);
	ps_packet_close(&packet);

	ps_packet_destroy(&packet);
}

void *exec_thread(void *addr)
{
	ps_buffer_t *buffer = (ps_buffer_t *) addr;
	ps_packet_t packet;
	ps_packet_init(&packet, buffer);
	char *command = NULL;
	size_t len;

	while (1) {
		ps_packet_open(&packet, PS_PACKET_READ);
		ps_packet_getsize(&packet, &len);

		if (command == NULL)
			command = (char *) malloc(len);
		else
			command = (char *) realloc(command, len);

		ps_packet_read(&packet, command, len);
		ps_packet_close(&packet);

		if (command[0] == 'k') {
			send_kill(buffer);
			break;
		}

		system(&command[1]);
	}

	ps_packet_destroy(&packet);

	return NULL;
}

void run_server(ps_buffer_t *buffer, int threads, sem_t *finished)
{
	pthread_t *exec_threads;
	pthread_attr_t tattr;
	int i;

	exec_threads = (pthread_t *) malloc(sizeof(pthread_t) * threads);

	pthread_attr_init(&tattr);
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE);

	for (i = 0; i < threads; i++)
		pthread_create(&exec_threads[i], &tattr, exec_thread, (void *) buffer);

	pthread_attr_destroy(&tattr);

	for (i = 0; i < threads; i++)
		pthread_join(exec_threads[i], NULL);

	free(exec_threads);
	ps_buffer_destroy(buffer);

	sem_post(finished);
}

int main(int argc, char *argv[])
{
	int threads, opt, server, exec, kill, shmid;
	long size;
	ps_buffer_t buffer;
	ps_bufferattr_t battr;
	char *command = NULL;
	struct texec_shared_s *shared;

	threads = server = exec = kill = 0;
	size = -1;
	shmid = IPC_PRIVATE;

	while ((opt = getopt(argc, argv, "hst:b:c:ke:")) != -1) {
		switch (opt) {
		case 's':
			server = 1;
			break;
		case 't':
			threads = atoi(optarg);
			break;
		case 'b':
			size = atol(optarg);
			break;
		case 'c':
			shmid = atoi(optarg);
			break;
		case 'k':
			kill = 1;
			break;
		case 'e':
			exec = 1;
			command = optarg;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (threads < 1)
		threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;

	if (size < 1024) /* something sane */
		size = threads * 1024;

	shared = shm_get_shared(&shmid);

	ps_bufferattr_init(&battr);
	ps_bufferattr_setflags(&battr, PS_BUFFER_PSHARED);
	ps_bufferattr_setshmid(&battr, shared->buffer_shmid);
	ps_bufferattr_setsize(&battr, size);

	if (ps_buffer_init(&buffer, &battr)) {
		fprintf(stderr, "can't create buffer\n");
		return EXIT_FAILURE;
	}

	ps_bufferattr_destroy(&battr);

	if (server) {
		printf("%d\n", shmid);
		ps_buffer_getshmid(&buffer, &shared->buffer_shmid);

		fclose(stdout);
		fclose(stdin);
		fclose(stderr);

		if (fork() == 0) {
			setsid();
			run_server(&buffer, threads, &shared->finished);
		}
	} else if (exec)
		send_command(&buffer, command);
	else if (kill) {
		send_kill(&buffer);

		sem_wait(&shared->finished);
		sem_destroy(&shared->finished);

		shmdt(shared);
		shmctl(shmid, IPC_RMID, 0);
	}
	else
		goto usage;

	return EXIT_SUCCESS;

usage:
	printf("%s [OPTION]...\n", argv[0]);
	printf("  -s               start texec server\n");
	printf("  -t THREADS       number of threads, default is number of CPUs * 2\n");
	printf("  -b SIZE          command buffer size, default is THREADS * 1024\n");
	printf("  -c SHMID         connect to texec server\n");
	printf("  -k               kill server after all commands have returned\n");
	printf("  -e COMMAND       execute command\n");
	printf("  -h               show help\n");

	return EXIT_FAILURE;
}
