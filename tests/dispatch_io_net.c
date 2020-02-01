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
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <netdb.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#include <Block.h>
#include <bsdtests.h>
#include "dispatch_test.h"
#include <dispatch/dispatch.h>

#if !defined(_WIN32)
extern char **environ;
#endif

#ifndef DISPATCHTEST_IO
#if DISPATCH_API_VERSION >= 20100226 && DISPATCH_API_VERSION != 20101110
#define DISPATCHTEST_IO 1
#endif
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(_WIN32) || defined(__OpenBSD__)
#define _NSGetExecutablePath(ef,bs) (*(bs)=(size_t)snprintf(ef,*(bs),"%s",argv[0]),0)
#endif

#if defined(_WIN32)
typedef USHORT in_port_t;
#endif

#if !defined(_WIN32)
#define closesocket(x) close(x)
#endif

#if DISPATCHTEST_IO
int
main(int argc, char** argv)
{
	struct hostent *he;
	int sockfd = -1, clientfd = -1;
	dispatch_fd_t read_fd = -1, fd = -1;
	struct sockaddr_in addr1, addr2, server;
	socklen_t addr2len;
	socklen_t addr1len;
	pid_t clientid;

#if defined(_WIN32)
	WSADATA wsa;
	int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0) {
		fprintf(stderr, "WSAStartup failed with %d\n", err);
		test_stop();
	}
#endif

	if (argc == 3) {
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

		memcpy(&server.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
		server.sin_family = AF_INET;
		server.sin_port = (in_port_t)atoi(argv[1]);

		fprintf(stderr, "Client-connecting on port ... %d\n", server.sin_port);

		if (connect(sockfd, (struct sockaddr *)&server, sizeof(server))) {
			test_errno("client-connect()", errno, 0);
			test_stop();
		}

		// Read from the socket and compare the contents are what we expect

		const char *path = argv[2];
		fd = dispatch_test_fd_open(path, O_RDONLY);
		if (fd == -1) {
			test_errno("client-open", errno, 0);
			test_stop();
		}

		// The reference file path given to us by the server was produced by
		// dispatch_test_get_large_file(). It may point to a temporary file, and
		// we are responsible for cleaning it up because we are the last to
		// access it.
		dispatch_test_release_large_file(path);

#ifdef F_NOCACHE
		if (fcntl(fd, F_NOCACHE, 1)) {
			test_errno("client-fcntl F_NOCACHE", errno, 0);
			test_stop();
		}
#else
		// investigate what the impact of lack of file cache disabling has 
		// for this test
#endif
		size_t size = (size_t)dispatch_test_fd_lseek(fd, 0, SEEK_END);
		dispatch_test_fd_lseek(fd, 0, SEEK_SET);

		__block dispatch_data_t g_d1 = dispatch_data_empty;
		__block dispatch_data_t g_d2 = dispatch_data_empty;
		__block int g_error = 0;

		dispatch_group_t g = dispatch_group_create();
		dispatch_group_enter(g);
		dispatch_read(fd, size, dispatch_get_global_queue(0, 0),
				^(dispatch_data_t d1, int error) {
			test_errno("Client-dict-read error", error, 0);
			test_long("Client-dict-dispatch data size",
					  (long)dispatch_data_get_size(d1), (long)size);
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
		test_long("Client-dispatch data size", (long)dispatch_data_get_size(g_d2),
				  (long)size);

		size_t dict_contig_size, socket_contig_size;
		const void *dict_contig_buf, *socket_contig_buf;
		dispatch_data_t dict_data = dispatch_data_create_map(g_d1,
				&dict_contig_buf, &dict_contig_size);
		dispatch_data_t socket_data = dispatch_data_create_map(g_d2,
				&socket_contig_buf, &socket_contig_size);
		test_long("Client-dispatch data contents",
				 memcmp(dict_contig_buf, socket_contig_buf,
				 MIN(dict_contig_size, socket_contig_size)), 0);

		dispatch_test_fd_close(fd);
		closesocket(sockfd);
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
		size_t bufsize = 256;

		if (_NSGetExecutablePath(exec_filename, &bufsize) == -1) {
			fprintf(stderr, "Failed to get path name for running executable\n");
			test_stop();
		}

		char port_str[10] = {};
		snprintf(port_str, 10, " %d", addr1.sin_port);

		// The client must read from the same test file as the server. It will
		// unlink the file as soon as it can, so the server must open it before
		// starting the client process.
		char *path = dispatch_test_get_large_file();
		read_fd = dispatch_test_fd_open(path, O_RDONLY);
		if (read_fd == -1) {
			test_errno("open", errno, 0);
			goto stop_test;
		}

		char *arguments [4] = {};
		arguments[0] = exec_filename;
		arguments[1] = port_str;
		arguments[2] = path;
		arguments[3] = NULL;

#ifdef HAVE_POSIX_SPAWNP
		int error;
		if ((error = posix_spawnp(&clientid, exec_filename, NULL, NULL,
				arguments, environ)) != 0) {
			test_errno("Server-posix_spawnp()", error, 0);
			goto stop_test;
		}
#elif defined(_WIN32)
		WCHAR *cmdline = argv_to_command_line(arguments);
		if (!cmdline) {
			fprintf(stderr, "argv_to_command_line() failed\n");
			test_stop();
		}
		STARTUPINFOW si = {.cb = sizeof(si)};
		PROCESS_INFORMATION pi;
		BOOL created = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL,
				NULL, &si, &pi);
		DWORD error = GetLastError();
		free(cmdline);
		if (!created) {
			print_winapi_error("CreateProcessW", error);
			test_stop();
		}
		clientid = (pid_t)pi.dwProcessId;
#elif defined(__unix__)
		clientid = fork();
		if (clientid == -1) {
			test_errno("Server-fork()", errno, 0);
			test_stop();
		} else if (clientid == 0) {
			// Child process
			if (execve(exec_filename, arguments, environ) == -1) {
				perror(exec_filename);
				_Exit(EXIT_FAILURE);
			}
		}
#else
#error "dispatch_io_net not implemented on this platform"
#endif

		addr2len = sizeof(struct sockaddr_in);
		clientfd = accept(sockfd, (struct sockaddr *)&addr2, &addr2len);
		if(clientfd == -1) {
			test_errno("Server-accept()", errno, 0);
			goto stop_test;
		}

		fprintf(stderr, "Server accepted connection. Server now writing\n");
#ifdef F_NOCACHE
		if (fcntl(read_fd, F_NOCACHE, 1)) {
			test_errno("fcntl F_NOCACHE", errno, 0);
			goto stop_test;
		}
#else
		// investigate what the impact of lack of file cache disabling has 
		// for this test
#endif
		size_t size = (size_t)dispatch_test_fd_lseek(read_fd, 0, SEEK_END);
		dispatch_test_fd_lseek(read_fd, 0, SEEK_SET);

		dispatch_group_t g = dispatch_group_create();
		dispatch_group_enter(g);
		dispatch_read(read_fd, size, dispatch_get_global_queue(0, 0),
				^(dispatch_data_t d, int r_err){
			fprintf(stderr, "Server-dispatch_read()\n");
			test_errno("Server-read error", r_err, 0);
			test_long("Server-dispatch data size", (long)dispatch_data_get_size(d),
					  (long)size);

			// convenience method handlers should only be called once
			if (dispatch_data_get_size(d)!= size) {
				fprintf(stderr, "Reading of data didn't complete\n");
				dispatch_test_fd_close(read_fd);
				closesocket(clientfd);
				closesocket(sockfd);
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
							"%zu bytes\n", dispatch_data_get_size(remaining));
					dispatch_test_fd_close(read_fd);
					closesocket(clientfd);
					closesocket(sockfd);
					test_stop();
				}
				closesocket(clientfd); // Sending the client EOF
				dispatch_group_leave(g);
			});
			dispatch_test_fd_close(read_fd);
			dispatch_group_leave(g);
		});
		test_group_wait(g);
		dispatch_release(g);
		fprintf(stderr, "Shutting down server\n");
		closesocket(sockfd);
		free(path);
		test_stop();

stop_test:
		if (path != NULL) {
			dispatch_test_release_large_file(path);
			free(path);
		}
		dispatch_test_fd_close(read_fd);
		closesocket(clientfd);
		closesocket(sockfd);
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
