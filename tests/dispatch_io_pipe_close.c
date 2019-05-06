/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include <bsdtests.h>
#include "dispatch_test.h"
#include <dispatch/dispatch.h>

int
main() {
    int pipe_fds[2] = { -1, -1 };
    int pipe_err = pipe(pipe_fds);
    int readFD = pipe_fds[0];
    int writeFD = pipe_fds[1];

    dispatch_test_start(NULL);
    if (pipe_err) {
        test_errno("pipe", errno, 0);
        test_stop();
        _Exit(EXIT_FAILURE);
    }

    printf("readFD=%d, writeFD=%d\n", readFD, writeFD);
    dispatch_queue_t q = dispatch_queue_create("q", NULL);
    dispatch_io_t io = dispatch_io_create(DISPATCH_IO_STREAM, readFD, q, ^(int err) {
        printf("cleanup, err=%d\n", err);
        close(readFD);
        printf("all done\n");
        test_stop();
        _Exit(EXIT_SUCCESS);
    });
    dispatch_io_set_low_water(io, 0);
    dispatch_io_read(io, 0, UINT_MAX, q, ^(bool done, dispatch_data_t data, int err) {
        printf("read: \%d, %zu, %d\n", done, data == NULL ? 0 : dispatch_data_get_size(data), err);
        if (data != NULL && dispatch_data_get_size(data) > 0) {
            // will only happen once
            printf("closing writeFD\n");
            close(writeFD);
            dispatch_after(DISPATCH_TIME_NOW + 1, q, ^{
                dispatch_io_close(io, 0);
            });
        }
    });
    dispatch_resume(io);
    printf("writing\n");
    write(writeFD, "x", 1);
    printf("wrtten\n");
    dispatch_main();
}
