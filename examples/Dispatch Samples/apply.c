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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#define kIT	10

uint64_t		elapsed_time;

void	timer_start() {
		elapsed_time = mach_absolute_time();
}

double	timer_milePost() {
	static dispatch_once_t		justOnce;
	static double		scale;
	
	dispatch_once(&justOnce, ^{
		mach_timebase_info_data_t	tbi;
		mach_timebase_info(&tbi);
		scale = tbi.numer;
		scale = scale/tbi.denom;
		printf("Scale is %10.4f  Just computed once courtesy of dispatch_once()\n", scale);
	});
	
	uint64_t	now = mach_absolute_time()-elapsed_time;
	double	fTotalT = now;
	fTotalT = fTotalT * scale;			// convert this to nanoseconds...
	fTotalT = fTotalT / 1000000000.0;
	return fTotalT;
}

int
main(void)
{
	dispatch_queue_t myQueue = dispatch_queue_create("myQueue", NULL);
	dispatch_group_t myGroup = dispatch_group_create();
		
// dispatch_apply on a serial queue finishes each block in order so the following code will take a little more than a second
	timer_start();
	dispatch_apply(kIT, myQueue, ^(size_t current){
		printf("Block #%ld of %d is being run\n",
			   current+1, // adjusting the zero based current iteration we get passed in
			   kIT);	
		usleep(USEC_PER_SEC/10);
	});	
	printf("and dispatch_apply( serial queue ) returned after %10.4lf seconds\n",timer_milePost());
	
// dispatch_apply on a concurrent queue returns after all blocks are finished, however it can execute them concurrently with each other
// so this will take quite a bit less time
	timer_start();	
	dispatch_apply(kIT, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(size_t current){
		printf("Block #%ld of %d is being run\n",current+1, kIT);
		usleep(USEC_PER_SEC/10);
	});
	printf("and dispatch_apply( concurrent queue) returned after %10.4lf seconds\n",timer_milePost());
	
// To execute all blocks in a dispatch_apply asynchonously, you will need to perform the dispatch_apply
// asynchonously, like this (NOTE the nested dispatch_apply inside of the async block.)
// Also note the use of the dispatch_group so that we can ultimatly know when the work is
// all completed
	
	timer_start();	
	dispatch_group_async(myGroup, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ 
		dispatch_apply(kIT, myQueue, ^(size_t current){
			printf("Block #%ld of %d is being run\n",current+1, kIT);
			usleep(USEC_PER_SEC/10);
		});
	});
	
	printf("and dispatch_group_async( dispatch_apply( )) returned after %10.4lf seconds\n",timer_milePost());
	printf("Now to wait for the dispatch group to finish...\n");
	dispatch_group_wait(myGroup, UINT64_MAX);
	printf("and we are done with dispatch_group_async( dispatch_apply( )) after %10.4lf seconds\n",timer_milePost()); 
	return 0;
}

