#!/bin/sh
# Isolated Linux server and load generator. No host port or source-tree writes.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PERF_IMAGE=${PERF_IMAGE:-jungle-perf:local}
PERF_RUN_ID=${PERF_RUN_ID:-local}
case "$PERF_RUN_ID" in ''|*[!a-zA-Z0-9_-]*) echo 'PERF_RUN_ID must contain only letters, digits, _ or -' >&2; exit 2;; esac
PERF_OUTPUT_DIR=${PERF_OUTPUT_DIR:-$SOURCE_ROOT/build/perf-results/$PERF_RUN_ID}
mkdir -p "$PERF_OUTPUT_DIR"
PERF_OUTPUT_DIR=$(CDPATH= cd -- "$PERF_OUTPUT_DIR" && pwd)
SERVER="jungle-perf-$PERF_RUN_ID-server"
NETWORK="jungle-perf-$PERF_RUN_ID-net"
VOLUME="jungle-perf-$PERF_RUN_ID-fixtures"
LABEL="jungle.perf.run=$PERF_RUN_ID"

cleanup() {
    # Names and labels must both match before deleting any resource.
    if [ "$(docker inspect -f '{{index .Config.Labels "jungle.perf.run"}}' "$SERVER" 2>/dev/null || true)" = "$PERF_RUN_ID" ]; then
        docker logs "$SERVER" > "$PERF_OUTPUT_DIR/server.log" 2>&1 || true
        docker stop -t 20 "$SERVER" >/dev/null || true
        docker logs "$SERVER" > "$PERF_OUTPUT_DIR/server.log" 2>&1 || true
        docker inspect "$SERVER" > "$PERF_OUTPUT_DIR/server-final.json" || true
        docker rm "$SERVER" >/dev/null
    fi
    if [ "$(docker network inspect -f '{{index .Labels "jungle.perf.run"}}' "$NETWORK" 2>/dev/null || true)" = "$PERF_RUN_ID" ]; then
        docker network rm "$NETWORK" >/dev/null
    fi
    if [ "$(docker volume inspect -f '{{index .Labels "jungle.perf.run"}}' "$VOLUME" 2>/dev/null || true)" = "$PERF_RUN_ID" ]; then
        docker volume rm "$VOLUME" >/dev/null
    fi
}
client() {
    docker run --rm --label "$LABEL" --network "$NETWORK" --pid="container:$SERVER" \
        -v "$SOURCE_ROOT:/source:ro" -v "$PERF_OUTPUT_DIR:/results" -v "$VOLUME:/fixtures:ro" \
        -e PERF_BASE_URL=http://server:8000 -e PERF_SERVER_PID=1 \
        -e PERF_FIXTURE_ROOT=/fixtures -e PERF_OUTPUT_DIR=/results -e PYTHONDONTWRITEBYTECODE=1 \
        "$PERF_IMAGE" "$@"
}
case "${1:-}" in
    build)
        docker build -f "$SCRIPT_DIR/Dockerfile" -t "$PERF_IMAGE" "$SCRIPT_DIR"
        ;;
    up)
        if docker inspect "$SERVER" >/dev/null 2>&1 || docker network inspect "$NETWORK" >/dev/null 2>&1 || docker volume inspect "$VOLUME" >/dev/null 2>&1; then
            echo "Resources already exist for $PERF_RUN_ID; use down or a different PERF_RUN_ID" >&2; exit 2
        fi
        trap cleanup EXIT HUP INT TERM
        git -C "$SOURCE_ROOT" rev-parse HEAD > "$PERF_OUTPUT_DIR/source-head.txt"
        git -C "$SOURCE_ROOT" status --short > "$PERF_OUTPUT_DIR/source-status.txt"
        git -C "$SOURCE_ROOT" diff --binary HEAD > "$PERF_OUTPUT_DIR/source-working.patch"
        python3 - "$SOURCE_ROOT" "$PERF_OUTPUT_DIR/source-files-sha256.json" <<'PYHASH'
import hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1])
paths=[root/"Makefile", *sorted((root/"src").glob("*")), *sorted((root/"tests/perf").glob("*"))]
hashes={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}
pathlib.Path(sys.argv[2]).write_text(json.dumps(hashes,indent=2)+"\n")
PYHASH
        docker image inspect "$PERF_IMAGE" > "$PERF_OUTPUT_DIR/image.json"
        docker info > "$PERF_OUTPUT_DIR/docker-info.txt"
        docker network create --label "$LABEL" "$NETWORK" >/dev/null
        docker volume create --label "$LABEL" "$VOLUME" >/dev/null
        # This container has an independent port 8000; no -p host publishing.
        docker run -d --name "$SERVER" --label "$LABEL" --network "$NETWORK" --network-alias server \
            -v "$SOURCE_ROOT:/source:ro" -v "$VOLUME:/fixtures" "$PERF_IMAGE" server \
            --worker "${PERF_WORKERS:-4}" --queue "${PERF_QUEUE:-64}" \
            --max "${PERF_MAX:-128}" --buffer "${PERF_BUFFER:-32}" >/dev/null
        client python3 -c '
import socket,time
for attempt in range(120):
    try:
        with socket.create_connection(("server",8000),timeout=1) as s:
            s.sendall(b"GET /perf/small.bin HTTP/1.1\r\nHost: server\r\nConnection: close\r\n\r\n")
            assert s.recv(4096).startswith(b"HTTP/1.1 200 ")
            break
    except (OSError,AssertionError): time.sleep(.25)
else: raise SystemExit("server readiness timeout")'
        docker inspect "$SERVER" > "$PERF_OUTPUT_DIR/server-start.json"
        client sh -c 'uname -a; gcc --version; wrk --version; python3 --version; ulimit -n; cat /proc/1/cmdline; printf "\n"; dpkg-query -W wrk' > "$PERF_OUTPUT_DIR/environment.txt"
        trap - EXIT HUP INT TERM
        echo "server=$SERVER url=http://server:8000 output=$PERF_OUTPUT_DIR"
        ;;
    exec)
        shift
        [ "$#" -gt 0 ] || { echo 'exec requires a command' >&2; exit 2; }
        client "$@"
        ;;
    down) cleanup ;;
    smoke)
        # Deliberately brief checks, not a benchmark sweep.
        "$0" up
        trap 'status=$?; trap - EXIT HUP INT TERM; cleanup; exit "$status"' EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM
        client python3 /source/tests/perf/environment_smoke.py
        client wrk -t1 -c2 -d1s --timeout 5s http://server:8000/perf/small.bin
        "$0" down
        trap - EXIT HUP INT TERM
        python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))[0]; assert d["State"]["ExitCode"]==0,d["State"]; print("server graceful exit=0")' "$PERF_OUTPUT_DIR/server-final.json"
        ;;
    *) echo "Usage: $0 build|up|exec <command...>|down|smoke" >&2; exit 2 ;;
esac
