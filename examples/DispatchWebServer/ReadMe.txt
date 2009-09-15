### DispatchWebServer ###

===========================================================================
DESCRIPTION:

Sample code showing how to: Use dispatch in a real world setting,
schedule file and network I/O, use vnode sources, create and manage
timers.

===========================================================================
BUILD REQUIREMENTS:

Mac OS X version 10.6 Snow Leopard

===========================================================================
RUNTIME REQUIREMENTS:

Mac OS X version 10.6 Snow Leopard

===========================================================================
PACKAGING LIST:

DispatchWebServer.c       - the web server

===========================================================================
RUNNING:

Running the program will start a web server on port 8080, it will read
content from ~/Sites and write ~/Library/Logs/DispatchWebServer-transfer.log
each time complets a request.

It will write some to stdout when it makes new connections, recieves
requests, completes requests, and when it closes connections.   It also
shows the state of each actiave request once evey five seconds and any
time you send a SIGINFO signal to it.

===========================================================================
CHANGES FROM PREVIOUS VERSIONS:

Version 1.0
- First version

===========================================================================
Copyright (C) 2009 Apple Inc. All rights reserved.
