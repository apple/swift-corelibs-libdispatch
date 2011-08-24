#!/bin/sh

/usr/bin/leaks "$@" 2>&1 | tee "${TMPDIR}$*.leakslog" | grep -q " 0 leaks for 0 total leaked bytes"

if [ $? -eq 0 ]; then
    rm "${TMPDIR}$*.leakslog"
    exit 0
else
    exit $?
fi
