---
title: Using Grand Central Dispatch on macOS
---

Only a few years ago, computers with multiple CPUs were considered exotic. Today, multi-core systems are routine. The challenge faced by programmers has been to take advantage of these systems. Apple's ground-breaking Grand Central Dispatch offers a new approach to programming for modern computer architectures.

The obvious way to take advantage of multiple cores is to write programs that do several tasks at once. Broadly, this is known as "concurrent programming". The traditional approach to concurrent programming is to use multiple threads of control. Threads are a generalization of the original programming model – well suited to single CPU systems – where a single thread of control determined what the computer would do, step by step. Most multi-threaded programs are structured like several independent programs running together. They share a memory address space and common resources like file descriptors, but each thread has it's own step-by-step flow of control.

Multi-threaded programming solves the problem of making use of a computer with two or more cores, but computer hardware designers keep adding twists. Computers have ever more cores, so a multi-threaded program that takes the best advantage of a system with two cores may not perform as well as it might on a system with eight. All those cores come with a cost as well. Each core uses power and generates heat. To manage energy consumption and heat production, some computers will vary the number of cores that are active any given time. It would be a enough of a burden to try to write programs that work well on systems that have 2, 4, or 8 cores. It's an even tougher challenge to write multi-threaded programs that make the best use of hardware when the number of cores changes as the program runs.

This is where GCD steps in to lighten the programmer's load. GCD works with the Operating System to keep track of the number of cores available and the current load on the system. This allows a program to take the best advantage of the system's hardware resources, while allowing the operating system to balance the load of all the programs currently running along with considerations like heating and battery life.

Programming with GCD means leaving behind the details about threads and focusing on the essential work that needs to be done. The program assigns work items to GCD, identifying which items must be done sequentially, which may be done in parallel, and what synchronization is required between them. GCD dispatches the work to the system's available cores.


## GCD for thread programmers

If you are just starting out with concurrent programming, you can safely skip this section. If you have experience using threads, read on! It will help you understand the differences between writing multi-threaded code and using GCD.

Programming with GCD is different than multi-threaded programming. When you program with threads, you think about what each thread will be doing at all times. Each thread is a mini-program with its own flow of control. You decide when to start and end threads, and you establish ways for them to interact – typically using locks or semaphores to protect shared resources where access by multiple threads must be limited.

When you switch to GCD, you still need to identify shared resources and critical sections of code that might need protection, but you don't need to think about threads very much. Remember that GCD will create and destroy threads for you behind the scenes. Your task is to define the program as a set of independent units of work that needs to be done. These work units are expressed in your code as either functions or blocks. Blocks are a newly supported feature of the C language families on macOS. There's more information about blocks in the next section, but for now just think of a block as a snippet of code that can be passed around and executed like a function where it is required.

You'll find that code written for GCD tends to have an event-driven style. GCD defines an object type called a dispatch queue, which acts as a runloop as well as work queue. You can tell a GCD dispatch queue to watch for events – like a timer firing, a message received on a network socket, text typed by a user, and so on – and have it trigger a function or block of code whenever the event occurs. The function or block responds by performing some of it's own computations and possibly telling GCD to execute some further functions or blocks by assigning them to dispatch queues.


## Defining work items: Functions and Blocks

Before examining the GCD library routines and data types, let's take a moment to look at the basic units of code you will be defining. GCD works equally with functions and blocks as the work units that you give to GCD, which in turn dispatches their execution on various threads. We expect that you are already familiar with functions, so we'll leave them aside and focus on blocks.

Blocks are a newly-supported feature of the C language family on macOS. A block is a segment of code that looks like this:

        ^{ printf("Hello World\n"); }

In some ways, a block is like a function definition. It can have arguments as part of it's definition. It has read-only (by default) access to local variables. Blocks can be assigned to variables, and then invoked using the variable name. In this example, a block is assigned to the variable speak. The block has an argument x, but it also uses the variable greeting.

        const char *greeting = "Hello";
        void (^speak)() = ^(char *x){ printf("%s %s\n", greeting, x); };
        speak("World");

The result of running this example would be "Hello World". Take a moment to create a simple C program that does this! Experiment with some variations like taking command-line arguments.

We think you'll like blocks. They work well with GCD and you'll see lots of examples in the sections ahead. If you want to learn more about blocks, see [Blocks Programming Topics](http://developer.apple.com/mac/library/documentation/Cocoa/Conceptual/Blocks/Articles/00_Introduction.html) at [Apple Developer Connection](http://developer.apple.com/).


## Using GCD: libdispatch

Your main point of contact with GCD is in the libdispatch library. It's included in the macOS system library (libSystem), automatically linked in when you build your program with Xcode or cc and ld. You can use the following directive to include all the interface definitions.

    #include <dispatch/dispatch.h>

The on-line manual pages, starting with the dispatch(3) manual provide details on the various library routines, data types and structures, and constants. The [Grand Central Dispatch (GCD) Reference](http://developer.apple.com/mac/library/documentation/Performance/Reference/GCD_libdispatch_Ref/Reference/reference.html) is available at [Apple Developer Connection](http://developer.apple.com/). If you are happier reading header files, you'll find them in `/usr/include/dispatch`. The library is divided into several subcomponents, each with its own header file.

As we noted above, libdispatch provides parallel versions of its routines, one for blocks and one for functions. For example, parallel to `dispatch_async` is `dispatch_async_f`, which takes a function pointer and a data pointer. The data pointer is passed to the function as a single argument when the function is dequeued and executed. In the examples in this article, we use the block versions of the libdispatch routines. Please refer to the GCD documentation for information on using functions.


## Putting work in the pipeline: Queues

Dispatch queues (`dispatch_queue_t` types) are the workhorses of GCD. A queue is a pipeline for executing blocks and functions. At times you will use them as an execution pipeline, and at times you'll use queues in combination with other GCD types to create flexible runloops. We'll explore all the essentials below.

There are two types of queues. Serial queue execute work units in First-In-First-Out (FIFO) order, and execute them one at a time. Concurrent queues also execute work units in FIFO order, but they don't wait for one unit to finish executing before starting the next.

This is one of the few places where threads programmers will recognize a connection between GCD and threads. Concurrent queues will use as many threads as they have available to chew through their workload. However, don't be fooled into thinking that queues, work units, and threads are tightly connected. GCD will use threads from a thread pool, create new threads, and destroy threads in inscrutable ways! A thread that was – a moment ago – being used to execute work from a serial queue might suddenly get reused for work on a concurrent queue, and then may get put to work for some other purpose. It's best to think of queues as little workhorse engines that will get your work done for you, either one item at a time, or as fast that they can by doing work concurrently.

GCD provides four pre-made queues for you to use: one serial queue called the "main" queue, and three concurrent queues. The three concurrent queues have execution priority levels: low, normal or default, and high priority. If you have background work that doesn't need to be done quickly, just submit it to the low-priority queue. If you need really fast response, use the high priority concurrent queue.

GCD provide you with one serial queue "out of the box", but serial queues are easy to create, and they are cheap, consuming relatively small amounts of memory and processing time by themselves. So while a serial queue might seem plodding – doing only one unit of work at a time – your code can create many of them. Each serial queue will run in parallel with all the other queues, once again providing a way for your program to take advantage of the system's processing power. Serial queues also have another useful trick that we'll see a bit later.

There's one more thing to consider before we get to a code example. When you submit a work unit (we'll be using blocks for our examples) to a queue, do you want to just add it to the execution pipeline and go on with other tasks? Or do you want to wait for all the work that's currently enqueued to be processed by the FIFO pipeline, and then for your new work unit to be dequeued and finish executing before you go on? If you just want to submit the work and go on, use `dispatch_async`. If you need to know that the work (and everything enqueued in front of it) has completed, submit it using `dispatch_sync`.

    #include <stdio.h>
    #include <stdlib.h>
    #include <dispatch/dispatch.h>

    /*
     * An example of executing a set of blocks on the main dispatch queue.
     * Usage: hello name ...
     */
    int
    main(int argc, char *argv[])
    {
        int i;

        /*
         * Get the main serial queue.
         * It doesn't start processing until we call dispatch_main()
         */
        dispatch_queue_t main_q = dispatch_get_main_queue();

        for (i = 1; i < argc; i++)
        {
            /* Add some work to the main queue. */
            dispatch_async(main_q, ^{ printf("Hello %s!\n", argv[i]); });
        }

        /* Add a last item to the main queue. */
        dispatch_async(main_q, ^{ printf("Goodbye!\n"); });

        /* Start the main queue */
        dispatch_main();

        /* NOTREACHED */
        return 0;
    }

Try compiling and running this program. You'll notice that the program never exits. The reason is that the main queue continues waiting for more work to do. You can fix that by adding a call to `exit` in the last block.

        /* Add a last item to the main queue. */
        dispatch_async(main_q, ^{
            printf("Goodbye!\n");
            exit(0);
        });

Now let's try a variation that looks nearly identical to the first version of this program. Instead of using the main queue, we create a new serial queue. Since the main queue doesn't have any work to do, the `dispatch_main` call has been removed, so the program will exit at the final `return` statement. This seems to be an alternative way that to make the program exit when it finishes printing, and it also illustrates how easy it is to create a new serial queue.

Unlike the main queue, serial queues produced by `dispatch_queue_create` are active as soon as they are created. Try compiling and running this example!

    #include <stdio.h>
    #include <stdlib.h>
    #include <dispatch/dispatch.h>

    /*
     * An example of executing a set of blocks on a serial dispatch queue.
     * Usage: hello [name]...
     */
    int
    main(int argc, char *argv[])
    {
        int i;

        /* Create a serial queue. */
        dispatch_queue_t greeter = dispatch_queue_create("Greeter", NULL);

        for (i = 1; i < argc; i++)
        {
            /* Add some work to the queue. */
            dispatch_async(greeter, ^{ printf("Hello %s!\n", argv[i]); });
        }

        /* Add a last item to the queue. */
        dispatch_async(greeter, ^{ printf("Goodbye!\n"); });

        return 0;
    }

Ooops! When you run this program, you will see a problem. Some or even all of the output is missing!

Yes, this is a contrived example, but it illustrates something interesting! The reason that the output is missing is that the program exits – at the `return(0)` statement – before the "Greeter" serial queue can do all of it's work. After all, you told it to execute all the blocks asynchronously.

The problem can be fixed by submitting the last block using `dispatch_sync` instead of `dispatch_async`. That call returns after the block has been dequeued and executed. Once the final work has been done, it's safe to return and exit the program.

        /* Add a last item to the queue and wait for it to complete. */
        dispatch_sync(greeter, ^{ printf("Goodbye!\n"); });

A call to `dispatch_sync` blocks until the queue has dequeued all the items already in the pipeline, and when the item you just submitted has completed. That's exactly what you need in a situation like the example above, but it's good to be a little bit cautious when using `dispatch_sync`. More than one programmer has inadvertently created a deadlock in their program logic caused by making a synchronous call in the wrong place!

Also note that calling `dispatch_sync` doesn't guarantee that the queue will be empty when the call returns. Other parts of your program may submit work items to a queue after a blocking call to `dispatch_sync`. So while you know that everything that was ahead of your call in the queue has been dispatched, new items may have been added behind it.


## Synchronizing activities: Serial Queues

We've seen that serial queues are execution engines, and that they do their work in FIFO order, one item at a time. What some people miss on first reading is that "one item at a time" is a very useful control mechanism. Thread programmers are familiar with the notion of "critical sections" of code. These are parts of a multi-threaded program that only one thread at a time should execute. This is often done to protect the integrity of some data while it is being modified. If two threads accessed the data at the same time, it might not be updated correctly, or a thread might get an incorrect value. No doubt you have probably rushed ahead and exclaimed *"AHA! I could use a serial queue for that!"* That's correct! A serial queue can also be used as a device that provides mutual exclusion. After all, it only executes one item at a time.

Let's say you have a data structure `foo` that gets updated by the two functions `increase(foo)` and `decrease(foo)`. Although you might have may concurrently executing blocks, you can't allow more than one thread at a time to increase or decrease the foo structure. The solution is easy! Create a serial queue for the special purpose of adjusting foo.

        dispatch_queue_t foo_adjust = dispatch_queue_create("Adjust foo", NULL);

Now, whenever you need to change foo, use the `foo_adjust` serial queue to do the work. You'll be guaranteed that only one block at a time will make a change.

        dispatch_sync(foo_adjust, ^{ increase(foo); });     ...     dispatch_sync(foo_adjust, ^{ decrease(foo); });

If you need the call to complete before going on with your work, then use `dispatch_sync`. If you only need to ensure that the increase or decrease is done without interference, then you can use `dispatch_async`.

A special-purpose serial queue like this is handy for printing output from your program. You can submit a block to execute on the printing queue and be sure that all the lines you print in that block will appear together and without interference from some other code that might be trying to print at the same time.

        dispatch_async(printer, ^{
            uint32_t i;
            
            printf("Processing Gadget Serial Number %u  Color %s", gadget_serial_number(g), gadget_color(g));
            printf("  Using Widgets:");
            for (i = 0; i < gadget_widget_count(g); i++) printf(" %u", gadget_get_widget(g, i));
            printf("\n");
        });


## Sharing resources: Semaphores

In some cases you might want to limit access to some resource to at most some fixed number of threads. If that limit is one, then you can use a serial queue to give one thread at a time exclusive access. However, you can also use a counting semaphore. They are created with `dispatch_semaphore_create(n)`, where n is the number of simultaneous accesses the semaphore will allow. Code that wants access a limited resource calls `dispatch_semaphore_wait` before using the resource, and then calls `dispatch_semaphore_signal` to relinquish its claim on the resource. Here's a code example which allows up to three threads to simultaneously access a "paint bucket".

    #define PAINTERS 3

        ...
        dispatch_semaphore_t paint_bucket = dispatch_semaphore_create(PAINTERS);

        ...
        dispatch_semaphore_wait(paint_bucket, DISPATCH_TIME_FOREVER);
        use_paint_color();
        dispatch_semaphore_signal(paint_bucket);
        ...

The second argument of `dispatch_semaphore_wait` is a timer. If you don't want to wait for the semaphore in case it is unavailable, you can call `dispatch_semaphore_wait(paint_bucket, DISPATCH_TIME_NOW);`. If you are willing to have your code wait for a few seconds, you can create an appropriate time value using `dispatch_time` or `dispatch_walltime`. More information about these routines is in the Keeping time section below.

A simple lock, which allows only one thread at a time, can be created using `dispatch_semaphore_create(1)`. While you can use a simple lock in place of the serial queue mechanism we explored above, a serial queue is less error prone. A lock requires you to remember to unlock when you are done with it. That may sound pretty basic when you first write some code that uses a lock. However, after many edits and possibly many different programmers, it's not uncommon for someone to forget to unlock when they return from a function call or re-organize the code. On the other hand, submitting a block or a function call to a serial queue will automatically lock and unlock for each item of work.


## Initializing

You can often improve the performance of a program by delaying initialization code until it is required. This is called "lazy" or "just-in-time" initialization. When your program has many items executing concurrently, you need a way to specify that certain operations are never done more than once. libdispatch provides an easy way to do this with `dispatch_once` or `dispatch_once_f`.

        static dispatch_once_t initialize_once;
        ...
        dispatch_once(&initialize_once, ^{
            /* Initialization code */
            ...
        });

The `static` declaration of `initialize_once` ensures that the compiler will make it's value start off as zero. libdispatch will ensure that the initialization occurs at most once.


## Responding to events: Sources

Programs that react to events generated by users, network sources, timers, and other inputs require a runloop: code that waits for an event, responds by taking some action, and then returns to wait for another event. Building a runloop with GCD just requires a dispatch queue and a dispatch source object, of type `dispatch_source_t`.

A dispatch source is paired with a queue, and is assigned the task of watching for a specific event. When the event occurs, the source submits a block or a function to the queue. GCD provides sources that will respond to any of the following events:

* Data available to read on a file descriptor.
* Buffer space available to write on a file descriptor.
* A message received on a mach port.
* A state change notification for a sending mach port.
* Notification of various events occurring for a UNIX process.
* Various file system changes.
* A timer event.
* A UNIX signal.
* An internal data change (for signaling within a program).

If you've been programming long enough to have written your own runloops that handle all these sorts of events, you'll quickly start to appreciate how much easier it is with GCD. As we've seen above, it only takes a single libdispatch call to get one of the four pre-defined dispatch queues, or to create a new serial queue. Creating a dispatch source to submit a block or function to a queue when an event occurs is almost as easy. For example, say you want to run an input processing routine to trigger on the main queue every time data arrives on a file descriptor.

        input_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, my_file, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(input_src,  ^{ process_input(my_file); });
        dispatch_source_set_cancel_handler(input_src,  ^{ close(my_file); });
        dispatch_resume(input_src);

The first line creates a `dispatch_source_t` object that watches for data on the `my_file` file descriptor, and pairs the new source with the main dispatch queue. When a dispatch source is created, it is in a "suspended" state, allowing you to configure it before it goes to work. In this example, we assign an event handler and a cancellation handler. Then we activate the source with `dispatch_resume`.

The event handler is a block of code we want to be executed when the source is triggered – in this case by data arriving at a file descriptor. The cancellation handler is a block that is executed exactly once when a source has been cancelled. The asynchronous call `dispatch_source_cancel` starts the process of shutting down a dispatch source. The call is not pre-emptive, so there may be event handler blocks still executing after a call to `dispatch_source_cancel`. The cancellation handler is executed once after any active event handlers have completed. At that point, it's safe to close file descriptors, mach ports, or clean up any other resources that might have been in use to handle source events.

You can cause a dispatch source to suspend anytime using `dispatch_suspend`, and reactivate it with `dispatch_resume`. This just causes a pause, and doesn't involve the cancellation handler. You might suspend and resume if you wanted to change the event handler, or if you simply wanted part of your program to go quiet for a while.

Let's try an example program that creates some timer sources. Each timer has a duration (the interval between successive timer firings) and a count (how many times to fire). When all the timers have finished, the program exits. This example program uses a couple of serial queues for printing and updating a counter, as we saw in the examples in the section on serial queues.

The program also uses some aspects of block programming that we haven't seen before. It creates dispatch source objects that are local in the scope of a for loop, but the variables seem to be used outside that loop – in the event handler block for each source – once the program invokes `dispatch_main`. The reason this works is that the entire block is copied by the dispatch source. A block includes not only program instructions, but data (variables) as well, so the copy of the block includes copies of the duration and src variables that it uses. These variables default to being "read only", and can't be modified in the block. The count variable is also used in the block, but it is modified. To make it "read/write", it is declared using the `__block` qualifier. See [Blocks Programming Topics](http://developer.apple.com/mac/library/documentation/Cocoa/Conceptual/Blocks/Articles/00_Introduction.html) for all the details about programming with blocks.

    /*
     * usage: source_timer [duration count]...
     *
     * example: source_timer 1 10 2 7 5 5
     *
     *     Creates a 1 second timer that counts down 10 times (10 second total),
     *     a 2 second timer that counts down 7 times (14 second total),
     *     and a 5 second timer that counts down 5 times (25 second total).
     */
    #include <stdio.h>
    #include <stdlib.h>
    #include <dispatch/dispatch.h>

    /*
     * We keep track of how many timers are active with ntimers.
     * The ntimer_serial queue is used to update the variable.
     */
    static uint32_t ntimers = 0;
    static dispatch_queue_t ntimer_serial = NULL;

    /*
     * The print_serial queue is used to serialize printing.
     */
    static dispatch_queue_t print_serial = NULL;

    int
    main(int argc, char *argv[])
    {
        dispatch_queue_t main_q;
        int i;

        /*
         * Create our serial queues and get the main queue.
         */
        print_serial = dispatch_queue_create("Print Queue", NULL);
        ntimer_serial = dispatch_queue_create("N Timers Queue", NULL);

        main_q = dispatch_get_main_queue();

        /*
         * Pick up arguments two at a time.
         */
        for (i = 1; i < argc; i += 2)
        {
            uint64_t duration;
            dispatch_source_t src;

            /*
             * The count variable is declared with __block to make it read/write.
             */
            __block uint32_t count;

            /*
             * Timers are in nanoseconds.  NSEC_PER_SEC is defined by libdispatch.
             */
            duration = atoll(argv[i]) * NSEC_PER_SEC;
            count = atoi(argv[i + 1]);

            /*
             * Create a dispatch source for a timer, and pair it with the main queue.
             */
            src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main_q);

            /*
             * Set the timer's duration (in nanoseconds).
             */
            dispatch_source_set_timer(src, 0, duration, 0);

            /*
             * Set an event handler block for the timer source.
             * This is the block of code that will be executed when the timer fires.
             */
            dispatch_source_set_event_handler(src, ^{
                /* Count down to zero */
                count--;
                dispatch_async(print_serial, ^{ printf("%s second timer   count %u\n", argv[i], count); });

                if (count == 0) 
                {
                    /*
                     * When the counter hits zero, we cancel and release the
                     * timer source, and decrement the count of active timers.
                     */
                    dispatch_source_cancel(src);
                    dispatch_release(src);
                    dispatch_sync(ntimer_serial, ^{ ntimers--; });
                }

                if (ntimers == 0)
                {
                    /*
                     * This was the last active timer.  Say goodbye and exit.
                     */
                    dispatch_sync(print_serial, ^{ printf("All timers finished.  Goodbye\n"); });
                    exit(0);
                }
            });

            /*
             * Increment the count of active timers, and activate the timer source.
             */
            dispatch_sync(ntimer_serial, ^{ ntimers++; });
            dispatch_resume(src);
        }

        /*
         * When we reach this point, all the timers have been created and are active.
         * Start the main queue to process the event handlers.
         */
        dispatch_main();

        return 0;
    }

Other source types are as easy to use as the examples above. Let's look the code that a GCD program would need to use to receive and process UNIX signals in a runloop.

    #include <stdio.h>
    #include <stdlib.h>
    #include <signal.h>
    #include <unistd.h>
    #include <dispatch/dispatch.h>

    int
    main(int argc, char *argv[])
    {
        dispatch_source_t sig_src;

        printf("My PID is %u\n", getpid());

        signal(SIGHUP, SIG_IGN);
        
        sig_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sig_src, ^{ printf("Caught SIGHUP\n"); });
        dispatch_resume(sig_src);

        dispatch_main();
        return 0;
    }

Try running this program in one shell, and send it HUP signals from another shell. The only wrinkle in using signals is that you need to disable the default signal handling with so that GCD can "catch" the signals.

If you are an old hand at UNIX programming, stop here for a second to appreciate how much easier it is to handle signals using a dispatch source than using a signal handler! There are no special rules for what you can safely do while handling the signal. There are no strange mechanisms required to tell your runloop that a signal was caught. With `DISPATCH_SOURCE_TYPE_SIGNAL` sources, signals are just events like data on a file descriptor or a timer firing.

Before ending this section, we'll examine the `DISPATCH_SOURCE_TYPE_DATA_ADD` and `DISPATCH_SOURCE_TYPE_DATA_OR` types. These are useful when one part of a program needs to communicate with another. The communication is achieved through an unsigned long value. The section of code that wants to trigger the data source just calls

        dispatch_source_merge_data(data_src, value);

The event handler function or block for the data source can fetch the data value using

        data = dispatch_source_get_data(data_src);

The difference between the ADD type and the OR type has to do with how the data gets coalesced if there are multiple calls to `dispatch_source_merge_data` before the dispatch data source's event handler gets executed. `dispatch_source_merge_data` will either add or logically OR the values with the dispatch source's data buffer. The data buffer is reset for each invocation of the data source's event handler.

Other dispatch source types also provide information using `dispatch_source_get_data`. For example, a `DISPATCH_SOURCE_TYPE_PROC` source can be used to monitor a process. The mask parameter passed to `dispatch_source_create` for this type specifies the events you wish to monitor, such as the process exiting, calling `fork`, calling `exec`, and others. When the source is triggered by some event, the value returned by `dispatch_source_get_data` will have bits set corresponding the the mask bits, reporting which events took place. Similarly, sources of type `DISPATCH_SOURCE_TYPE_VNODE` report which changes occurred using bits set in the value returned by `dispatch_source_get_data`. See the on-line manual page for `dispatch_source_create` for details.

Try writing a few of these simple programs with some other dispatch source types!


## Tracking progress: Groups and callbacks

GCD helps you gain performance when you can split large programming tasks into smaller operations that can run independently on multiple cores. It's often necessary to know when several independent operations have completed. That's the purpose of dispatch groups. Conceptually, a group keeps track of a set of work items. Your code can either wait for them all to complete, or you can set up the group to submit a block or function to a queue when they all complete.

There are several ways to use dispatch groups. Here's simple example that dispatches three concurrent operations using function calls. It sets up a group to dispatch a function to the main queue when they complete.

        dispatch_group_t grp = dispatch_group_create();
        dispatch_queue_t q = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

        /*
        * Three independent operations must be done...
        */
        dispatch_group_async(grp, q, (void *)ptr, ^{ /* Do some operation */ ... } );
        dispatch_group_async(grp, q, (void *)ptr, ^{ /* Do another operation */ ... } );
        dispatch_group_async(grp, q, (void *)ptr, ^{ /* Do yet another operation */ ... } );

        /*
        * Tell the group to invoke a block to deal with
        * the data after all the operations are done.
        */
        dispatch_group_notify(grp, dispatch_get_main_queue(), ^{ data_processing_done(ptr); });
        dispatch_release(grp);

The three operations will be dispatched on the default priority concurrent queue. When all three items complete, the group will dispatch the last block. Note that `dispatch_group_notify` just tells the group what to do when all the items it is tracking complete. The call doesn't block, and your code can go on to do other things.

Internally, a dispatch group simply maintains a counter that has special rules when the value is zero. You can have the group submit a block or function to a queue whenever the counter is zero, or you can use `dispatch_group_wait` to block (with a timeout) for the count to be zero.

        dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);

You can add more work items to a group after the counter reaches zero to track another set of work items. Each time the internal counter becomes zero, the group will dispatch the notification block or function, or you can call `dispatch_group_wait` again.

This brings up an interesting edge case. In the example above, `dispatch_group_notify_f` is called after the calls to `dispatch_group_async_f`. If you set up a group notification before any work is added, the group may dispatch the notification immediately since the internal counter is zero to start. In practice this is non-deterministic.

`dispatch_group_async` – and similarly `dispatch_group_async_f` – are really just convenience routines that behave like this:

        void
        dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue, dispatch_block_t block)
        {
            dispatch_retain(group);
            dispatch_group_enter(group);
            dispatch_async(queue, ^{
                block();
                dispatch_group_leave(group);
                dispatch_release(group);
            });
        }

Note that the group is retained until after the call to `dispatch_group_leave`. In the simple example above, the functions submitted to the concurrent queue may still be waiting to execute when the code calls `dispatch_release(grp)`. If `dispatch_group_async` did not retain the group, it may be freed before being used.

The internal counter is incremented by `dispatch_group_enter`, and decremented by `dispatch_group_leave`. You could do this in your own code, but using `dispatch_group_async` ensures that the counter is always incremented and decremented correctly.

Dispatch groups will keep track of any number of work items and will dispatch a completion callback when they finish. If you only need to dispatch a callback when a single asynchronous item completes, you can simply use nested `dispatch_async` calls.

        dispatch_retain(callback_queue);

        dispatch_async(some_queue, ^{
            /* work to be done */
            ...

            dispatch_async(callback_queue, ^{
                /* code to execute when work has completed */
                ...
            });

            dispatch_release(callback_queue);
        });

For the same reason that `dispatch_group_async` retains and then releases its group, code that uses nested `dispatch_async` calls must retain and release object references.


## Keeping time

Two time-related constants were introduced in the section on using dispatch semaphores: `DISPATCH_TIME_NOW` and `DISPATCH_TIME_FOREVER`. These two values are the endpoints of a time scale represented by `dispatch_time_t` type variables. When used with `dispatch_semaphore_wait` or `dispatch_group_wait` routines, `DISPATCH_TIME_NOW` means that you are not willing to wait at all. Those routines will return an error status if the semaphore is unavailable or the group counter is non-zero. `DISPATCH_TIME_FOREVER` simply means you are willing to wait as long as required.

In addition to the two "wait" routines, libdispatch uses a `dispatch_time_t` variable in `dispatch_after` and `dispatch_after_f`. These routines submit a block or function to a queue (using `dispatch_async` or `dispatch_async_f`) at some time in the future. Calling with a time value of `DISPATCH_TIME_NOW` will cause the work to be dispatched immediately. Using `DISPATCH_TIME_FOREVER `clearly doesn't make a lot of sense, but you can be assured that it truly will be forever before the work is submitted to a queue!

Values between "now" and "forever" can be of two forms: n nanoseconds from now in "system" time, or at a specific "wall clock" time. System time uses an internal clock that just keeps track of how many seconds the system has been active. If you put your computer to sleep, the system clock will sleep as well. Many "wall clock" hours might pass with zero or very little system clock time passing.

`dispatch_time_t` values are generated using two very similar routines.

        dispatch_time_t
        dispatch_time(dispatch_time_t base, int64_t offset);

        dispatch_time_t
        dispatch_walltime(struct timespec *base, int64_t offset);

`dispatch_walltime` always produces a wall clock time value. The base parameter may point to a filled-out `struct timespec` indicating a specific time, or NULL indicating the current wall-clock time. The offset parameter represents a value in nanoseconds which is added to the base value. To produce a time value 5 minutes from the current time:

        dispatch_time_t in_five_minutes = dispatch_walltime(NULL, 5 * 60 * NSEC_PER_SEC);

`dispatch_time` adds an offset (in nanoseconds) to a base time. The base time can be a wall clock time that was previously produced by `dispatch_walltime, or it can be `DISPATCH_TIME_NOW`, which will produce a system time value.

**Note:** At the time of writing of this article, `dispatch_group_wait` and `dispatch_semaphore_wait` treat timeouts strictly as system clock nanoseconds. This may change in a future update. Also note that a dispatch source of `DISPATCH_SOURCE_TYPE_TIMER` that uses a wall clock time may be delayed in triggering following a system wake from sleep. This is also the case for a work item waiting for dispatch at a wall clock time in `dispatch_after` or `dispatch_after_f`. These timers will trigger as soon as there is any activity in your program. This behavior may change in a future update.


## Putting it all together

We hope that the discussion and examples provided here will help you on your way to becoming a proficient GCD programmer. However, the best way to learn is to jump in and write some code! Try writing a few small programs that explore just some part of the libdispatch API. Experiment! Try some variations! We think you'll like programming with GCD, and that you'll appreciate how it it makes it possible to take advantage of modern multi-core computer systems.


## For More Information

A complete library of resource information is available on the Web at Apple Developer Connection <http://developer.apple.com/>.
Detailed information on programming with blocks starts at <http://developer.apple.com/mac/library/documentation/Cocoa/Conceptual/Blocks/Articles/00_Introduction.html/>.
More information on GCD starts at <http://developer.apple.com/mac/library/documentation/Performance/Reference/GCD_libdispatch_Ref/Reference/reference.html/>.

Copyright (c) 2009 Apple Inc. All Rights Reserved.  
(Last updated: 2009-11-04)
