/* program to test if libdispatch invokes group notify functions too early
 * when a group is recycled after dispatch_group_wait().
 *
 * compile with
 * gcc -o dispatch_break dispatch_break.c -O2 -ldispatch -pthread -Wall -std=gnu99
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <dispatch/dispatch.h>


#define NUM_SLEEPERS 700


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int sleeper_count = 0;
static bool notify_done = false;


void sleeper(void *parameter)
{
	int *param = parameter;
	usleep(*param);
	free(param);

	pthread_mutex_lock(&mutex);
	assert(sleeper_count > 0);
	sleeper_count--;
	pthread_mutex_unlock(&mutex);
}


void notify_call(void *parameter)
{
	pthread_mutex_lock(&mutex);
	if(sleeper_count > 0) {
		printf("sleeper_count is %d in notify_call!\n", sleeper_count);
		abort();
	}
	assert(!notify_done);
	notify_done = true;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}


static void launch_sleepers(dispatch_group_t grp)
{
	pthread_mutex_lock(&mutex);
	assert(sleeper_count == 0);
	sleeper_count += NUM_SLEEPERS;
	pthread_mutex_unlock(&mutex);

	for(int i=0; i<NUM_SLEEPERS; i++) {
		int *p = malloc(sizeof(*p));
		*p = (NUM_SLEEPERS - i) * 7 + 11999;
		dispatch_group_async_f(grp,
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
			p, &sleeper);
	}
}


int main(int argc, char *argv[])
{
	bool workaround = false, no_wait = false;
	if(argc > 1 && strcmp(argv[1], "--workaround") == 0) {
		printf("--- using workaround for the group recycle bug.\n");
		workaround = true;
		argc--; argv++;
	}
	if(argc > 1 && strcmp(argv[1], "--no-wait") == 0) {
		printf("--- won't call dispatch_group_wait().\n");
		no_wait = true;
	}

	printf("before recycling:\n");
	dispatch_group_t grp = dispatch_group_create();
	launch_sleepers(grp);
	dispatch_group_notify_f(grp,
		dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
		NULL, &notify_call);
	if(!no_wait) dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);
	pthread_mutex_lock(&mutex);
	while(!notify_done) pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);
	assert(sleeper_count == 0);

	printf("repeating...\n");
	for(int i=0; i<100; i++) {
		if(workaround) {
			/* don't recycle the group, instead create one for each iteration.
			 * this prevents the bug from occurring.
			 */
			dispatch_release(grp);
			grp = dispatch_group_create();
		}

		pthread_mutex_lock(&mutex);
		notify_done = false;
		pthread_mutex_unlock(&mutex);

		printf("iter %d:\n", i);
		launch_sleepers(grp);
		dispatch_group_notify_f(grp,
			dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
			NULL, &notify_call);
		if(!no_wait) dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);
		pthread_mutex_lock(&mutex);
		while(!notify_done) pthread_cond_wait(&cond, &mutex);
		pthread_mutex_unlock(&mutex);
		assert(sleeper_count == 0);
	}
	printf("success!\n");

	dispatch_release(grp);
	return EXIT_SUCCESS;
}
