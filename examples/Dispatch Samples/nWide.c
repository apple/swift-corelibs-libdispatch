/*
 * Copyright (c) 2009 Apple Inc.  All rights reserved.
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

/*
 *  nWide.c
 *  Samples project
 *
 *  Created by Mensch on 5/1/09.
 *  Copyright 2009 Apple, Inc. All rights reserved.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#import <libkern/OSAtomic.h>
#include <string.h>


/*
 *	Demonstrate using dispatch_semaphore to create a concurrent queue that
 *	allows only a fixed number of blocks to be in flight at any given time
 */

int main (int argc, const char * argv[]) {
	dispatch_group_t	mg = dispatch_group_create();
	dispatch_semaphore_t ds;
	__block int numRunning = 0;
	int	qWidth = 5;		
	int numWorkBlocks = 100;
	
	if (argc >= 2) {
		qWidth = atoi(argv[1]);	// use the command 1st line parameter as the queue width
		if (qWidth==0) qWidth==1; // protect against bad values
	}
	
	if (argc >=3) {
		numWorkBlocks = atoi(argv[2]);	// use the 2nd command line parameter as the queue width
		if (numWorkBlocks==0) numWorkBlocks==1; // protect against bad values
	}
	
	printf("Starting dispatch semaphore test to simulate a %d wide dispatch queue\n", qWidth );
	ds = dispatch_semaphore_create(qWidth);
	
	int i;
	for (i=0; i<numWorkBlocks; i++) {
		// synchronize the whole shebang every 25 work units...
		if (i % 25 == 24) {
			dispatch_group_async(mg,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
				int x;
				// wait for all pending work units to finish up... 
				for (int x=0; x<qWidth; x++) dispatch_semaphore_wait(ds, DISPATCH_TIME_FOREVER);
				// do the thing that is critical here
				printf("doing something critical...while %d work units are running \n",numRunning);
				// and let work continue unimpeeded
				for (int x=0; x<qWidth; x++) dispatch_semaphore_signal(ds);
			});
		} else {
			// schedule the next block waiting when there are qWidth blocks running
			dispatch_semaphore_wait(ds, DISPATCH_TIME_FOREVER);				
			dispatch_group_async(mg,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
				OSAtomicIncrement32( &numRunning );
				usleep(random() % 10000);	// simulate some random amount of work
				printf("Value of i is %d  Number of blocks in flight %d\n",i, numRunning);
				// tell the loop it's time to schedule the next block if there is one
				OSAtomicDecrement32( &numRunning );
				dispatch_semaphore_signal(ds);
			});
		}
	}
	
	dispatch_group_notify(mg, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		printf("And we are done!\n");
		dispatch_release(mg);
		dispatch_release(ds);
		exit(0);
	});

	dispatch_main();
    return 0;
}


