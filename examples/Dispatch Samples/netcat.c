/*
 * Copyright (c) 2008 Apple Inc.  All rights reserved.
 *
 * @APPLE_DTS_LICENSE_HEADER_START@
 * 
 * IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
 * ("Apple") in consideration of your agreement to the following terms, and your
 * use, installation, modification or redistribution of this Apple software
 * constitutes acceptance of these terms.  If you do not agree with these terms,
 * please do not use, install, modify or redistribute this Apple software.
 * 
 * In consideration of your agreement to abide by the following terms, and
 * subject to these terms, Apple grants you a personal, non-exclusive license,
 * under Apple's copyrights in this original Apple software (the "Apple Software"),
 * to use, reproduce, modify and redistribute the Apple Software, with or without
 * modifications, in source and/or binary forms; provided that if you redistribute
 * the Apple Software in its entirety and without modifications, you must retain
 * this notice and the following text and disclaimers in all such redistributions
 * of the Apple Software.  Neither the name, trademarks, service marks or logos of
 * Apple Computer, Inc. may be used to endorse or promote products derived from
 * the Apple Software without specific prior written permission from Apple.  Except
 * as expressly stated in this notice, no other rights or licenses, express or
 * implied, are granted by Apple herein, including but not limited to any patent
 * rights that may be infringed by your derivative works or by other works in
 * which the Apple Software may be incorporated.
 * 
 * The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
 * COMBINATION WITH YOUR PRODUCTS. 
 * 
 * IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR
 * DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF
 * CONTRACT, TORT (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF
 * APPLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * @APPLE_DTS_LICENSE_HEADER_END@
 */

#include <dispatch/dispatch.h>
#include <Block.h>

#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <sysexits.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <mach/mach.h>
#include <pthread.h>

// #define DEBUG 1

#if DEBUG
#define dlog(a) dispatch_debug(a, #a)
#else
#define dlog(a) do { } while(0)
#endif

void usage(void);
void *run_block(void *);
void setup_fd_relay(int netfd /* bidirectional */,
					int infd /* local input */,
					int outfd /* local output */,
					void (^finalizer_block)(void));
void doreadwrite(int fd1, int fd2, char *buffer, size_t len);

#define BUFFER_SIZE 1099

int main(int argc, char *argv[]) {
	
	int ch;
	bool use_v4_only = false, use_v6_only = false;
	bool debug = false, no_stdin = false;
	bool keep_listening = false, do_listen = false;
	bool do_loookups = true, verbose = false;
	bool do_udp = false, do_bind_ip = false, do_bind_port = false;
	const char *hostname, *servname;
	int ret;
	struct addrinfo hints, *aires, *aires0;
	const char *bind_hostname, *bind_servname;
	
	dispatch_queue_t dq;
	dispatch_group_t listen_group = NULL;
	
	while ((ch = getopt(argc, argv, "46Ddhklnvup:s:")) != -1) {
		switch (ch) {
			case '4':
				use_v4_only = true;
				break;
			case '6':
				use_v6_only = true;
				break;
			case 'D':
				debug = true;
				break;
			case 'd':
				no_stdin = true;
				break;
			case 'h':
				usage();
				break;
			case 'k':
				keep_listening = true;
				break;
			case 'l':
				do_listen = true;
				break;
			case 'n':
				do_loookups = false;
				break;
			case 'v':
				verbose = true;
				break;
			case 'u':
				do_udp = true;
				break;
			case 'p':
				do_bind_port = true;
				bind_servname = optarg;
				break;
			case 's':
				do_bind_ip = true;
				bind_hostname = optarg;
				break;
			case '?':
			default:
				usage();
				break;
		}
	}
	
	argc -= optind;
	argv += optind;
	
	if (use_v4_only && use_v6_only) {
		errx(EX_USAGE, "-4 and -6 specified");
	}
	
	if (keep_listening && !do_listen) {
		errx(EX_USAGE, "-k specified but no -l");
	}
	
	if (do_listen && (do_bind_ip || do_bind_port)) {
		errx(EX_USAGE, "-p or -s option with -l");
	}
	
	if (do_listen) {
		if (argc >= 2) {
			hostname = argv[0];
			servname = argv[1];
		} else if (argc >= 1) {
			hostname = NULL;
			servname = argv[0];
		} else {
			errx(EX_USAGE, "No service name provided");
		}
	} else {
		if (argc >= 2) {
			hostname = argv[0];
			servname = argv[1];
		} else {
			errx(EX_USAGE, "No hostname and service name provided");
		}            
	}
	
	if (do_bind_ip || do_bind_port) {
		if (!do_bind_ip) {
			bind_hostname = NULL;
		}
		if (!do_bind_port) {
			bind_servname = NULL;
		}
	}
	
	openlog(getprogname(), LOG_PERROR|LOG_CONS, LOG_DAEMON);
	setlogmask(debug ? LOG_UPTO(LOG_DEBUG) : verbose ? LOG_UPTO(LOG_INFO) : LOG_UPTO(LOG_ERR));
	
	dq = dispatch_queue_create("netcat", NULL);
	listen_group = dispatch_group_create();
	
	bzero(&hints, sizeof(hints));
	hints.ai_family = use_v4_only ? PF_INET : (use_v6_only ? PF_INET6 : PF_UNSPEC);
	hints.ai_socktype = do_udp ? SOCK_DGRAM : SOCK_STREAM;
	hints.ai_protocol = do_udp ? IPPROTO_UDP : IPPROTO_TCP;
	hints.ai_flags = (!do_loookups ? AI_NUMERICHOST | AI_NUMERICSERV : 0) | (do_listen ? AI_PASSIVE : 0);
	
	ret = getaddrinfo(hostname, servname, &hints, &aires0);
	if (ret) {
		errx(1, "getaddrinfo(%s, %s): %s", hostname, servname, gai_strerror(ret));
	}
	
	for (aires = aires0; aires; aires = aires->ai_next) {
		if (do_listen) {
			// asynchronously set up the socket
			dispatch_retain(dq);
			dispatch_group_async(listen_group,
								 dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
								 ^{
									 int s, val = 1;
									 dispatch_source_t ds;
									 
									 s = socket(aires->ai_family, aires->ai_socktype, aires->ai_protocol);
									 if (s < 0) {
										 warn("socket");
										 return;
									 }
									 
									 if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val)) < 0) {
										 warn("Could not set SO_REUSEADDR");
									 }
									 
									 if(setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&val, sizeof(val)) < 0) {
										 warn("Could not set SO_REUSEPORT");
									 }
									 
									 if(setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val)) < 0) {
										 warn("Could not set SO_NOSIGPIPE");
									 }
									 
									 if (bind(s, aires->ai_addr, aires->ai_addrlen) < 0) {
										 warn("bind");
										 close(s);
										 return;
									 }
									 
									 listen(s, 2);
									 syslog(LOG_DEBUG, "listening on socket %d", s);
									 ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, s, 0, dq);
									 dispatch_source_set_event_handler(ds, ^{
																		  // got an incoming connection
																		  int s2, lfd = dispatch_source_get_handle(ds);
																		  dispatch_queue_t listen_queue = dispatch_get_current_queue();
																		  
																		  // prevent further accept(2)s across multiple sources
																		  dispatch_retain(listen_queue);
																		  dispatch_suspend(listen_queue);
																		  
																		  if (do_udp) {
																			  // lfd is our socket, but let's connect in the reverse
																			  // direction to set up the connection fully
																			  char udpbuf[4];
																			  struct sockaddr_storage sockin;
																			  socklen_t socklen;
																			  ssize_t peeklen;
																			  int cret;
																			  
																			  socklen = sizeof(sockin);
																			  peeklen = recvfrom(lfd, udpbuf, sizeof(udpbuf),
																								 MSG_PEEK, (struct sockaddr *)&sockin, &socklen);
																			  if (peeklen < 0) {
																				  warn("recvfrom");
																				  dispatch_resume(listen_queue);
																				  dispatch_release(listen_queue);																			
																				  return;
																			  }
																			  
																			  cret = connect(lfd, (struct sockaddr *)&sockin, socklen);
																			  if (cret < 0) {
																				  warn("connect");
																				  dispatch_resume(listen_queue);
																				  dispatch_release(listen_queue);																			
																				  return;																			
																			  }
																			  
																			  s2 = lfd;
																			  syslog(LOG_DEBUG, "accepted socket %d", s2);																		
																		  } else {
																			  s2 = accept(lfd, NULL, NULL);
																			  if (s2 < 0) {
																				  warn("accept");
																				  dispatch_resume(listen_queue);
																				  dispatch_release(listen_queue);
																				  return;
																			  }
																			  syslog(LOG_DEBUG, "accepted socket %d -> %d", lfd, s2);
																		  }
																		  
																		  
																		  setup_fd_relay(s2, no_stdin ? -1 : STDIN_FILENO, STDOUT_FILENO, ^{
																			  if (!do_udp) {
																				  close(s2);
																			  }
																			  dispatch_resume(listen_queue);
																			  dispatch_release(listen_queue);
																			  if (!keep_listening) {
																				  exit(0);
																			  }
																		  });
																	  });
									 dispatch_resume(ds);
									 dispatch_release(dq);
								 });
		} else {
			// synchronously try each address to try to connect
			__block bool did_connect = false;
			
			dispatch_sync(dq, ^{
				int s, val = 1;
				
				s = socket(aires->ai_family, aires->ai_socktype, aires->ai_protocol);
				if (s < 0) {
					warn("socket");
					return;
				}
				
				if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val)) < 0) {
					warn("Could not set SO_REUSEADDR");
				}
				
				if(setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&val, sizeof(val)) < 0) {
					warn("Could not set SO_REUSEPORT");
				}
				
				if(setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val)) < 0) {
					warn("Could not set SO_NOSIGPIPE");
				}
				
				if (do_bind_port || do_bind_ip) {
					struct addrinfo bhints, *bind_aires;
					int bret;
					in_port_t bport;
					
					bzero(&bhints, sizeof(bhints));
					bhints.ai_family = aires->ai_family;
					bhints.ai_socktype = aires->ai_socktype;
					bhints.ai_protocol = aires->ai_protocol;
					bhints.ai_flags = (do_bind_ip ? AI_NUMERICHOST : 0) | (do_bind_port ? AI_NUMERICSERV : 0) | AI_PASSIVE;
					
					bret = getaddrinfo(bind_hostname, bind_servname, &bhints, &bind_aires);
					if (bret) {
						warnx("getaddrinfo(%s, %s): %s", bind_hostname, bind_servname, gai_strerror(bret));
						close(s);
						freeaddrinfo(bind_aires);
						return;
					}
					
					switch(bind_aires->ai_family) {
						case PF_INET:
							bport = ((struct sockaddr_in *)bind_aires->ai_addr)->sin_port;
							break;
						case PF_INET6:
							bport = ((struct sockaddr_in6 *)bind_aires->ai_addr)->sin6_port;
							break;
						default:
							bport = htons(0);
							break;
					}
					
					if (ntohs(bport) > 0 && ntohs(bport) < IPPORT_RESERVED) {
						bret = bindresvport_sa(s, (struct sockaddr *)bind_aires->ai_addr);
					} else {
						bret = bind(s, bind_aires->ai_addr, bind_aires->ai_addrlen);
					}
					
					if (bret < 0) {
						warn("bind");
						close(s);
						freeaddrinfo(bind_aires);						
						return;
					}
					
					freeaddrinfo(bind_aires);
				}
				
				if (connect(s, aires->ai_addr, aires->ai_addrlen) < 0) {
					syslog(LOG_INFO, "connect to %s port %s (%s) failed: %s",
						   hostname,
						   servname,
						   aires->ai_protocol == IPPROTO_TCP ? "tcp" : aires->ai_protocol == IPPROTO_UDP ? "udp" : "unknown",
						   strerror(errno));
					close(s);
					return;
				}
				
				syslog(LOG_INFO, "Connection to %s %s port [%s] succeeded!",
					   hostname,
					   servname,
					   aires->ai_protocol == IPPROTO_TCP ? "tcp" : aires->ai_protocol == IPPROTO_UDP ? "udp" : "unknown");
				did_connect = true;
				
				if (do_udp) {
					// netcat sends a few bytes to set up the connection
					doreadwrite(-1, s, "XXXX", 4);
				}
				
				setup_fd_relay(s, no_stdin ? -1 : STDIN_FILENO, STDOUT_FILENO, ^{
					close(s);
					exit(0);
				});
			});
			
			if (did_connect) {
				break;
			}
		}
	}
	
	dispatch_group_wait(listen_group, DISPATCH_TIME_FOREVER);	
	freeaddrinfo(aires0);
	
	if (!do_listen && aires == NULL) {
		// got to the end of the address list without connecting
		exit(1);
	}
	
	dispatch_main();
	
	return 0;
}

void usage(void)
{
	fprintf(stderr, "Usage: %s [-4] [-6] [-D] [-d] [-h] [-k] [-l] [-n] [-v]\n", getprogname());
	fprintf(stderr, "       \t[-u] [-p <source_port>] [-s <source_ip>]\n");
	exit(EX_USAGE);
}

void *run_block(void *arg)
{
	void (^b)(void) = (void (^)(void))arg;
	
	b();
	
	_Block_release(arg);
	
	return NULL;
}

/*
 * Read up-to as much as is requested, and write
 * that to the other fd, taking into account exceptional
 * conditions and re-trying
 */
void doreadwrite(int fd1, int fd2, char *buffer, size_t len) {
	ssize_t readBytes, writeBytes, totalWriteBytes;
	
	if (fd1 != -1) {
		syslog(LOG_DEBUG, "trying to read %ld bytes from fd %d", len, fd1);
		readBytes = read(fd1, buffer, len);
		if (readBytes < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				/* can't do anything now, hope we get called again */
				syslog(LOG_DEBUG, "error read fd %d: %s (%d)", fd1, strerror(errno), errno);
				return;
			} else {
				err(1, "read fd %d", fd1);
			}
		} else if (readBytes == 0) {
			syslog(LOG_DEBUG, "EOF on fd %d", fd1);
			return;
		}
		syslog(LOG_DEBUG, "read %ld bytes from fd %d", readBytes, fd1);
	} else {
		readBytes = len;
		syslog(LOG_DEBUG, "read buffer has %ld bytes", readBytes);
	}
	
	totalWriteBytes = 0;
	do {
		writeBytes = write(fd2, buffer+totalWriteBytes, readBytes-totalWriteBytes);
		if (writeBytes < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			} else {
				err(1, "write fd %d", fd2);
			}
		}
		syslog(LOG_DEBUG, "wrote %ld bytes to fd %d", writeBytes, fd2);
		totalWriteBytes += writeBytes;
		
	} while (totalWriteBytes < readBytes);
	
	return;
}

/*
 * We set up dispatch sources for netfd and infd.
 * Since only one callback is called at a time per-source,
 * we don't need any additional serialization, and the network
 * and infd could be read from at the same time.
 */
void setup_fd_relay(int netfd /* bidirectional */,
					int infd /* local input */,
					int outfd /* local output */,
					void (^finalizer_block)(void))
{
	dispatch_source_t netsource = NULL, insource = NULL;
	
	dispatch_queue_t teardown_queue = dispatch_queue_create("teardown_queue", NULL);
	
	void (^finalizer_block_copy)(void) = _Block_copy(finalizer_block); // release after calling
	void (^cancel_hander)(dispatch_source_t source) = ^(dispatch_source_t source){
		dlog(source);
		dlog(teardown_queue);
		
		/*
		 * allowing the teardown queue to become runnable will get
		 * the teardown block scheduled, which will cancel all other
		 * sources and call the client-supplied finalizer
		 */
		dispatch_resume(teardown_queue);
		dispatch_release(teardown_queue);
	};
	void (^event_handler)(dispatch_source_t source, int wfd) = ^(dispatch_source_t source, int wfd) {
		int rfd = dispatch_source_get_handle(source);
		size_t bytesAvail = dispatch_source_get_data(source);
		char *buffer;
		
		syslog(LOG_DEBUG, "dispatch source %d -> %d has %lu bytes available",
			   rfd, wfd, bytesAvail);
		if (bytesAvail == 0) {
			dlog(source);
			dispatch_source_cancel(source);
			return;																											  
		}
		buffer = malloc(BUFFER_SIZE);
		doreadwrite(rfd,wfd, buffer, MIN(BUFFER_SIZE, bytesAvail+2));
		free(buffer);
	};
	
	/* 
	 * Suspend this now twice so that neither source can accidentally resume it
	 * while we're still setting up the teardown block. When either source
	 * gets an EOF, the queue is resumed so that it can teardown the other source
	 * and call the client-supplied finalizer
	 */
	dispatch_suspend(teardown_queue);
	dispatch_suspend(teardown_queue);
	
	if (infd != -1) {
		dispatch_retain(teardown_queue); // retain so that we can resume in this block later
		
		dlog(teardown_queue);
		
		// since the event handler serializes, put this on a concurrent queue
		insource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, infd, 0, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
		dispatch_source_set_event_handler(insource, ^{ event_handler(insource, netfd); });
		dispatch_source_set_cancel_handler(insource, ^{ cancel_hander(insource); });
		dispatch_resume(insource);
		dlog(insource);
	}
	
	dispatch_retain(teardown_queue); // retain so that we can resume in this block later
	
	dlog(teardown_queue);
	
	// since the event handler serializes, put this on a concurrent queue
	netsource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, netfd, 0, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
	dispatch_source_set_event_handler(netsource, ^{ event_handler(netsource, outfd); });
	dispatch_source_set_cancel_handler(netsource, ^{ cancel_hander(netsource); });
	dispatch_resume(netsource);
	dlog(netsource);
	
	dispatch_async(teardown_queue, ^{
		syslog(LOG_DEBUG, "Closing connection on fd %d -> %d -> %d", infd, netfd, outfd);
		
		if (insource) {
			dlog(insource);
			dispatch_source_cancel(insource);
			dispatch_release(insource); // matches initial create
			dlog(insource);
		}
		
		dlog(netsource);
		dispatch_source_cancel(netsource);
		dispatch_release(netsource); // matches initial create
		dlog(netsource);
		
		dlog(teardown_queue);
		
		finalizer_block_copy();
		_Block_release(finalizer_block_copy);
	});
	
	/* Resume this once so their either source can do the second resume
	 * to start the teardown block running
	 */
	dispatch_resume(teardown_queue);
	dispatch_release(teardown_queue); // matches initial create
	dlog(teardown_queue);
}

