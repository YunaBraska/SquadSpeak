#!/bin/sh
# Runs inside the disposable Linux CI container with read-only source files.
set -eu

mode=${1:-release}
if test "$#" -gt 0; then shift; fi
if test -n "${SQUADSPEAK_VERSION:-}"; then set -- "-DSQUADSPEAK_VERSION=$SQUADSPEAK_VERSION" "$@"; fi
case "$mode" in
    runtime)
        # No compiler, development headers, test libraries or build tree.
        if command -v cmake || command -v c++; then
            printf 'Runtime image must not contain build tools.\n' >&2
            exit 1
        fi
        work=$(mktemp -d)
        server_pid=
        trap 'if test -n "$server_pid"; then kill "$server_pid" 2>/dev/null || true; fi; rm -rf "$work"' EXIT
        tar -xzf "/output/squadspeak-linux-$(uname -m).tar.gz" -C "$work"
        ldd "$work/bin/squadspeak" > "$work/runtime-libraries.txt"
        cat "$work/runtime-libraries.txt"
        for module in Multimedia Qml; do
            library="libQt6$module.so.6"
            test -s "$work/lib/$library"
            resolved=$(awk -v library="$library" '$1 == library && $2 == "=>" { print $3 }' "$work/runtime-libraries.txt")
            # The loader may retain bin/../lib from $ORIGIN in its output.
            test "$(readlink -f "$resolved")" = "$(readlink -f "$work/lib/$library")"
        done
        export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_QUICK_CONTROLS_STYLE=Basic
        export XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" XDG_RUNTIME_DIR="$work/runtime"
        mkdir -m 700 "$XDG_RUNTIME_DIR"
        test -z "${LD_LIBRARY_PATH:-}"
        timeout 30 "$work/bin/squadspeak" --smoke-test --settings-file "$work/gui/audio.ini" --screenshot "$work/settings.png"
        test -s "$work/settings.png"
        timeout 15 "$work/bin/squad_image_worker" < /source/ui/icons/app.png > "$work/sanitized.png"
        test -s "$work/sanitized.png"
        mkfifo "$work/commands" "$work/replies"
        # The process deadline also bounds reads from an unresponsive server.
        # Reopening the same profile exercises the installed QSQLITE driver.
        for launch in 1 2; do
            printf 'Checking runtime-only headless start %s.\n' "$launch"
            timeout 30 "$work/bin/squadspeak" --headless --settings-file "$work/server/audio.ini" \
                --identity-file "$work/identity.pem" < "$work/commands" > "$work/replies" &
            server_pid=$!
            exec 3>"$work/commands" 4<"$work/replies"
            attempt=0
            while :; do
                printf '{"command":"status"}\n' >&3
                IFS= read -r reply <&4
                case "$reply" in *'"hosting":true'*) break ;; esac
                attempt=$((attempt + 1))
                test "$attempt" -lt 20 || { printf '%s\n' "$reply"; exit 1; }
                sleep 1
            done
            case "$reply" in *'"hostParticipants":[]'*) ;; *) printf '%s\n' "$reply"; exit 1 ;; esac
            printf '{"command":"quit"}\n' >&3
            IFS= read -r reply <&4
            case "$reply" in *'"command":"quit"'*'"ok":true'*) ;; *) printf '%s\n' "$reply"; exit 1 ;; esac
            exec 3>&- 4<&-
            wait "$server_pid"
            server_pid=
            test -s "$work/server/audio.ini.channel.json.chat.sqlite"
        done
        printf 'Runtime-only archive: GUI, PNG worker and two headless starts passed.\n'
        exit 0
        ;;
    release) set -- -DCMAKE_BUILD_TYPE=Release "$@" ;;
    store)
        set -- -DCMAKE_BUILD_TYPE=Release -DSQUADSPEAK_STORE_BUILD=ON \
            -DSQUADSPEAK_GITHUB_CLIENT_ID=fixture_client "$@"
        ;;
    sanitizers)
        # GCC rejects valid constexpr function-pointer comparisons in Abseil
        # under UBSan (abseil/abseil-cpp#1634). Keep all checks with Clang.
        export CC=clang CXX=clang++
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
        set -- -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" \
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined \
            -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined "$@"
        export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        export QT_LOGGING_RULES='squadspeak.media.debug=true'
        ;;
    *) printf 'Usage: linux-ci.sh [release|store|sanitizers|runtime] [CMake options...]\n' >&2; exit 2 ;;
esac

test -f /source/CMakeLists.txt
test -d /output
if test "$mode" = sanitizers; then
    "$CXX" -g -fsanitize=address /source/tests/fontconfig_lsan_probe.cpp \
        $(pkg-config --cflags --libs fontconfig) -o /tmp/fontconfig-lsan-probe
    /tmp/fontconfig-lsan-probe > /output/fontconfig-reachability.log 2>&1
    if LSAN_OPTIONS=suppressions=/source/tests/lsan.supp /tmp/fontconfig-lsan-probe leak \
        > /output/leak-canary.log 2>&1; then
        printf 'Leak detection failed to reject the intentional allocation.\n' >&2
        exit 1
    fi
    grep -q 'LeakSanitizer: detected memory leaks' /output/leak-canary.log
fi
export XDG_RUNTIME_DIR=/tmp/squadspeak-ci-runtime
export XDG_DATA_HOME=/tmp/squadspeak-ci-data
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
chmod 700 "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
printf '%s' isolated-ci-keyring | gnome-keyring-daemon --unlock --components=secrets
# Qt excludes sink monitors from microphone enumeration. Remap the silent
# monitor as an input device so settings tests exercise both device directions.
pulseaudio --start --exit-idle-time=-1
pactl load-module module-null-sink sink_name=ci_audio rate=48000 channels=2
pactl load-module module-remap-source master=ci_audio.monitor source_name=ci_microphone
pactl set-default-sink ci_audio
pactl set-default-source ci_microphone
export SQUADSPEAK_TEST_INPUT=present SQUADSPEAK_CAPTURE_DIAGNOSTICS=1
trap 'pulseaudio --kill' EXIT

dpkg-query -W > /output/packages.txt
cmake -S /source -B /tmp/squadspeak-ci-build -G Ninja -DBUILD_TESTING=ON "$@"
cmake --build /tmp/squadspeak-ci-build --parallel 4
cp /tmp/squadspeak-ci-build/discovery_network_probe /output/
# Each repetition starts a fresh X server, window manager and capture producer.
if test "$mode" != store; then
    ctest --parallel 2 --test-dir /tmp/squadspeak-ci-build --output-on-failure --no-tests=error \
        -R '^capture_contract$' --repeat until-fail:5 \
        --output-junit /output/capture-startup.xml --output-log /output/capture-startup.log
    # The same libpulse API must isolate applications when PipeWire supplies
    # it. Use a separate runtime directory and bus; leave the main PulseAudio
    # server and its virtual microphone intact for the remaining contracts.
    # Sway refuses root. Keep both PipeWire capture modes in one disposable
    # unprivileged session, apart from the main test server and keyring.
    useradd --system --user-group --create-home --home-dir /tmp/squadspeak-capture-home --shell /bin/sh squadspeak-capture
    install -d -o squadspeak-capture -g squadspeak-capture /output/pipewire
    # Activated portal services must inherit the same session as the processes
    # started below, not the root-owned runtime directory of the main tests.
    runuser -u squadspeak-capture -- env \
        XDG_RUNTIME_DIR=/tmp/squadspeak-ci-pipewire \
        XDG_STATE_HOME=/tmp/squadspeak-ci-pipewire-state \
        XDG_DATA_HOME=/tmp/squadspeak-ci-pipewire-data \
        XDG_CONFIG_HOME=/tmp/squadspeak-ci-pipewire-config \
        PULSE_SERVER=unix:/tmp/squadspeak-ci-pipewire/pulse/native \
        dbus-run-session -- sh -eu <<'PIPEWIRE'
mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
reports=/output/pipewire
core= session= pulse= compositor= portal= backend= documents=
trap 'for pid in "$portal" "$backend" "$documents" "$compositor" "$pulse" "$session" "$core"; do
    if test -n "$pid"; then kill "$pid" 2>/dev/null || true; fi
done
wait || true' EXIT
pipewire > "$reports/pipewire.log" 2>&1 & core=$!
attempt=0
until test -S "$XDG_RUNTIME_DIR/pipewire-0"; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || { cat "$reports/pipewire.log"; exit 1; }
    kill -0 "$core"
    sleep 1
done
wireplumber > "$reports/wireplumber.log" 2>&1 & session=$!
pipewire-pulse > "$reports/pipewire-pulse.log" 2>&1 & pulse=$!
attempt=0
until pactl info > "$reports/pipewire-server.log" 2>&1; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || { cat "$reports/pipewire-server.log"; exit 1; }
    kill -0 "$core" "$session" "$pulse"
    sleep 1
done
pactl load-module module-null-sink sink_name=ci_audio rate=48000 channels=2
pactl set-default-sink ci_audio
# Direct execution keeps the build directory root-owned and read-only here.
for repeat in 1 2 3; do
    QT_QPA_PLATFORM=xcb WAYLAND_DISPLAY= XDG_SESSION_TYPE=x11 LSAN_OPTIONS=suppressions=/source/tests/lsan.supp \
        timeout 120 xvfb-run -a /tmp/squadspeak-ci-build/capture_tests -v2 \
        -o "$reports/capture-x11-$repeat.xml",junitxml -o -,txt \
        > "$reports/capture-x11-$repeat.log" 2>&1 || { cat "$reports/capture-x11-$repeat.log"; exit 1; }
done
mkdir -p "$XDG_CONFIG_HOME/sway" "$XDG_CONFIG_HOME/xdg-desktop-portal" "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr"
printf '%s\n' 'output * mode 640x480' 'output * bg #202020 solid_color' 'default_border none' 'xwayland disable' \
    > "$XDG_CONFIG_HOME/sway/config"
printf '%s\n' '[preferred]' 'org.freedesktop.portal.ScreenCast=wlr' > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf"
printf '%s\n' '[screencast]' 'chooser_type=none' \
    > "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman
export XDG_CURRENT_DESKTOP=sway XDG_SESSION_TYPE=wayland
unset WAYLAND_DISPLAY DISPLAY
sway --config "$XDG_CONFIG_HOME/sway/config" > "$reports/sway.log" 2>&1 & compositor=$!
attempt=0
while test -z "${WAYLAND_DISPLAY:-}"; do
    for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
        if test -S "$socket"; then export WAYLAND_DISPLAY="${socket##*/}"; break; fi
    done
    test -n "${WAYLAND_DISPLAY:-}" && break
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || { cat "$reports/sway.log"; exit 1; }
    kill -0 "$compositor"
    sleep 1
done
dbus-update-activation-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE
/usr/libexec/xdg-document-portal > "$reports/document-portal.log" 2>&1 & documents=$!
attempt=0
until dbus-send --session --print-reply --reply-timeout=1000 --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner string:org.freedesktop.portal.Documents | grep -q 'boolean true'; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || { cat "$reports/document-portal.log"; exit 1; }
    kill -0 "$documents"
    sleep 1
done
/usr/libexec/xdg-desktop-portal-wlr --loglevel=DEBUG > "$reports/portal-wlr.log" 2>&1 & backend=$!
/usr/libexec/xdg-desktop-portal --verbose > "$reports/portal.log" 2>&1 & portal=$!
attempt=0
# Checking the name does not activate a competing portal during startup.
until dbus-send --session --print-reply --reply-timeout=1000 --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner string:org.freedesktop.portal.Desktop | grep -q 'boolean true'; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || { cat "$reports/portal.log" "$reports/portal-wlr.log"; exit 1; }
    kill -0 "$portal" "$backend" "$compositor"
    sleep 1
done
dbus-send --session --print-reply --reply-timeout=1000 --dest=org.freedesktop.portal.Desktop \
    /org/freedesktop/portal/desktop org.freedesktop.DBus.Properties.Get \
    string:org.freedesktop.portal.ScreenCast string:version > "$reports/portal-ready.log" 2>&1
QT_QPA_PLATFORM=wayland SQUADSPEAK_TEST_WAYLAND=isolated QT_LOGGING_RULES='qt.multimedia.pipewire.capture=true' \
    LSAN_OPTIONS=suppressions=/source/tests/lsan.supp \
    timeout 120 /tmp/squadspeak-ci-build/capture_tests -v2 \
    -o "$reports/capture-wayland.xml",junitxml -o -,txt \
    > "$reports/capture-wayland.log" 2>&1 || { cat "$reports/capture-wayland.log"; exit 1; }
if grep -q '^SKIP *:' "$reports"/capture-*.log; then exit 1; fi
PIPEWIRE
fi
result=0
ctest --parallel 2 --test-dir /tmp/squadspeak-ci-build --output-on-failure --no-tests=error --stop-on-failure \
    --output-junit /output/ctest.xml --output-log /output/ctest.log || result=$?
cp /tmp/squadspeak-ci-build/Testing/Temporary/LastTest.log /output/test-details.log
if test -f /tmp/squadspeak-ci-build/audio-corpus/report.json; then
    cp /tmp/squadspeak-ci-build/audio-corpus/report.json /output/audio-report.json
fi
if test -d /tmp/squadspeak-ci-build/smoke; then
    cp -R /tmp/squadspeak-ci-build/smoke /output/screenshots
fi
test "$result" -eq 0

# Repeated chat/theme churn exposed a Qt 6.10 allocator fault after the full UI
# suite. Keep that sequence, with frequent collection and no retry-on-failure.
attempt=1
while test "$attempt" -le 20; do
    printf 'Chat collection repetition %s\n' "$attempt" >> /output/chat-collection.log
    QV4_GC_TIMELIMIT=1 QV4_JIT_CALL_THRESHOLD=1 QTEST_DISABLE_STACK_DUMP=1 \
        LSAN_OPTIONS=suppressions=/source/tests/lsan.supp \
        SQUAD_TEST_ARTIFACTS=/output/screenshots \
        timeout 60 /tmp/squadspeak-ci-build/controls_tests \
        -input /source/tests/tst_channel_controls.qml \
        DirectChannelControls::test_markdownCodeAndQuotesStayReadable \
        DirectChannelControls::test_markdownStructuresStayWithinChat \
        >> /output/chat-collection.log 2>&1 || { cat /output/chat-collection.log; exit 1; }
    attempt=$((attempt + 1))
done

# Exercise the unavailable-device path separately. The main run above must
# still see the virtual microphone; losing it cannot silently reduce coverage.
SQUADSPEAK_TEST_INPUT=absent PULSE_SERVER=unix:/tmp/nonexistent-squadspeak-ci \
    /tmp/squadspeak-ci-build/controls_tests -input /source/tests/tst_channel_controls.qml \
    DirectChannelControls::test_voiceActivationCanBeToggledAndAdjustedInInput \
    DirectChannelControls::test_inputCurveTracksGainWhilePreviewIsStopped \
    > /output/no-input-device.log 2>&1 || result=$?
cat /output/no-input-device.log
test "$result" -eq 0
python3 - <<'CHECK'
from pathlib import Path
import re
for name in ("test-details.log", "no-input-device.log"):
    if re.search(r"^SKIP\s+:", (Path("/output") / name).read_text(), re.MULTILINE):
        raise SystemExit(f"A contract was skipped; inspect {name} before accepting this run.")
CHECK

# Instrumented and Store test builds are verification artifacts, never direct packages.
if test "$mode" != release; then exit 0; fi

cmake --install /tmp/squadspeak-ci-build --prefix /tmp/squadspeak-ci-install
cmp /source/LICENSE /tmp/squadspeak-ci-install/share/squadspeak/LICENSE
cmp /source/ui/app.squadspeak.desktop /tmp/squadspeak-ci-install/share/applications/app.squadspeak.desktop
cmp /source/ui/icons/app.png /tmp/squadspeak-ci-install/share/icons/hicolor/256x256/apps/app.squadspeak.png
diff -r /source/docs/third-party /tmp/squadspeak-ci-install/share/squadspeak/THIRD_PARTY_NOTICES
archive="/output/squadspeak-linux-$(uname -m).tar.gz"
tar -C /tmp/squadspeak-ci-install -czf "$archive" .
archive_root=/tmp/squadspeak-ci-archive-check
rm -rf "$archive_root"
mkdir -p "$archive_root"
tar -xzf "$archive" -C "$archive_root"
# Exercise the exact published archive with no build-tree library search path.
env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH PATH=/usr/bin:/bin \
    XDG_CONFIG_HOME=/tmp/squadspeak-ci-archive-config \
    XDG_DATA_HOME=/tmp/squadspeak-ci-archive-data \
    "$archive_root/bin/squadspeak" --smoke-test \
    --settings-file /tmp/squadspeak-ci-archive-profile/audio.ini --screenshot /output/installed.png
# Exercise the same production subprocess contract against the extracted app.
SQUAD_TEST_APP="$archive_root/bin/squadspeak" \
    env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH PATH=/usr/bin:/bin \
    XDG_CONFIG_HOME=/tmp/squadspeak-ci-archive-config \
    XDG_DATA_HOME=/tmp/squadspeak-ci-archive-data \
    /tmp/squadspeak-ci-build/headless_tests productionServerBecomesReadyWithoutGuiOrAudio:argument-identity-file \
    > /output/package-headless.log 2>&1
cat /output/package-headless.log
