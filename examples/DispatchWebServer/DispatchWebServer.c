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

/* A tiny web server that does as much stuff the "dispatch way" as it can, like a queue per connection... */

/****************************************************************************
overview of dispatch related operations:

main() {
	have dump_reqs() called every 5 to 6 seconds, and on every SIGINFO 
	and SIGPIPE

	have accept_cb() called when there are new connections on our port

	have reopen_logfile_when_needed() called whenever our logfile is
	renamed, deleted, or forcibly closed
}

reopen_logfile_when_needed() {
	call ourself whenever our logfile is renamed, deleted, or forcibly 
	closed
}

accept_cb() {
	allocate a new queue to handle network and file I/O, and timers
	for a series of HTTP requests coming from a new network connection
	
	have read_req() called (on the new queue) when there
	is network traffic for the new connection

	have req_free(new_req) called when the connection is "done" (no
	pending work to be executed on the queue, an no sources left to
	generate new work for the queue)
}

req_free() {
	uses dispatch_get_current_queue() and dispatch_async() to call itself
	"on the right queue"
}

read_req() {
	If there is a timeout source delete_source() it

	if (we have a whole request) {
		make a new dispatch source (req->fd_rd.ds) for the
		content file

		have clean up fd, req->fd and req->fd_rd (if
		appropriate) when the content file source is canceled

		have read_filedata called when the content file is
		read to be read

		if we already have a dispatch source for "network
		socket ready to be written", enable it.  Otherwise
		make one, and have write_filedata called when it
		time to write to it.

		disable the call to read_req
	}

	close the connection if something goes wrong
}

write_filedata() {
	close the connection if anything goes wrong

	if (we have written the whole HTTP document) {
		timeout in a little bit, closing the connection if we 
		haven't received a new command

		enable the call to read_req
	}

	if (we have written all the buffered data) {
		disable the call to write_filedata()
	}
}

read_filedata() {
	if (nothing left to read) {
		delete the content file dispatch source
	} else {
		enable the call to write_filedata()
	}
}

qprintf, qfprintf, qflush
	schedule stdio calls on a single queue

disable_source, enable_source
	implements a binary enable/disable on top of dispatch's
	counted suspend/resume

delete_source
	cancels the source (this example program uses source
	cancelation to schedule any source cleanup it needs,
	so "delete" needs a cancel).

	ensure the source isn't suspended

	release the reference, which _should_ be the last
	reference (this example program never has more
	then one reference to a source)

****************************************************************************/

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <stdarg.h>
#include <assert.h>
#include <netinet/in.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <regex.h>
#include <time.h>
#include <malloc/malloc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <dispatch/dispatch.h>
#include <Block.h>
#include <errno.h>

char *DOC_BASE = NULL;
char *log_name = NULL;
FILE *logfile = NULL;
char *argv0 = "a.out";
char *server_port = "8080";
const int re_request_nmatch = 4;
regex_t re_first_request, re_nth_request, re_accept_deflate, re_host;


// qpf is the queue that we schedule our "stdio file I/O", which serves as a lock,
// and orders the output, and also gets it "out of the way" of our main line execution
dispatch_queue_t qpf;

void qfprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void qfprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *str;
    /* We gennerate the formatted string on the same queue (or
      thread) that calls qfprintf, that way the values can change
      while the fputs call is being sent to the qpf queue, or waiting
      for other work to complete ont he qpf queue. */

    vasprintf(&str, fmt, ap);
    dispatch_async(qpf, ^{ fputs(str, f); free(str); });
    if ('*' == *fmt) {
	    dispatch_sync(qpf, ^{ fflush(f); });
    }
    va_end(ap);
}

void qfflush(FILE *f) {
	dispatch_sync(qpf, ^{ fflush(f); });
}

void reopen_logfile_when_needed() {
    // We don't want to use a fd with a lifetime managed by something else
    // because we need to close it inside the cancel handler (see below)
    int lf_dup = dup(fileno(logfile));
    FILE **lf = &logfile;

    // We register the vnode callback on the qpf queue since that is where
    // we do all our logfile printing.   (we set up to reopen the logfile
    // if the "old one" has been deleted or renamed (or revoked).  This
    // makes it pretty safe to mv the file to a new name, delay breifly,
    // then gzip it.   Safer to move the file to a new name, wait for the
    // "old" file to reappear, then gzip.   Niftier then doing the move,
    // sending a SIGHUP to the right process (somehow) and then doing
    // as above.    Well, maybe it'll never catch on as "the new right 
    /// thing", but it makes a nifty demo.
    dispatch_source_t vn = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, lf_dup, DISPATCH_VNODE_REVOKE|DISPATCH_VNODE_RENAME|DISPATCH_VNODE_DELETE, qpf);

    dispatch_source_set_event_handler(vn, ^{
	printf("lf_dup is %d (logfile's fileno=%d), closing it\n", lf_dup, fileno(logfile));
	fprintf(logfile, "# flush n' roll!\n");
	dispatch_cancel(vn);
	dispatch_release(vn);
	fflush(logfile);
	*lf = freopen(log_name, "a", logfile);

	// The new logfile has (or may have) a diffrent fd from the old one, so
	// we have to register it again
	reopen_logfile_when_needed();
    });

    dispatch_source_set_cancel_handler(vn, ^{ close(lf_dup); });

    dispatch_resume(vn);
}

#define qprintf(fmt...) qfprintf(stdout, ## fmt);

struct buffer {
    // Manage a buffer, currently at sz bytes, but will realloc if needed
    // The buffer has a part that we read data INTO, and a part that we
    // write data OUT OF.
    //
    // Best use of the space would be a circular buffer (and we would
    // use readv/writev and pass around iovec structs), but we use a
    // simpler layout:
    //   data from buf to outof is wasted.   From outof to into is
    //   "ready to write data OUT OF", from into until buf+sz is
    //   "ready to read data IN TO".
    size_t sz;
    unsigned char *buf;
    unsigned char *into, *outof;
};

struct request_source {
	// libdispatch gives suspension a counting behaviour, we want a simple on/off behaviour, so we use
	// this struct to provide track suspensions
	dispatch_source_t ds;
	bool suspended;
};

// The request struct manages an actiave HTTP request/connection.   It gets reused for pipelined HTTP clients.
// Every request has it's own queue where all of it's network traffic, and source file I/O as well as
// compression (when requested by the HTTP client) is done.
struct request {
    struct sockaddr_in r_addr;
    z_stream *deflate;
    // cmd_buf holds the HTTP request
    char cmd_buf[8196], *cb;
    char chunk_num[13], *cnp;  // Big enough for 8 digits plus \r\n\r\n\0
    bool needs_zero_chunk;
    bool reuse_guard;
    short status_number;
    size_t chunk_bytes_remaining;
    char *q_name;
    int req_num;    // For debugging
    int files_served;	// For this socket
    dispatch_queue_t q;
    // "sd" is the socket descriptor, where the network I/O for this request goes.   "fd" is the source file (or -1)
    int sd, fd;
    // fd_rd is for read events from the source file (say /Users/YOU/Sites/index.html for a GET /index.html request)
    // sd_rd is for read events from the network socket (we suspend it after we read an HTTP request header, and
    // resume it when we complete a request)
    // sd_wr is for write events to the network socket (we suspend it when we have no buffered source data to send,
    // and resume it when we have data ready to send)
    // timeo is the timeout event waiting for a new client request header.
    struct request_source fd_rd, sd_rd, sd_wr, timeo;
    uint64_t timeout_at;
    struct stat sb;

    // file_b is where we read data from fd into.
    // For compressed GET requests:
    //  - data is compressed from file_b into deflate_b
    //  - data is written to the network socket from deflate_b
    // For uncompressed GET requests
    //  - data is written to the network socket from file_b
    //  - deflate_b is unused
    struct buffer file_b, deflate_b;

    ssize_t total_written;
};

void req_free(struct request *req);

void disable_source(struct request *req, struct request_source *rs) {
    // we want a binary suspend state, not a counted state.   Our
    // suspend flag is "locked" by only being used on req->q, this
    // assert makes sure we are in a valid context to write the new
    // suspend value.
    assert(req->q == dispatch_get_current_queue());
    if (!rs->suspended) {
	    rs->suspended = true;
	    dispatch_suspend(rs->ds);
    }
}

void enable_source(struct request *req, struct request_source *rs) {
    assert(req->q == dispatch_get_current_queue());
    if (rs->suspended) {
	    rs->suspended = false;
	    dispatch_resume(rs->ds);
    }
}

void delete_source(struct request *req, struct request_source *rs) {
    assert(req->q == dispatch_get_current_queue());
    if (rs->ds) {
	    /* sources need to be resumed before they can be deleted
	      (otherwise an I/O and/or cancel block might be stranded
	      waiting for a resume that will never come, causing
	      leaks) */

	    enable_source(req, rs);
	    dispatch_cancel(rs->ds);
	    dispatch_release(rs->ds);
    }
    rs->ds = NULL;
    rs->suspended = false;
}

size_t buf_into_sz(struct buffer *b) {
    return (b->buf + b->sz) - b->into;
}

void buf_need_into(struct buffer *b, size_t cnt) {
    // resize buf so into has at least cnt bytes ready to use
    size_t sz = buf_into_sz(b);
    if (cnt <= sz) {
	return;
    }
    sz = malloc_good_size(cnt - sz + b->sz);
    unsigned char *old = b->buf;
    // We could special case b->buf == b->into && b->into == b->outof to
    // do a free & malloc rather then realloc, but after testing it happens
    // only for the 1st use of the buffer, where realloc is the same cost as
    // malloc anyway.
    b->buf = reallocf(b->buf, sz);
    assert(b->buf);
    b->sz = sz;
    b->into = b->buf + (b->into - old);
    b->outof = b->buf + (b->outof - old);
}

void buf_used_into(struct buffer *b, size_t used) {
    b->into += used;
    assert(b->into <= b->buf + b->sz);
}

size_t buf_outof_sz(struct buffer *b) {
    return b->into - b->outof;
}

int buf_sprintf(struct buffer *b, char *fmt, ...) __attribute__((format(printf,2,3)));

int buf_sprintf(struct buffer *b, char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t s = buf_into_sz(b);
    int l = vsnprintf((char *)(b->into), s, fmt, ap);
    if (l < s) {
	buf_used_into(b, l);
    } else {
	// Reset ap -- vsnprintf has already used it.
	va_end(ap);
	va_start(ap, fmt);
	buf_need_into(b, l);
	s = buf_into_sz(b);
	l = vsnprintf((char *)(b->into), s, fmt, ap);
	assert(l <= s);
	buf_used_into(b, l);
    }
    va_end(ap);

    return l;
}

void buf_used_outof(struct buffer *b, size_t used) {
    b->outof += used;
    //assert(b->into <= b->outof);
    assert(b->outof <= b->into);
    if (b->into == b->outof) {
	b->into = b->outof = b->buf;
    }
}

char *buf_debug_str(struct buffer *b) {
    char *ret = NULL;
    asprintf(&ret, "S%d i#%d o#%d", b->sz, buf_into_sz(b), buf_outof_sz(b));
    return ret;
}

uint64_t getnanotime() {
	struct timeval tv;
	gettimeofday(&tv, NULL);

	return tv.tv_sec * NSEC_PER_SEC + tv.tv_usec * NSEC_PER_USEC;
}

int n_req;
struct request **debug_req;

void dump_reqs() {
    int i = 0;
    static last_reported = -1;

    // We want to see the transition into n_req == 0, but we don't need to
    // keep seeing it.
    if (n_req == 0 && n_req == last_reported) {
	    return;
    } else {
	    last_reported = n_req;
    }

    qprintf("%d actiave requests to dump\n", n_req);
    uint64_t now = getnanotime();
    /* Because we iterate over the debug_req array in this queue
      ("the main queue"), it has to "own" that array.   All manipulation
      of the array as a whole will have to be done on this queue.  */

    for(i = 0; i < n_req; i++) {
	struct request *req = debug_req[i];
	qprintf("%s sources: fd_rd %p%s, sd_rd %p%s, sd_wr %p%s, timeo %p%s\n", req->q_name, req->fd_rd.ds, req->fd_rd.suspended ? " (SUSPENDED)" : "", req->sd_rd.ds, req->sd_rd.suspended ? " (SUSPENDED)" : "", req->sd_wr.ds, req->sd_wr.suspended ? " (SUSPENDED)" : "", req->timeo.ds, req->timeo.suspended ? " (SUSPENDED)" : "");
	if (req->timeout_at) {
		double when = req->timeout_at - now;
		when /= NSEC_PER_SEC;
		if (when < 0) {
			qprintf("  timeout %f seconds ago\n", -when);
		} else {
			qprintf("  timeout in %f seconds\n", when);
		}
	} else {
		qprintf("  timeout_at not set\n");
	}
	char *file_bd = buf_debug_str(&req->file_b), *deflate_bd = buf_debug_str(&req->deflate_b);
	qprintf("  file_b %s; deflate_b %s\n  cmd_buf used %ld; fd#%d; files_served %d\n", file_bd, deflate_bd, (long)(req->cb - req->cmd_buf), req->fd, req->files_served);
	if (req->deflate) {
		qprintf("  deflate total in: %ld ", req->deflate->total_in);
	}
	qprintf("%s total_written %lu, file size %lld\n", req->deflate ? "" : " ", req->total_written, req->sb.st_size);
	free(file_bd);
	free(deflate_bd);
    }
}

void req_free(struct request *req) {
    assert(!req->reuse_guard);
    if (dispatch_get_main_queue() != dispatch_get_current_queue()) {
	    /* dispatch_set_finalizer_f arranges to have us "invoked
	      asynchronously on req->q's target queue".  However,
	      we want to manipulate the debug_req array in ways
	      that are unsafe anywhere except the same queue that
	      dump_reqs runs on (which happens to be the main queue).
	      So if we are running anywhere but the main queue, we
	      just arrange to be called there */

	    dispatch_async(dispatch_get_main_queue(), ^{ req_free(req); });
	    return;
    }

    req->reuse_guard = true;
    *(req->cb) = '\0';
    qprintf("$$$ req_free %s; fd#%d; buf: %s\n", dispatch_queue_get_label(req->q), req->fd, req->cmd_buf);
    assert(req->sd_rd.ds == NULL && req->sd_wr.ds == NULL);
    close(req->sd);
    assert(req->fd_rd.ds == NULL);
    if (req->fd >= 0) close(req->fd);
    free(req->file_b.buf);
    free(req->deflate_b.buf);
    free(req->q_name);
    free(req->deflate);
    free(req);

    int i;
    bool found = false;
    for(i = 0; i < n_req; i++) {
	if (found) {
	    debug_req[i -1] = debug_req[i];
	} else {
	    found = (debug_req[i] == req);
	}
    }
    debug_req = reallocf(debug_req, sizeof(struct request *) * --n_req);
    assert(n_req >= 0);
}

void close_connection(struct request *req) {
    qprintf("$$$ close_connection %s, served %d files -- canceling all sources\n", dispatch_queue_get_label(req->q), req->files_served);
    delete_source(req, &req->fd_rd);
    delete_source(req, &req->sd_rd);
    delete_source(req, &req->sd_wr);
    delete_source(req, &req->timeo);
}

// We have some "content data" (either from the file, or from
// compressing the file), and the network socket is ready for us to
// write it
void write_filedata(struct request *req, size_t avail) {
    /* We always attempt to write as much data as we have.   This
     is safe becuase we use non-blocking I/O.   It is a good idea
     becuase the amount of buffer space that dispatch tells us may
     be stale (more space could have opened up, or memory presure
     may have caused it to go down). */

    struct buffer *w_buf = req->deflate ? &req->deflate_b : &req->file_b;
    ssize_t sz = buf_outof_sz(w_buf);
    if (req->deflate) {
	struct iovec iov[2];
	if (!req->chunk_bytes_remaining) {
	    req->chunk_bytes_remaining = sz;
	    req->needs_zero_chunk = sz != 0;
	    req->cnp = req->chunk_num;
	    int n = snprintf(req->chunk_num, sizeof(req->chunk_num), "\r\n%lx\r\n%s", sz, sz ? "" : "\r\n");
	    assert(n <= sizeof(req->chunk_num));
	}
	iov[0].iov_base = req->cnp;
	iov[0].iov_len = req->cnp ? strlen(req->cnp) : 0;
	iov[1].iov_base = w_buf->outof;
	iov[1].iov_len = (req->chunk_bytes_remaining < sz) ? req->chunk_bytes_remaining : sz;
	sz = writev(req->sd, iov, 2);
	if (sz > 0) {
	    if (req->cnp) {
		if (sz >= strlen(req->cnp)) {
		    req->cnp = NULL;
		} else {
		    req->cnp += sz;
		}
	    }
	    sz -= iov[0].iov_len;
	    sz = (sz < 0) ? 0 : sz;
	    req->chunk_bytes_remaining -= sz;
	}
    } else {
	sz = write(req->sd, w_buf->outof, sz);
    }
    if (sz > 0) {
	buf_used_outof(w_buf, sz);
    } else if (sz < 0) {
	int e = errno;
	qprintf("write_filedata %s write error: %d %s\n", dispatch_queue_get_label(req->q), e, strerror(e));
	close_connection(req);
	return;
    }

    req->total_written += sz;
    off_t bytes = req->total_written;
    if (req->deflate) {
	bytes = req->deflate->total_in - buf_outof_sz(w_buf);
	if (req->deflate->total_in < buf_outof_sz(w_buf)) {
	    bytes = 0;
	}
    }
    if (bytes == req->sb.st_size) {
	if (req->needs_zero_chunk && req->deflate && (sz || req->cnp)) {
	    return;
	}

	// We have transfered the file, time to write the log entry.

	// We don't deal with " in the request string, this is an example of how
	// to use dispatch, not how to do C string manipulation, eh?
	size_t rlen = strcspn(req->cmd_buf, "\r\n");
	char tstr[45], astr[45];
	struct tm tm;
	time_t clock;
	time(&clock);
	strftime(tstr, sizeof(tstr), "%d/%b/%Y:%H:%M:%S +0", gmtime_r(&clock, &tm));
	addr2ascii(AF_INET, &req->r_addr.sin_addr, sizeof(struct in_addr), astr);
	qfprintf(logfile, "%s - - [%s] \"%.*s\" %hd %zd\n", astr, tstr, (int)rlen, req->cmd_buf, req->status_number, req->total_written);

	int64_t t_offset = 5 * NSEC_PER_SEC + req->files_served * NSEC_PER_SEC / 10;
	int64_t timeout_at = req->timeout_at = getnanotime() + t_offset;

	req->timeo.ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, req->q);
	dispatch_source_set_timer(req->timeo.ds, dispatch_time(DISPATCH_TIME_NOW, t_offset), NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(req->timeo.ds, ^{
			if (req->timeout_at == timeout_at) {
				qfprintf(stderr, "$$$ -- timeo fire (delta=%f) -- close connection: q=%s\n", (getnanotime() - (double)timeout_at) / NSEC_PER_SEC, dispatch_queue_get_label(req->q));
				close_connection(req);
			} else {
				// This happens if the timeout value has been updated, but a pending timeout event manages to race in before the cancel
			}
		});
	dispatch_resume(req->timeo.ds);

	req->files_served++;
	qprintf("$$$ wrote whole file (%s); timeo %p, about to enable %p and close %d, total_written=%zd, this is the %d%s file served\n", dispatch_queue_get_label(req->q), req->timeo.ds, req->sd_rd.ds, req->fd, req->total_written, req->files_served, (1 == req->files_served) ? "st" : (2 == req->files_served) ? "nd" : "th");
	enable_source(req, &req->sd_rd);
	if (req->fd_rd.ds) {
		delete_source(req, &req->fd_rd);
	}
	req->cb = req->cmd_buf;
    } else {
	assert(bytes <= req->sb.st_size);
    }

    if (0 == buf_outof_sz(w_buf)) {
	// The write buffer is now empty, so we don't need to know when sd is ready for us to write to it.
	disable_source(req, &req->sd_wr);
    }
}

// Our "content file" has some data ready for us to read.
void read_filedata(struct request *req, size_t avail) {
    if (avail == 0) {
	    delete_source(req, &req->fd_rd);
	    return;
    }

    /* We make sure we can read at least as many bytes as dispatch
      says are avilable, but if our buffer is bigger we will read as
      much as we have space for.  We have the file opened in non-blocking
      mode so this is safe. */

    buf_need_into(&req->file_b, avail);
    size_t rsz = buf_into_sz(&req->file_b);
    ssize_t sz = read(req->fd, req->file_b.into, rsz);
    if (sz >= 0) {
	assert(req->sd_wr.ds);
	size_t sz0 = buf_outof_sz(&req->file_b);
	buf_used_into(&req->file_b, sz);
	assert(sz == buf_outof_sz(&req->file_b) - sz0);
    } else {
	int e = errno;
	qprintf("read_filedata %s read error: %d %s\n", dispatch_queue_get_label(req->q), e, strerror(e));
	close_connection(req);
	return;
    }
    if (req->deflate) {
	// Note:: deflateBound is "worst case", we could try with any non-zero
	// buffer, and alloc more if we get Z_BUF_ERROR...
	buf_need_into(&req->deflate_b, deflateBound(req->deflate, buf_outof_sz(&req->file_b)));
	req->deflate->next_in = (req->file_b.outof);
	size_t o_sz = buf_outof_sz(&req->file_b);
	req->deflate->avail_in = o_sz;
	req->deflate->next_out = req->deflate_b.into;
	size_t i_sz = buf_into_sz(&req->deflate_b);
	req->deflate->avail_out = i_sz;
	assert(req->deflate->avail_in + req->deflate->total_in <= req->sb.st_size);
	// at EOF we want to use Z_FINISH, otherwise we pass Z_NO_FLUSH so we get maximum compression
	int rc = deflate(req->deflate, (req->deflate->avail_in + req->deflate->total_in >= req->sb.st_size) ? Z_FINISH : Z_NO_FLUSH);
	assert(rc == Z_OK || rc == Z_STREAM_END);
	buf_used_outof(&req->file_b, o_sz - req->deflate->avail_in);
	buf_used_into(&req->deflate_b, i_sz - req->deflate->avail_out);
	if (i_sz != req->deflate->avail_out) {
	    enable_source(req, &req->sd_wr);
	}
    } else {
	enable_source(req, &req->sd_wr);
    }
}

// We are waiting to for an HTTP request (we eitther havn't gotten
// the first request, or pipelneing is on, and we finished a request),
// and there is data to read on the network socket.
void read_req(struct request *req, size_t avail) {
    if (req->timeo.ds) {
	delete_source(req, &req->timeo);
    }

    // -1 to account for the trailing NUL
    int s = (sizeof(req->cmd_buf) - (req->cb - req->cmd_buf)) -1;
    if (s == 0) {
	qprintf("read_req fd#%d command overflow\n", req->sd);
	close_connection(req);
	return;
    }
    int rd = read(req->sd, req->cb, s);
    if (rd > 0) {
	req->cb += rd;
	if (req->cb > req->cmd_buf + 4) {
	    int i;
	    for(i = -4; i != 0; i++) {
		char ch = *(req->cb + i);
		if (ch != '\n' && ch != '\r') {
		    break;
		}
	    }
	    if (i == 0) {
		*(req->cb) = '\0';

		assert(buf_outof_sz(&req->file_b) == 0);
		assert(buf_outof_sz(&req->deflate_b) == 0);
		regmatch_t pmatch[re_request_nmatch];
		regex_t *rex = req->files_served ? &re_first_request : &re_nth_request;
		int rc = regexec(rex, req->cmd_buf, re_request_nmatch, pmatch, 0);
		if (rc) {
		    char ebuf[1024];
		    regerror(rc, rex, ebuf, sizeof(ebuf));
		    qprintf("\n$$$ regexec error: %s, ditching request: '%s'\n", ebuf, req->cmd_buf);
		    close_connection(req);
		    return;
		} else {
		    if (!strncmp("GET", req->cmd_buf + pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so)) {
			rc = regexec(&re_accept_deflate, req->cmd_buf, 0, NULL, 0);
			assert(rc == 0 || rc == REG_NOMATCH);
			// to disable deflate code:
			// rc = REG_NOMATCH;
			if (req->deflate) {
			    deflateEnd(req->deflate);
			    free(req->deflate);
			}
			req->deflate = (0 == rc) ? calloc(1, sizeof(z_stream)) : NULL;
			char path_buf[4096];
			strlcpy(path_buf, DOC_BASE, sizeof(path_buf));
			// WARNING: this doesn't avoid use of .. in the path
			// do get outside of DOC_ROOT, a real web server would
			// really have to avoid that.
			char ch = *(req->cmd_buf + pmatch[2].rm_eo);
			*(req->cmd_buf + pmatch[2].rm_eo) = '\0';
			strlcat(path_buf, req->cmd_buf + pmatch[2].rm_so, sizeof(path_buf));
			*(req->cmd_buf + pmatch[2].rm_eo) = ch;
			req->fd = open(path_buf, O_RDONLY|O_NONBLOCK);
			qprintf("GET req for %s, path: %s, deflate: %p; fd#%d\n", dispatch_queue_get_label(req->q), path_buf, req->deflate, req->fd);
			size_t n;
			if (req->fd < 0) {
			    const char *msg = "<HTML><HEAD><TITLE>404 Page not here</TITLE></HEAD><BODY><P>You step in the stream,<BR>but the water has moved on.<BR>This <B>page is not here</B>.<BR></BODY></HTML>";
			    req->status_number = 404;
			    n = buf_sprintf(&req->file_b, "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\nExpires: now\r\nServer: %s\r\n\r\n%s", strlen(msg), argv0, msg);
			    req->sb.st_size = 0;
			} else {
			    rc = fstat(req->fd, &req->sb);
			    assert(rc >= 0);
			    if (req->sb.st_mode & S_IFDIR) {
				req->status_number = 301;
				regmatch_t hmatch[re_request_nmatch];
				rc = regexec(&re_host, req->cmd_buf, re_request_nmatch, hmatch, 0);
				assert(rc == 0 || rc == REG_NOMATCH);
				if (rc == REG_NOMATCH) {
				    hmatch[1].rm_so = hmatch[1].rm_eo = 0;
				}
				n = buf_sprintf(&req->file_b, "HTTP/1.1 301 Redirect\r\nContent-Length: 0\r\nExpires: now\r\nServer: %s\r\nLocation: http://%*.0s/%*.0s/index.html\r\n\r\n", argv0, (int)(hmatch[1].rm_eo - hmatch[1].rm_so), req->cmd_buf + hmatch[1].rm_so, (int)(pmatch[2].rm_eo - pmatch[2].rm_so), req->cmd_buf + pmatch[2].rm_so);
				req->sb.st_size = 0;
				close(req->fd);
				req->fd = -1;
			    } else {
				req->status_number = 200;
				if (req->deflate) {
				    n = buf_sprintf(&req->deflate_b, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Encoding: deflate\r\nExpires: now\r\nServer: %s\r\n", argv0);
				    req->chunk_bytes_remaining = buf_outof_sz(&req->deflate_b);
				} else {
				    n = buf_sprintf(req->deflate ? &req->deflate_b : &req->file_b, "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\nExpires: now\r\nServer: %s\r\n\r\n", req->sb.st_size, argv0);
				}
			    }
			}

			if (req->status_number != 200) {
			    free(req->deflate);
			    req->deflate = NULL;
			}

			if (req->deflate) {
			    rc = deflateInit(req->deflate, Z_BEST_COMPRESSION);
			    assert(rc == Z_OK);
			}

			// Cheat: we don't count the header bytes as part of total_written
			req->total_written = -buf_outof_sz(&req->file_b);
			if (req->fd >= 0) {
			    req->fd_rd.ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, req->fd, 0, req->q);
			    // Cancelation is async, so we capture the fd and read sources we will want to operate on as the req struct may have moved on to a new set of values
			    int fd = req->fd;
			    dispatch_source_t fd_rd = req->fd_rd.ds;
			    dispatch_source_set_cancel_handler(req->fd_rd.ds, ^{
				    close(fd);
				    if (req->fd == fd) {
					    req->fd = -1;
				    }
				    if (req->fd_rd.ds == fd_rd) {
					    req->fd_rd.ds = NULL;
				    }
			    });
			    dispatch_source_set_event_handler(req->fd_rd.ds, ^{
				    if (req->fd_rd.ds) {
					    read_filedata(req, dispatch_source_get_data(req->fd_rd.ds)); 
				    }
			    });
			    dispatch_resume(req->fd_rd.ds);
			} else {
			    req->fd_rd.ds = NULL;
			}

			if (req->sd_wr.ds) {
			    enable_source(req, &req->sd_wr);
			} else {
			    req->sd_wr.ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, req->sd, 0, req->q);
			    dispatch_source_set_event_handler(req->sd_wr.ds, ^{ write_filedata(req, dispatch_source_get_data(req->sd_wr.ds)); });
			    dispatch_resume(req->sd_wr.ds);
			}
			disable_source(req, &req->sd_rd);
		    }
		}
	    }
	}
    } else if (rd == 0) {
	qprintf("### (%s) read_req fd#%d rd=0 (%s); %d files served\n", dispatch_queue_get_label(req->q), req->sd, (req->cb == req->cmd_buf) ? "no final request" : "incomplete request", req->files_served);
	close_connection(req);
	return;
    } else {
	int e = errno;
	qprintf("reqd_req fd#%d rd=%d err=%d %s\n", req->sd, rd, e, strerror(e));
	close_connection(req);
	return;
    }
}

// We have a new connection, allocate a req struct & set up a read event handler
void accept_cb(int fd) {
    static int req_num = 0;
    struct request *new_req = calloc(1, sizeof(struct request));
    assert(new_req);
    new_req->cb = new_req->cmd_buf;
    socklen_t r_len = sizeof(new_req->r_addr);
    int s = accept(fd, (struct sockaddr *)&(new_req->r_addr), &r_len);
    if (s < 0) {
	    qfprintf(stderr, "accept failure (rc=%d, errno=%d %s)\n", s, errno, strerror(errno));
	    return;
    }
    assert(s >= 0);
    new_req->sd = s;
    new_req->req_num = req_num;
    asprintf(&(new_req->q_name), "req#%d s#%d", req_num++, s);
    qprintf("accept_cb fd#%d; made: %s\n", fd, new_req->q_name);

    // All further work for this request will happen "on" new_req->q,
    // except the final tear down (see req_free())
    new_req->q = dispatch_queue_create(new_req->q_name, NULL);
    dispatch_set_context(new_req->q, new_req);
    dispatch_set_finalizer_f(new_req->q, (dispatch_function_t)req_free);

    debug_req = reallocf(debug_req, sizeof(struct request *) * ++n_req);
    debug_req[n_req -1] = new_req;

    
    new_req->sd_rd.ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, new_req->sd, 0, new_req->q);
    dispatch_source_set_event_handler(new_req->sd_rd.ds, ^{
	    read_req(new_req, dispatch_source_get_data(new_req->sd_rd.ds));
    });

    // We want our queue to go away when all of it's sources do, so we
    // drop the reference dispatch_queue_create gave us & rely on the
    // references each source holds on the queue to keep it alive.
    dispatch_release(new_req->q);
    dispatch_resume(new_req->sd_rd.ds);
}

int main(int argc, char *argv[]) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock > 0);
    int rc;
    struct addrinfo ai_hints, *my_addr;

    qpf = dispatch_queue_create("printf", NULL);

    argv0 = basename(argv[0]);
    struct passwd *pw = getpwuid(getuid());
    assert(pw);
    asprintf(&DOC_BASE, "%s/Sites/", pw->pw_dir);
    asprintf(&log_name, "%s/Library/Logs/%s-transfer.log", pw->pw_dir, argv0);
    logfile = fopen(log_name, "a");
    reopen_logfile_when_needed(logfile, log_name);

    bzero(&ai_hints, sizeof(ai_hints));
    ai_hints.ai_flags = AI_PASSIVE;
    ai_hints.ai_family = PF_INET;
    ai_hints.ai_socktype = SOCK_STREAM;
    ai_hints.ai_protocol = IPPROTO_TCP;
    rc = getaddrinfo(NULL, server_port, &ai_hints, &my_addr);
    assert(rc == 0);

    qprintf("Serving content from %s on port %s, logging transfers to %s\n", DOC_BASE, server_port, log_name);

    int yes = 1;
    rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    assert(rc == 0);
    yes = 1;
    rc = setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    assert(rc == 0);

    rc = bind(sock, my_addr->ai_addr, my_addr->ai_addr->sa_len);
    assert(rc >= 0);

    rc = listen(sock, 25);
    assert(rc >= 0);

    rc = regcomp(&re_first_request, "^([A-Z]+)[ \t]+([^ \t\n]+)[ \t]+HTTP/1\\.1[\r\n]+", REG_EXTENDED);
    assert(rc == 0);

    rc = regcomp(&re_nth_request, "^([A-Z]+)[ \t]+([^ \t\n]+)([ \t]+HTTP/1\\.1)?[\r\n]+", REG_EXTENDED);
    assert(rc == 0);

    rc = regcomp(&re_accept_deflate, "[\r\n]+Accept-Encoding:(.*,)? *deflate[,\r\n]+", REG_EXTENDED);
    assert(rc == 0);

    rc = regcomp(&re_host, "[\r\n]+Host: *([^ \r\n]+)[ \r\n]+", REG_EXTENDED);
    assert(rc == 0);

    dispatch_source_t accept_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, sock, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(accept_ds, ^{ accept_cb(sock); });
    assert(accept_ds);
    dispatch_resume(accept_ds);

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINFO);
    sigaddset(&sigs, SIGPIPE);

    int s;
    for(s = 0; s < NSIG; s++) {
	    if (sigismember(&sigs, s)) {
		    dispatch_source_t sig_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, s, 0, dispatch_get_main_queue());
		    assert(sig_ds);
		    dispatch_source_set_event_handler(sig_ds, ^{ dump_reqs(); });
		    dispatch_resume(sig_ds);
	    }
    }

    rc = sigprocmask(SIG_BLOCK, &sigs, NULL);
    assert(rc == 0);

    dispatch_source_t dump_timer_ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(dump_timer_ds, DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(dump_timer_ds, ^{ dump_reqs(); });
    dispatch_resume(dump_timer_ds);

    dispatch_main();
    printf("dispatch_main returned\n");

    return 1;
}
