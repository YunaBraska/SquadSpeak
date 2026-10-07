#!/bin/sh
# Network changes occur only in disposable containers on internal networks.
set -eu
test "$#" -eq 1 || { printf 'Usage: discovery-network.sh BUILD_ARTIFACT_DIRECTORY\n' >&2; exit 2; }
artifacts=$(cd "$1" && pwd)
test -x "$artifacts/discovery_network_probe"
evidence=$(mktemp -d "$artifacts/network-change.XXXXXX")
name="squadspeak-${evidence##*/}"
cleanup() {
    for process in receiver sender-1 sender-2; do
        docker logs "$name-$process" > "$evidence/$process.log" 2>&1 || true
    done
    docker rm -f "$name-receiver" "$name-sender-1" "$name-sender-2" >/dev/null 2>&1 || true
    docker network rm "$name-0" "$name-1" "$name-2" >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
wait_file() {
    count=0
    while test ! -f "$evidence/$1"; do
        count=$((count + 1))
        if test "$count" -gt 25; then
            printf 'Missing network proof: %s\n' "$1" >&2
            docker logs "$name-receiver" >&2
            exit 1
        fi
        sleep 1
    done
}
start() {
    docker run -d --name "$name-$1" --network "$2" \
        --mount "type=bind,src=$artifacts,dst=/artifacts,readonly" \
        --mount "type=bind,src=$evidence,dst=/evidence" \
        --env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        --env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        --env QT_LOGGING_RULES=squadspeak.discovery.debug=true \
        --entrypoint /artifacts/discovery_network_probe squadspeak-ci "$3" "$1" >/dev/null
}
# No physical parent or gateway: Docker creates isolated dummy interfaces.
# Internal bridge networks filter multicast between namespaces.
docker network create --internal --driver macvlan "$name-0" >/dev/null
docker network create --internal --driver macvlan "$name-1" >/dev/null
docker network create --internal --driver macvlan "$name-2" >/dev/null
start receiver "$name-0" receiver
wait_file receiver.ready
start sender-1 "$name-1" sender
wait_file sender-1.ready
docker network connect "$name-1" "$name-receiver"
wait_file sender-1.passed
docker network disconnect "$name-1" "$name-receiver"
docker network disconnect "$name-0" "$name-receiver"
start sender-2 "$name-2" sender
wait_file sender-2.ready
docker network connect "$name-2" "$name-receiver"
wait_file sender-2.passed
touch "$evidence/stop"
for process in receiver sender-1 sender-2; do
    code=$(docker wait "$name-$process")
    docker logs "$name-$process" > "$evidence/$process.log" 2>&1
    cat "$evidence/$process.log"
    test "$code" = 0
done
printf 'Discovery and TLS chat survived two network changes. Evidence: %s\n' "$evidence"
