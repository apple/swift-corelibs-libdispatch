/*
 * Copyright (c) 2010-2011 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <spawn.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <Block.h>
#include <bsdtests.h>
#include "dispatch_test.h"
#include <dispatch/dispatch.h>

extern char **environ;

#ifndef DISPATCHTEST_IO
#if DISPATCH_API_VERSION >= 20100226 && DISPATCH_API_VERSION != 20101110
#define DISPATCHTEST_IO 1
#endif
#endif

#if DISPATCHTEST_IO
int
main(int argc, char** argv)
{
	struct hostent *he;
	int sockfd, clientfd;
	struct sockaddr_in addr1, addr2, server;
	socklen_t addr2len;
	socklen_t addr1len;
	pid_t clientid;

	const char *path = "/usr/share/dict/words";
	int read_fd, fd;

	if (argc == 2) {
		// Client
		dispatch_test_start(NULL);

		if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			test_errno("Client-socket()", errno, 0);
			test_stop();
		}

		if ((he = gethostbyname("localhost")) == NULL) {
			fprintf(stderr, "Client-gethostbyname() failed\n");
			test_stop();
		}

		memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
		server.sin_family = AF_INET;
		server.sin_port = atoi(argv[1]);

		fprintf(stderr, "Client-connecting on port ... %d\n", server.sin_port);

		if (connect(sockfd, (struct sockaddr *)&server, sizeof(server))) {
			test_errno("client-connect()", errno, 0);
			test_stop();
		}

		// Read from the socket and compare the contents are what we expect

		fd = open(path, O_RDONLY);
		if (fd == -1) {
			test_errno("client-open", errno, 0);
			test_stop();
		}
		if (fcntl(fd, F_NOCACHE, 1)) {
			test_errno("client-fcntl F_NOCACHE", errno, 0);
			test_stop();
		}
		struct stat sb;
		if (fstat(fd, &sb)) {
			test_errno("client-fstat", errno, 0);
			test_stop();
		}
		size_t size = sb.st_size;

		__block dispatch_data_t g_d1 = dispatch_data_empty;
		__block dispatch_data_t g_d2 = dispatch_data_empty;
		__block int g_error = 0;

		dispatch_group_t g = dispatch_group_create();
		dispatch_group_enter(g);
		dispatch_read(fd, size, dispatch_get_global_queue(0, 0),
				^(dispatch_data_t d1, int error) {
			test_errno("Client-dict-read error", error, 0);
			test_long("Client-dict-dispatch data size",
					dispatch_data_get_size(d1), size);
			dispatch_retain(d1);
			g_d1 = d1;
			dispatch_group_leave(g);
		});

		__block void (^b)(dispatch_data_t, int);
		b = Block_copy(^(dispatch_data_t d2, int error) {
			dispatch_data_t concat = dispatch_data_create_concat(g_d2, d2);
			dispatch_release(g_d2);
			g_d2 = concat;
			if (!error && dispatch_data_get_size(d2)) {
				dispatch_read(sockfd, SIZE_MAX,
						dispatch_get_global_queue(0, 0), b);
			} else {
				g_error = error;
				dispatch_group_leave(g);
			}
		});
		dispatch_group_enter(g);
		dispatch_read(sockfd, SIZE_MAX, dispatch_get_global_queue(0, 0), b);
		test_group_wait(g);
		test_errno("Client-read error", g_error, 0);
		test_long("Client-dispatch data size", dispatch_data_get_size(g_d2),
				size);

		size_t dict_contig_size, socket_contig_size;
		const void *dict_contig_buf, *socket_contig_buf;
		dispatch_data_t dict_data = dispatch_data_create_map(g_d1,
				&dict_contig_buf, &dict_contig_size);
		dispatch_data_t socket_data = dispatch_data_create_map(g_d2,
				&socket_contig_buf, &socket_contig_size);
		test_long("Client-dispatch data contents",
				 memcmp(dict_contig_buf, socket_contig_buf,
				 MIN(dict_contig_size, socket_contig_size)), 0);

		close(fd);
		close(sockfd);
		dispatch_release(g_d1);
		dispatch_release(g_d2);
		dispatch_release(dict_data);
		dispatch_release(socket_data);
		dispatch_release(g);
		test_stop();
	} else {
		// Server
		dispatch_test_start("Dispatch IO Network test");

		if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
			test_errno("Server-socket()", errno, 0);
			test_stop();
		}

		addr1.sin_family = AF_INET;
		addr1.sin_addr.s_addr = INADDR_ANY;
		addr1.sin_port = 0;

		if (bind(sockfd, (struct sockaddr *)&addr1, sizeof(struct sockaddr)) ==
				-1) {
			test_errno("Server-bind()", errno, 0);
			test_stop();
		}

		addr1len = sizeof(struct sockaddr);
		if (getsockname(sockfd, (struct sockaddr*)&addr1, &addr1len) == -1) {
			test_errno("Server-getsockname()", errno, 0);
			test_stop();
		}

		if(listen(sockfd, 3) == -1) {
			test_errno("Server-listen()", errno, 0);
			test_stop();
		}

		fprintf(stderr, "Server started and listening on port %d\n",
				addr1.sin_port);

		char exec_filename [256] = {};
		uint32_t bufsize = 256;

		if (_NSGetExecutablePath(exec_filename, &bufsize) == -1) {
			fprintf(stderr, "Failed to get path name for running executable\n");
			test_stop();
		}

		char port_str[10] = {};
		snprintf(port_str, 10, " %d", addr1.sin_port);

		char *arguments [3] = {};
		arguments[0] = exec_filename;
		arguments[1] = port_str;
		arguments[2] = NULL;

		int error;
		if ((error = posix_spawnp(&clientid, exec_filename, NULL, NULL,
				arguments, environ)) != 0) {
			test_errno("Server-posix_spawnp()", error, 0);
			test_stop();
		}

		addr2len = sizeof(struct sockaddr_in);
		clientfd = accept(sockfd, (struct sockaddr *)&addr2, &addr2len);
		if(clientfd == -1) {
			test_errno("Server-accept()", errno, 0);
			test_stop();
		}

		fprintf(stderr, "Server accepted connection. Server now writing\n");
		read_fd = open(path, O_RDONLY);
		if (read_fd == -1) {
			test_errno("open", errno, 0);
			goto stop_test;
		}
		if (fcntl(read_fd, F_NOCACHE, 1)) {
			test_errno("fcntl F_NOCACHE", errno, 0);
			goto stop_test;
		}
		struct stat sb;
		if (fstat(read_fd, &sb)) {
			test_errno("fstat", errno, 0);
			goto stop_test;
		}
		size_t size = sb.st_size;

		dispatch_group_t g = dispatch_group_create();
		dispatch_group_enter(g);
		dispatch_read(read_fd, size, dispatch_get_global_queue(0, 0),
				^(dispatch_data_t d, int r_err){
			fprintf(stderr, "Server-dispatch_read()\n");
			test_errno("Server-read error", r_err, 0);
			test_long("Server-dispatch data size", dispatch_data_get_size(d),
					size);

			// convenience method handlers should only be called once
			if (dispatch_data_get_size(d)!= size) {
				fprintf(stderr, "Reading of data didn't complete\n");
				close(read_fd);
				close(clientfd);
				close(sockfd);
				test_stop();
			}
			dispatch_group_enter(g);
			dispatch_write(clientfd, d, dispatch_get_global_queue(0, 0),
					^(dispatch_data_t remaining, int w_err) {
				test_errno("Server-write error", w_err, 0);
				test_ptr_null("Server-dispatch write remaining data",remaining);
				// convenience method handlers should only be called once
				if (remaining) {
					fprintf(stderr, "Server-dispatch_write() incomplete .. "
							"%lu bytes\n", dispatch_data_get_size(remaining));
					close(read_fd);
					close(clientfd);
					close(sockfd);
					test_stop();
				}
				close(clientfd); // Sending the client EOF
				dispatch_group_leave(g);
			});
			close(read_fd);
			dispatch_group_leave(g);
		});
		test_group_wait(g);
		dispatch_release(g);
		fprintf(stderr, "Shutting down server\n");
		close(sockfd);
		test_stop();

stop_test:
		close(read_fd);
		close(clientfd);
		close(sockfd);
		test_stop();
	}
}
#else
int
main()
{
	dispatch_test_start("Dispatch IO Network test - No Dispatch IO");
	test_stop();
}
#endif
