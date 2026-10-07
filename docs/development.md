# Development

[Product requirements](specs/voice-chat.md), [feature status](roadmap.md), [verification evidence](verification.md) and [avatar production](specs/avatars.md) are the project references. The README is for people using the app.

## Translations

Each language has one Qt Linguist catalog in `ui/i18n/squadspeak_<code>.ts` and
Apple permission text in `<code>.lproj/InfoPlist.strings`. Catalogs drive the
language menu and bundled resources. Do not add a parallel language registry.
The required language set is fixed in the specification and session contract.

Correct the translation rather than its English source key. Keep `%1`, `%2`
and `%n` placeholders, literal configuration values such as `true`/`false`,
URLs and keyboard names intact. A low cut removes bass; a high cut removes
treble. Microphone gain means amplification, and PTT keys are keyboard controls,
not license or encryption keys. Check mute/unmute action polarity explicitly.

Run `translation_sources` and `session_contract` after rebuilding. The former
compares catalogs with Qt's source extraction; the latter checks the promised
languages, complete compiled translations, placeholders and persistence.
`channel_controls` also checks language switching and compact/RTL layouts.
Technical checks cannot certify natural wording; report corrections through
the repository issue tracker with the language, screen and suggested wording.

Desktop command-line help and startup diagnostics use the saved profile language.
Help does not start services or acquire a profile lock. Invalid saved fields name
their stable JSON key; unreadable JSON or an invalid language uses English at
startup because no usable language preference is available. Profile files are
never repaired or overwritten after a failed load.

## Chat persistence

Default profile paths come from `QStandardPaths::AppConfigLocation` on each OS;
`--settings-file` overrides the profile. Host history uses
`<profile>.channel.json.chat.sqlite` and encrypted image blobs beside it in
`<profile>.channel.json.chat.images`. Test fixtures may use a different profile suffix.

Message text, names, avatars and receipts are authenticated ciphertext. SQLite
indexes contain sequence numbers, UTC creation/expiry times, image hashes and
keyed lookup tokens; the entire database file is not opaque ciphertext. Database
transactions finish before delivery is acknowledged. Pages remain bounded to
40 messages/40 KiB; the desktop keeps at most 160 messages/512 KiB in memory.
Expired content is hidden immediately and reclaimed in bounded maintenance
batches. The old encrypted JSON file migrates atomically and is removed only
after commit. Older binaries cannot read the new storage format.

Free hosts select 24h/7d/30d; Supporter adds 90d/180d/360d through Channel Info,
`--message-ttl`, `SQUADSPEAK_MESSAGE_TTL`, `messageTtl` or the JSON `configure`
command. Readers need no pass. An inactive pass limits new messages to 30 days
without shortening old receipts. Times use UTC epoch milliseconds; only the
chat timestamp display follows the user's system timezone.

## Build and test

The app uses C++20, Qt 6.10 or newer with Multimedia, Quick, Sql (QSQLITE) and LinguistTools, OpenSSL 3, Opus and libsamplerate. Pinned sources supply RNNoise, QtKeychain and WebRTC Audio Processing 2.1 with static Abseil 20240722.0. Pinned libdatachannel and its bundled dependencies supply encrypted UDP media. CMake, Ninja and Meson are build tools; Meson uses the audio library's upstream source lists. Radio requires Qt Multimedia's FFmpeg backend; the Darwin backend alone cannot provide `QAudioBufferOutput`. The video codec libraries must match the selected Qt multimedia kit.

Qt 6.10 provides the public foreign-window capture constructor used to associate
Windows windows with their owning process. The package keeps Windows 10 support
(version 1903 or newer). Qt 6.11 supports
[Windows 10](https://doc.qt.io/qt-6.11/windows.html); the later 1903 floor comes
from the app's [UTF-8 process manifest](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).
Screen audio requires Windows 11 or Server 2022 (build 20348), the minimum for Microsoft's
[process-loopback API](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/).
Window audio includes only the selected process tree; full-screen audio excludes
SquadSpeak itself. Older systems keep screen video and ordinary voice/audio.
Native CI uses Server 2022. [GitHub's standard runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
do not include Windows 10, so those results do not certify a Windows 10 runtime.
The native audio test requires a working render endpoint, physical or virtual.
The GitHub-hosted Windows job provisions VB-CABLE with `tests/windows-audio.ps1`
before building. It verifies the pinned official archive, the catalog signature
and kernel-signing coverage of the driver and INF. It does not disable signature
enforcement or modify trusted certificate stores. This setup refuses non-hosted
machines and is never included in SquadSpeak packages. VB-CABLE is
[donationware](https://vb-audio.com/Services/licensing.htm), evaluated here as CI
equipment; it is not a dependency for app users.

Linux application audio uses libpulse and X11 window/process metadata. It monitors
individual matching output streams, including current child processes, and never
silently substitutes a whole-output monitor for a selected window. Stream changes
are reconciled through Pulse subscriptions. When the window owner is unavailable,
the UI offers an initially disabled `Computer audio` option. Enabling it explicitly
shares the output mix. Losing an already selected app never widens its scope.
The native Wayland fixture verifies monitor video and explicit computer audio
through an isolated wlr portal; the X11 two-tone fixture verifies application
isolation against both PulseAudio and PipeWire-Pulse. This does not certify
every compositor or containerized application. The current Qt Wayland picker
offers monitors only; window selection remains unavailable there.

```sh
cmake -S . -B build/local -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/local --parallel 4
ctest --test-dir build/local --parallel 2 --output-on-failure
```

During development, build and run the affected contract first. CTest entries
are suites: Qt expands their data rows inside one process, not one build per
row. For example, a history change can use:

```sh
cmake --build build/local --target chat_history_tests --parallel 4
ctest --test-dir build/local -R '^chat_history_contract$' --output-on-failure
```

Use `ctest --test-dir build/local -N` to list suites; Qt test executables accept
`-functions` and `function:data-row` to select a regression. Then run the
affected integration suites before the full release matrix. CLI subprocess,
malformed-protocol, codec-lifetime and DPI checks prove different behavior;
a passing end-to-end smoke test does not replace them. CI caches the pinned Qt
SDK on Windows and macOS; application builds and test results are never cached.
CTest runs two suites concurrently. Native loopback capture, video timing and
64-client load checks run alone; desktop/audio fixtures share a resource lock.
Windows also caches its installed OpenSSL/Opus/libsamplerate prefix, keyed by
compiler, SDK and build-script inputs. CI checks preparation and reuse before
building the app; incompatible or incomplete prefixes fail explicitly.

`LocalChannel` retains the public API and connection ownership. Its discovery
member owns multicast membership, scan sockets and cancellation. Chat/image and
remote-control handlers live in separate implementation files and share the
same framing, validation and peer state; there is no second protocol model.

For macOS, install CMake, Ninja, Meson, pkg-config and NASM. The reproducible
package path builds deployment-compatible audio, TLS and FFmpeg libraries.
FFmpeg 7.1.5 includes VideoToolbox dropped-frame and failed-initialization
cleanup recorded in CMakeLists.txt. Build-tree tests and the installed Qt multimedia plugin use
the same patched runtime; package checks reject duplicate or unpatched copies.

```sh
sh tests/macos-ci.sh /path/to/Qt/6.11.3/macos build/macos-ci
```

For Windows x86_64, use an MSVC 2022 developer shell and the Qt 6.11.3
`win64_msvc2022_64` kit, with CMake, Ninja, Meson, Python 3.12 or newer, Perl,
the pinned NASM 3.2.0 package (upstream binary 3.02) and Git Bash available.
The CI job installs and verifies that package before configuring OpenSSL so its
optimized assembly remains enabled:

```powershell
./tests/windows-ci.ps1 -QtRoot C:/Qt/6.11.3/msvc2022_64 -BuildRoot build/windows-ci
```

The script builds pinned audio/TLS dependencies and derives import libraries
from Qt's own FFmpeg DLLs. Its packaged smoke checks remove build directories
from `PATH`; the existing headless subprocess test targets the installed app
through `SQUAD_TEST_APP`. The archive includes Microsoft's official Visual C++
Redistributable installer, whose presence and Microsoft signature are checked.
Users run it once before the app; the CI runner already has that runtime, so its
start check alone cannot prove installation on a bare Windows system.
The Windows application manifest requests a UTF-8
process codepage for Qt help and diagnostics; packaged checks cover German,
Japanese and Arabic output. Windows runs the full CTest suite, including the UI
and audio corpus, and preserves its JUnit report. The hosted job remains the required platform proof,
not the existence of this script.

For Linux, `tests/linux.Dockerfile` and `tests/linux-ci.sh` define the same isolated dependency, virtual-audio and Secret Service environment used by CI. Run `release`, `store` or `sanitizers`; mount the checkout read-only at `/source` and a writable artifact directory at `/output`. Release and sanitizer capture tests additionally need Docker's `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined` for the real desktop portal's private document mount. The default Docker AppArmor profile denies that mount even with the device and capability. Use a disposable Linux runner; Store and runtime-only package checks do not require these privileges. `tests/discovery-network.sh` additionally exercises independent network namespaces.

Linux packages use the system Qt runtime, including `libqt6sql6-sqlite` for encrypted chat storage. With Qt 6.10.2, CMake rebuilds and bundles the ABI-matching Multimedia and Qml libraries. `cmake/qt-wayland.patch` contains the portal request routing, cancellation and session cleanup fixes; `cmake/qt-qml-gc.patch` backports the QML incremental-GC transition and sweep corrections. These builds need Qt's matching private development headers. Qt 6.12 includes the upstream session fixes; other older versions are rejected rather than mixing Qt ABIs. The `runtime` stage of `tests/linux.Dockerfile` installs only runtime packages. CI extracts the archive as a non-root user, verifies that it resolves the bundled capture and Qml libraries, and checks the GUI, image worker and headless entrypoint without development packages or a build tree. After the full UI suite, Linux CI repeats the compact Markdown/theme cases 20 times with frequent garbage collection and stops on the first failure.

Linux packages use system fonts. Install `fonts-noto-core` and `fonts-noto-cjk`
alongside the runtime libraries to cover the offered scripts. The language-switch
test checks shaped glyphs for language names and settings tabs, including font
fallback; it does not certify every ligature or natural wording.

Tests use isolated identities, local TLS/HTTP servers and synthetic media. They do not need a purchase account or access to a real microphone. Native permission prompts, device drivers, external radio uptime and subjective listening quality still need separate platform evidence.

### Device checks with limited human time

The maintainer runs and diagnoses automation before asking someone to test.
Use the existing suites, not another manual chat/kick/ban checklist:

| Evidence | Unattended entrypoint | Human task left |
| --- | --- | --- |
| Admission, history, moderation, remote control and compatible protocol extensions | `channel_contract`, `headless_contract`, `channel_controls` | None for these deterministic contracts |
| Capture scope, audio isolation, source loss and restart | `capture_contract` on Windows/Linux; opt-in `capture_tests` on macOS | One-time macOS consent when needed |
| Noise, speech retention, clipping, echo drift and double-talk models | `audio_contract`, `mixer_contract`, `audio_corpus` | Short listening observation on the actual microphone/headset; simulated acoustics do not certify a room |
| Discovery changes, loss, quality adaptation, media load and lifetime | `discovery-network.sh`, media/channel/video contracts and the longer runs below | None for synthetic load/impairment; do not ask someone to create 64 clients |
| Packaged startup, native backends and leaks | Native CI, runtime-only package checks, sanitizers and leak probes | Windows 10 startup on an available PC; Server 2022 is not Windows 10 |
| Chat sound routing and notifications | Channel/UI/PCM contracts | Actual Apple notification and Focus behavior |

On macOS, prepare two isolated copies with `tests/prepare_two_apps.py` and join
them to one test channel before handing over. For a Windows visit without a
maintainer present, use the standalone kit below. A captures the microphone,
B plays through headphones; both start muted and deafened. Do not reset working
OS permissions or touch the person's normal profile. The maintainer handles
builds, packet measurements and log analysis. Allow initial installation and
setup time separately from the three-minute listening/device observation.

Generate the offline German handoff with the exact source checkout and build
being supplied. The tester needs only a browser, chooses an outcome per step
and downloads one JSON file. Nothing is uploaded or recorded by the page:

```sh
python3 tests/acceptance.py --platform macos \
  --setup 'Exact OS version / computer model / microphone / headphones' \
  --build '<tested commit and package version>' \
  --runtime '<verified compiler, Qt, codec versions and build preset>' \
  --junit /path/to/ctest.xml --output build/device-check/mac.html
```

Use `--platform windows` for the short PC visit; it adds package startup and
omits Apple notifications. The tester can enter the actual Windows version,
microphone and headset in the page before evaluating checks. Editing that field
resets observations, so results cannot silently migrate to a different setup.
No headset means a blocked device-change observation, not a failed application
or a request to buy hardware. A problem needs only a short note about the step;
the maintainer investigates it before requesting another attempt.

#### Windows delivery without on-site help

1. Freeze the candidate revision and the existing release criteria. Run Windows
   CI before the visit. A green older commit does not certify local edits; the
   hosted runner is Windows Server 2022, not a Windows 10 hardware substitute.
2. The Windows job prepares `squadspeak-windows-device-test.zip` as the separate
   `windows-device-test` workflow artifact. It contains the packaged app, its
   runtime DLLs, Microsoft's runtime installer, `device-test/Start.cmd`, two
   isolated profiles and `device-test/Check.html`. The regular release ZIP stays
   unchanged. CI runs the same launcher with `--smoke-test`, checking both
   profiles through the packaged executable without opening microphones.
3. Download and unpack the workflow artifact on the Mac before the visit. Verify
   the successful run's revision and take its inner device-test ZIP on USB or
   another normal file-transfer medium. The tester should not need GitHub
   authentication, Python, PowerShell setup, Qt or a compiler. Do not substitute
   the older private release draft for the verified candidate.
4. On Windows, extract the whole inner ZIP and double-click
   `device-test/Start.cmd`. The page explains local joining via `127.0.0.1:48764`,
   approval, headphone routing, the short hearing check and saving the result.
   This loopback setup avoids discovery variability for the listening check;
   it is not LAN discovery evidence. The tester stops at a failure and brings
   the JSON receipt plus a screenshot or the two `app.log` files. Never request
   the profile directories: they contain device identities. Both apps are
   closed through their tray menus with `Quit`; closing a window is not quitting.

To prepare the same kit locally from an already extracted Windows package:

```sh
python3 tests/acceptance.py --platform windows --setup '' \
  --build '<exact package revision>' --runtime '<verified build identity>' \
  --junit /path/to/windows-ctest.xml \
  --windows-package /path/to/extracted-package \
  --output build/device-check/windows.html
```

The generator refuses to overwrite an existing `device-test` directory. Kits
start quiet only on first use; subsequent starts restore their last audio state,
as the app normally does. The instructions require checking the switches before
another listening attempt. A blocked executable is recorded, not worked around
by disabling Windows security. Missing C++ runtime uses the bundled signed
installer. Subjective failures do not become requests for weekend debugging.

The HTML file is an offline instruction/result sheet, not an app frontend.
SquadSpeak remains native C++/Qt. The report's own tests only verify preparation
and evidence handling; they are not application acceptance. Keep the candidate
fixed while evaluating it, fix observed regressions with a reproducing test,
and rerun affected automation before handing over a replacement. Do not reopen
unrelated features or require repeated human checks after documentation changes.

Keep the downloaded receipt privately with the release evidence, then pass it
as `--previous /path/to/squadspeak-device-check.json` when preparing the next
check. Matching setup, verified runtime identity, instructions and affected
source hashes retain the original outcome, time and tested build. Documentation
changes do not require another hearing test. Build inputs, shared UI/protocol
code and unclassified runtime files conservatively invalidate all observations;
known subsystem changes invalidate their checks. Missing runtime identity
disables reuse. Review this mapping when moving responsibilities between files.
This is traceability of human observations, not binary attestation or a cached
release gate. The maintainer must match the supplied binary to the checkout and
update the runtime identity for compiler, dependency or build-option changes.

The page distinguishes pending, passed, failed and blocked checks. It preserves
partial work through the downloaded receipt; it does not persist notes in the
browser. Provided JUnit results show failed/skipped cases, including nested Qt
skips, and identify their source file hashes. They are not assumed to be a full
matrix. Never promote one headset observation to Bluetooth/room certification.
Full acoustic echo/double-talk evidence is still separate; prepare one bounded
session and keep usable evidence instead of repeating a broad manual checklist
after every unrelated change. A/V timing and sustained performance remain
automated engineering work, not subjective user checkboxes.

The existing media contracts also support longer runs without retaining all
received packets. Run them directly so a normal CTest timeout does not cut a
soak short:

```sh
SQUAD_SOAK_SECONDS=300 QTEST_FUNCTION_TIMEOUT=600000 build/local/channel_tests sixtyFourClientsHaveOneConsistentRosterAndAudioFanout
SQUAD_SOAK_SECONDS=10 SQUAD_SOAK_SPEAKERS=64 build/local/channel_tests sixtyFourClientsHaveOneConsistentRosterAndAudioFanout
SQUAD_VIDEO_SOAK_SECONDS=120 build/local/video_tests roundTripAndQualityChanges:native-rgb
```

Audio accepts 2-1800 seconds; video accepts 0-1800, where zero keeps the normal
short test. Audio uses four simultaneous senders by default; `SQUAD_SOAK_SPEAKERS`
accepts 2-64 to measure greater overlap while keeping all 64 clients connected.
Set `QTEST_FUNCTION_TIMEOUT` above the requested duration plus setup
and teardown; Qt Test otherwise stops each function after 300 seconds.
Video extends the first 4K phase to 30 frames per requested second,
then checks quality changes and recovery. It reports actual elapsed time,
encoded bytes, codec and processing time, so a slow encoder is not reported as
30 fps. Use external process sampling for CPU/RSS; these fixtures combine the
sender and receivers in one process and do not measure acoustic latency.

The 64-client audio test keeps its 90% real-time playout threshold in
uninstrumented builds. AddressSanitizer builds log that this timing assertion
is disabled; they still require exact packet delivery and audible output.
The 64-viewer test checks 1080p frame delivery and decoding, then isolates a
delayed connection at the lowest video tier on every build. This permits the
shared runner to adapt honestly when its own CPU cannot sustain 30 fps.

The optional `license_provider_probe` target exercises the production `License`
class against Lemon Squeezy. It is excluded from normal builds and CTest. Use a
fresh test-mode license with three free slots and save the JSON response from
`POST https://api.lemonsqueezy.com/v1/licenses/validate` privately. Build with
`cmake --build build --target license_provider_probe`, then run
`build/license_provider_probe /private/path/test-validation.json`. It checks three
activations, persisted-slot reuse, rejection of a fourth device, deactivation and
reuse, then releases its activations. It never uses the OS account's activation
or keychain. A network failure can leave an uncertain slot; inspect the test
license in the merchant dashboard before retrying. Never use a live customer key
or commit the input file.

On a Mac with Screen Recording permission, run the opt-in capture test:

```sh
build/local/capture_tests.app/Contents/MacOS/capture_tests
```

It opens two independent application windows, plays distinct quiet test tones,
captures each selected window, and sends image and audio to an authenticated
local receiver. It verifies application isolation, playback restart, audio
toggling, revocation/restart and source disappearance. It does not capture the
microphone. CTest deliberately does not run this macOS hardware test on unattended runners without OS consent.
Windows and Linux run the same fixture as `capture_contract`. A working audio
render endpoint is required, including for Windows process loopback. Linux CI
provides an isolated PulseAudio null sink, Xvfb and an owned Openbox session.
A separate unprivileged PipeWire session also runs X11 repetitions and a headless
Sway desktop with the real ScreenCast portal. Its sole output contains only test
windows; automatic selection is confined to this fixture. The Wayland gate checks
initially disabled computer audio, external-audio delivery, exclusion of the
sender's own process tree, disable/restart and entitlement revocation.
These are test-only dependencies. Store builds omit this Supporter contract.
The fixture pins wlr portal 0.8.4: Ubuntu's 0.8.1 omits readable buffer flags
required by PipeWire 1.6, causing capture clients to crash. The upstream
[buffer-flags fix](https://github.com/emersion/xdg-desktop-portal-wlr/commit/459831374d4bc07517d6526da9e78b0c15a33eed)
is required on affected wlroots desktops too; updating SquadSpeak alone cannot
repair the system portal. This condition does not apply to the GNOME/KDE portals.

`screen_rendering` checks decoded pixels in the real Qt video renderer, including
the separate resizable viewer. `desktop_rendering_smoke` checks app startup and
shutdown through that renderer. Linux uses Xvfb/OpenGL, macOS Cocoa/Metal and
Windows D3D11/WARP. Basic software scene-graph tests cannot verify VideoOutput.
Screenshots are retained in each platform's CI artifacts.

For fresh UI screenshots, set `SQUAD_TEST_ARTIFACTS` to an output directory when running `controls_tests -input tests/tst_channel_controls.qml`. Add a test function name to select one scenario. The README images come from the English chat and settings scenarios.

## Headless server

Normal startup opens the desktop app. `--headless` selects the server entrypoint, without loading personal GUI/audio profiles or joining a channel as a user.

```sh
squadspeak --headless -f /path/to/application.properties
```

Example properties:

```properties
channelName=Evening lounge
botName=System
port=48763
messageTtl=24h
language=en
passwordFile=/private/path/channel-password.txt
settingsFile=/private/path/server
```

Values resolve in this order: command-line arguments, `SQUADSPEAK_*` environment variables, properties, saved server values. Without `-f`, the app reads `application.properties` beside its executable/app, then `config/application.properties` there. The second file overrides the first. Relative file values resolve beside their configuration file.

Headless uses English by default. Select a catalog with `--language de`, `SQUADSPEAK_LANGUAGE=de`, or `language=de` in the preset; the same precedence applies. This selection is independent of desktop preferences. Help and diagnostics are translated; JSON field names, command names and enum values stay unchanged. Invalid or empty language codes are rejected.

Use `--headless --help` for all parameters and environment names. `--ban DEVICE_ID` can be repeated. Explicit bans always apply, and a password is checked before automatic admission. Headless does not offer a deny-everyone mode.

On Linux/macOS servers without a credential store, `--identity-file /private/path/identity` selects a protected identity file. Its parent must already exist and belong to the server account. Keep this file private and backed up: it identifies the host and protects its persisted data.

The server accepts one JSON command per stdin line and writes structured results to stdout. Send `{"command":"help"}` for the command list. Chat announcements use the system bot; radio administration does not create a human participant.

Read each complete reply before sending the next command. If a pipe reader
pauses, the server pauses command intake while continuing to serve channels.
Pending input is bounded to 64 KiB plus one overflow-detection byte and output
to 2 MiB. Replies resume in order when the reader drains the pipe; `quit` waits
for its reply to drain. Closing the output pipe terminates the process as an
output failure.

The CLI covers hosted channels, admission, bans, configuration, passwords, chat
history, bot announcements, radio and Supporter activation. It has no personal
voice client, microphone, playback, PTT, avatar selection, remote-control client
or screen capture.

`{"command":"history"}` reads the latest page, limited to 40 messages and 40 KiB.
Use `direction` (`older` or `newer`) and a returned message `sequence` as `cursor`
to continue. Expired messages are excluded. `{"command":"password","value":"new secret"}`
changes the channel password; an empty value removes it. Success is reported only
after persistence, and connected members remain connected. Both commands accept
`channelId` for an additional owned channel and create no participant. Avoid
putting real secrets into shell history or logs.

Desktop and headless share one Supporter activation per OS account, independently of server profiles. Direct builds with merchant configuration accept `{"command":"license","action":"activate","key":"YOUR-KEY"}` on stdin. Use `status`, `refresh` or `deactivate` as the action to inspect, check or release that slot. Replies include the confirmed expiry and support reference, never the key. `reset` additionally requires `"confirmed":true` and is only for a slot already released by support.

With `--identity-file`, a new activation is encrypted using that protected file. The account receipt remembers its location so the desktop and other server profiles reuse the same slot. Keep that original file even if another host profile uses a different identity. Moving from existing keychain storage requires access to its original key first; unreadable storage never triggers another activation. Periodic checks and offline expiry are the same in both modes. Store builds do not activate external passes.

## Protocol extensions

Application versions do not have to match. The base wire protocol remains version
1; peers advertise optional capability names and use their intersection. Add
optional fields without changing existing field meanings. Unknown valid message
types and encrypted chat kinds are ignored after admission. Screen receivers pin
the host certificate before accepting messages, so an unknown extension may also
precede the first quality response. Invalid envelopes and malformed known
messages still fail; extension handling never bypasses admission or size limits.

For a future file-sharing feature, negotiate its capability before sending file
metadata or bytes. A host or recipient without it keeps existing voice/chat
working, but is not expected to store or relay the new attachment. Unknown fields
alone do not make arbitrary new codecs, required fields or wire versions
compatible. Public protocol regressions cover unknown fields/types, chat kinds,
screen messages, remote control and invalid-message rejection.

Admitted voice peers negotiate `udp-audio`. Pinned libdatachannel 0.24.6 supplies
ICE, DTLS and SRTP; the existing TLS identity pins the exchanged SDP fingerprint.
The pinned build corrects numeric IPv6 candidate resolution and picohash callback
signatures in `cmake/Media.cmake`; sanitizer checks remain enabled.
Opus RTP prefers UDP on the same numbered service port as TCP. Outgoing clients
use separate ephemeral UDP sockets, even when they also host locally. This avoids
ICE-multiplexer collisions between voice and screen sessions. The client probes
only the authenticated server endpoint; no external STUN/TURN service is required.
Forward both TCP and UDP when exposing a host through a router. A blocked UDP path
retains bounded TLS audio and recovers when UDP becomes usable again. Legacy peers
continue using the original TLS frames. Chat and administration remain on TLS.

Each receiver has independent loss/RTT feedback and Opus quality. Source bindings
are acknowledged before sending datagrams; at most 65 reusable SSRC slots prevent
membership churn from accumulating SRTP stream state. Sequence fences reject old
speakers' packets after reuse. A bounded 20 ms reorder window drops stale packets;
the existing Opus decoder conceals at most six missing frames. TLS fallback uses
the same sequence space. Unknown valid media kinds/fields preserve baseline audio.
The library callback mailbox is bounded, Qt timers belong to their Qt owner, and
process teardown waits for library cleanup. See [verification](verification.md)
for measured latency, lifecycle and load coverage.

Screen video negotiates `udp-screen` on its separate, admitted TLS connection.
Frames and decode acknowledgments prefer an unordered WebRTC DataChannel using
SCTP/DTLS/UDP, retaining the existing H.264 and MPEG-4 codecs on all platforms.
The standard supplies congestion control and partial reliability; the app retains
per-viewer quality control, voice priority and bounded frame assembly. This is
not RTP video and does not add browser interoperability. See
[RFC 8831](https://www.rfc-editor.org/rfc/rfc8831.html) for the transport.
Messages are at most 16 KiB, packet lifetime is 100 ms, SCTP buffers are 256 KiB,
and application buffering stops above 32 KiB. Each viewer retains at most one
2 MiB encoded frame plus 32 KiB metadata. One envelope is shared across receivers
of the same tier. Missing frames expire and require a keyframe; old fragments and
late acknowledgments cannot revive them. UDP blockage and legacy peers use the
previous bounded TLS path. Sustained shared-uplink/device evidence remains a
release gate.
WebRTC Audio Processing is a separate dependency supplying echo cancellation.

The optional `screen-audio` capability carries Opus packets independently of video backlog.
Voice peers and capable text-only viewers use the negotiated audio transport.
Screen-only viewers establish it on demand and retain it until their chat
connection closes; legacy peers retain TLS delivery. This grants no microphone
rights. The host sends only to capable
voice members or explicit audio-enabled viewers. Preview subscriptions alone
stay silent outside that voice channel. Legacy peers retain video and basic
voice/chat. Native macOS capture uses one latest image and at most 100 ms of
pending mono samples; it excludes this process's playback from captured audio.
The host does not relay its captured system audio back to its own device, which
already hears the original output.

## Releases

Keep branches scoped to one reviewed change and squash-merge them into `main`.
GitHub permits squash merges only and removes merged branches automatically.

Versions are UTC calendar dates in SemVer form: `YYYY.M.D`, without leading zeroes. `cmake/Version.cmake` is the canonical resolver. CI resolves a release version once and supplies it to every package job.

`verify.yml` is callable by other workflows and runs desktop builds, protocol/UI
tests, sanitizer checks and installed-package smoke tests. Run `release.yml` on
`main` to publish a regular desktop release. It resolves the UTC version once,
rejects an existing tag or release before starting builds, and reuses the full
verification matrix. Failed API requests also stop the preflight; they are not
treated as a free version number.

Publication requires matching signed macOS appcasts and all five desktop
archives, the standalone Windows test kit and corresponding third-party sources.
Checksums include the appcasts and generated Homebrew cask. Uploads go into a
draft first; only a complete upload becomes a public latest release. Existing
published packages are never replaced. If upload/publication fails, inspect the
private draft before deleting it and retrying; do not silently replace a public
version. The source revision stays fixed throughout the workflow. Native package
tests run before publication; physical device observations follow the initial
release under U139.

For a focused manual check, use
`gh workflow run verify.yml --ref <branch> -f platform=windows` (also `linux`,
`macos`, `sanitizers`, or `all`). Pushes to main, pull requests and the reusable release
call always run every platform. A focused run does not replace the release gate.
Feature pushes are checked by their pull request, avoiding a duplicate matrix
for the same branch update.

The separate `squadspeak-third-party-sources.tar.gz` release asset contains the
pinned dependency archives, Qt SDK sources and Abseil build patch. Build it with
`python3 cmake/build_sources.py --cache /path/to/source-cache --output /path/to/package.tar.gz`.
The cache and output must be separate. `--check` validates the manifest against
current build pins without Git metadata or network access. Packaging requires
Git, an origin URL and HTTPS access for uncached sources; a bad cached hash is an
error rather than an implicit replacement. Files are never extracted or run.
`source_archive_contract` tests the public command with temporary repositories,
cache failures and a local HTTPS server; it requires Git and OpenSSL. Dependency
upgrades must update `cmake/source_archives.json` and retain matching Qt sources.
The macOS/Windows gate also compares the actual linked Qt version with the manifest;
`--qt-version` makes that check available directly.
Linux distribution libraries remain system dependencies, not bundled binaries.

The release job generates `squadspeak.rb` from both verified macOS archives with
`cmake/BuildCask.cmake`, using their exact SHA-256 hashes and the shared version.
After the first stable release, add that cask to `YunaBraska/homebrew-tap/Casks`.
The tap's existing updater then follows stable releases through its repository
and asset markers. The cask installs the app and exposes `squadspeak` for CLI use.
Drafts and prereleases are ignored by the tap updater; Linux packages are not a
Homebrew formula.

macOS bundles currently use ad-hoc signatures. Developer ID signing, notarization and store accounts are separate distribution work. Store builds exclude external Supporter purchasing and activation. No signing credentials belong in this repository.

Direct macOS packages use pinned Sparkle 2.10.0 for updates. Set the repository
variable `SQUADSPEAK_UPDATE_PUBLIC_KEY` to its base64 Ed25519 public key and the
Actions secret `SQUADSPEAK_UPDATE_PRIVATE_KEY` to the matching base64 32-byte seed.
The private key is needed only by the release signing job; app binaries receive
the public key. Archive and architecture-specific feed signatures are required
before installation. An unconfigured preview starts no updater. Store builds
exclude Sparkle and headless never initializes it. Checks are automatic;
installation and restart require a click. Draft prereleases are deliberately
excluded from the stable `releases/latest` feeds.

Update signing is separate from Apple's Developer ID and notarization. The
release job fails on a mismatched pair or missing keys. Keep a private backup
of the signing seed; losing it prevents ordinary
updates to already distributed clients. See [Sparkle publishing](https://sparkle-project.org/documentation/publishing/).

## iPhone and iPad

Before Store publication, review the selected distribution terms against the project's GPL-3.0-only license and the exact bundled Qt/dependency licenses. Source availability and signing credentials alone do not establish compatibility. Qt's [open-source licensing FAQ](https://www.qt.io/faq/qt-open-source-licensing) explicitly makes this a per-Store check. No commercial Qt license, project-license exception or relicensing has been selected; any such change needs a separate decision.

Keep the channel/chat layout, themes, portraits and shared QML controls. Adapt the containing window to safe areas, touch targets, the software keyboard and iPad resizing. Mobile needs a normal app entrypoint instead of a tray; double-click and hover affordances need equivalent tap/long-press controls.

The current desktop build is not an iOS package. Qt 6.11 supports iOS/iPadOS 17 or later, but desktop FFmpeg deployment, helper-process image decoding, global keyboard hooks, screen capture and notification setup need mobile-specific integration. The [Qt platform table](https://doc.qt.io/qt-6.11/supported-platforms.html) describes the supported SDK/device configurations.

Local discovery needs the local-network privacy declaration and, for raw multicast on iOS, the relevant entitlement. Ask for permission in the foreground and provide direct-address entry when discovery is unavailable. Follow [Apple's local-network guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy).

An iPhone cannot be promised to run a silent hosting service indefinitely while suspended. Background audio/VoIP modes must serve their actual purpose; preventing idle display sleep while foregrounded does not grant background execution. Preserve hosted channel data and reconnect state across suspension. See [App Review Guidelines, 2.5.4](https://developer.apple.com/app-store/review/guidelines/).
