### DispatchProcessMonitor ###

===========================================================================
DESCRIPTION:

Sample code showing how to: monitor process, do file and network I/O,
create and manage timers, and use dispatch_apply

===========================================================================
BUILD REQUIREMENTS:

Mac OS X version 10.6 Snow Leopard

===========================================================================
RUNTIME REQUIREMENTS:

Mac OS X version 10.6 Snow Leopard

===========================================================================
PACKAGING LIST:

apply.c       - dispatch_apply examples
netcat.c      - network I/O examples
nWide.c       - use of dispatch_semaphore to limit number of in-flight blocks
proc.c        - process monitoring example
readFile.c    - file I/O examples
readFileF.c   - file I/O examples without Blocks
timers.c      - create and manage timers

===========================================================================
SAMPLE USAGE:

dispatch-apply

dispatch-apply takes no arguments.   When run it will display some status
messages and timing information.

dispatch-netcat

Open two terminal windows.   In one window run the "server":

cat ReadMe.txt | dispatch-netcat -l localhost 5050

In the other run the "client":

dispatch-netcat localhost 5050

Your server will send the contents of ReadMe.txt to the client, the server
will close it's connection and exit.   The client will display whatever
the server sent (the ReadMe.txt file).   See the main function in netcat.c
for more options.

dispatch-nWide

dispatch-nWide takes no arguments.   When run it will display explanatory
text.

dispatch-proc

dispatch-proc takes no arguments.   When run it will display output from
some processes it runs, and it will display information from the
process lifecycle events dispatch generates.

dispatch-readFile
 
Run dispatch-readFile with a filename as an argument:

dispatch-readFile ReadMe.txt

It will read the file 10 (or fewer) bytes at a time and display how many
bytes dispatch thinks are remaining to read.

dispatch-readFileF

Exactly the same as dispatch-readFile, but written without the use of Blocks.

dispatch-timers

dispatch-timers takes no arguments, running it display timer ticks for
a timer with an initial interval of one second, changing to one half second 
after the first three events.   It will exit after six events.

===========================================================================
CHANGES FROM PREVIOUS VERSIONS:

Version 1.1
- Updated to current libdispatch API, and added samples readFileF.c and 
nWide.c
Version 1.0
- First version

===========================================================================
Copyright (C) 2009 Apple Inc. All rights reserved.
