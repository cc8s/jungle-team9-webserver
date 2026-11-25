#!/bin/sh
set -eu
if [ "${1:-}" = server ]; then
    shift
    mkdir -p /tmp/perf-source
    cp -R /source/src /source/Makefile /tmp/perf-source/
    cd /tmp/perf-source
    make -j2
    python3 /source/tests/perf/fixtures.py --root /fixtures --clients 8
    cd /fixtures
    exec /tmp/perf-source/webserver "$@"
fi
exec "$@"
