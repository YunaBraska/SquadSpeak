# Verification evidence

Updated 2026-10-07. [Release gates](roadmap.md), [requirements](specs/voice-chat.md)
and [build instructions](development.md) are separate from this evidence record.
A passed fixture proves its tested configuration, not every device, room or
network. Superseded failed and cancelled runs have been removed from GitHub.
Relevant failure investigations are summarized below. The earlier
[Windows startup timeout](https://github.com/YunaBraska/SquadSpeak/actions/runs/37518572991)
remains available because its cause is still unknown.

History was consolidated at `3ede20d` with the same file tree as `5ce7455`.
Earlier revision identifiers below refer to the original CI checkouts.

## Revisions and platform runs

| Revision | Evidence | Result and scope |
| --- | --- | --- |
| `08abcf3` | [37524538004](https://github.com/YunaBraska/SquadSpeak/actions/runs/37524538004) | All seven verification jobs pass. Linux ARM/x64: 31 groups in 1326.81/1118.14 seconds; Store: 30 in 1072.65; sanitizers: 31 in 1398.44, with the required failing leak canary. Native PulseAudio/PipeWire/Wayland and extracted-package checks pass. macOS ARM/Intel: 29 regular groups in 760.13/752.03 seconds plus both network groups in 983.90/876.92; both archives pass 111 checks and native codec leak probes, and Intel passes seven GuardMalloc rows. Windows: 30 groups in 1699.36 seconds, native capture, signed runtime-installer validation and extracted GUI/headless startup. Both macOS archives and appcasts pass production-key signing and verification. |
| `17dbfdd` | [37577197475](https://github.com/YunaBraska/SquadSpeak/actions/runs/37577197475) | Six jobs pass: Linux ARM/x64 each pass 31 groups in 1107.79/1067.69 seconds; Store passes 30 in 890.87; sanitizers pass 31 in 1242.93, with the required failing leak canary. Native capture, network changes and runtime-only package checks pass. macOS ARM/Intel pass 29 regular groups in 436.92/436.53 seconds and both network groups in 958.36/868.29. Both packages pass 111 binary checks and native codec leak probes; Intel passes seven GuardMalloc rows. Windows stops in the new cache-test assertion before building the app; the workflow correction is verified separately. |
| `999953e` | [37578635369](https://github.com/YunaBraska/SquadSpeak/actions/runs/37578635369) | Corrected Windows job passes all 30 groups in 1452.31 seconds, including five native capture rows and desktop startup. Extracted GUI/headless startup and runtime-installer signature checks pass. Dependency reuse, incompatible stamps and missing headers are verified; the prefix is saved to the GitHub cache. Only the Windows workflow differs from `17dbfdd`; application code and other platform jobs are identical. |

The complete `08abcf3` release workflow also succeeds. Its private
[2026.10.6 preview draft](https://github.com/YunaBraska/SquadSpeak/releases/tag/untagged-ba0faf2fc7d04fade94a)
contains five desktop archives, two signed macOS appcasts, `SHA256SUMS`, matching
third-party sources and the generated Homebrew cask. Ed25519 update signatures
are verified with the configured production key; they do not replace macOS
Developer ID/notarization or Windows publisher signing. No stable publication
or Homebrew tap update has been performed.

The public-release matrix at `dda4035` ([37603176911](https://github.com/YunaBraska/SquadSpeak/actions/runs/37603176911))
passes both macOS architectures, Linux ARM64, Store and sanitizers. Windows
passes all 31 CTest groups, but the seeded device kit exposes a channel-profile
migration failure: an open read handle prevents atomic replacement on Windows.
The constructor now closes it before migration; the existing public migration
test also covers a populated permission store and preserves its approvals,
request policy and port. Both data rows and the host-policy regression pass
locally. The Windows rerun at `281911d`
([37611022410](https://github.com/YunaBraska/SquadSpeak/actions/runs/37611022410))
passes all 31 groups in 1521.78 seconds, extracted GUI/headless startup and both
seeded device-test profiles through `Start.cmd --smoke-test`. Both macOS jobs
also pass; the Linux jobs in that run stop in module build configuration.
The corrected Linux build at `da873d7`
([37612824232](https://github.com/YunaBraska/SquadSpeak/actions/runs/37612824232))
passes 33 groups on ARM64/x64 in 1114.20/1080.43 seconds and 32 Store groups in
915.19 seconds, including discovery and runtime-only archive checks.

The same matrix exposes a Linux x64 QML crash. Native debugger reproduction
([37607538557](https://github.com/YunaBraska/SquadSpeak/actions/runs/37607538557))
locates the crash in `QV4::MemoryManager::collectFromJSStack` while
`qsTr` allocates a string. Qt's upstream correction
`cdbacb7ba78779fc1eecc05afae3a3a874623e6e` ensures the first GC transition runs
even when its deadline has expired. The matching Qt 6.10.2 Qml module is rebuilt
with that correction. It does not yet resolve the observed failure: the
[repeated native run](https://github.com/YunaBraska/SquadSpeak/actions/runs/37612345123)
passes all 252 UI rows, then crashes during a focused repetition in the same
collection path. The loaded library is verified, but the invalid pointer still
requires symbolized diagnosis. Repeated UI, sanitizer and installed-library
checks remain the release gate. Garbage collection and the failing tests remain
enabled. A second upstream correction, `f2e838e86cefd91577345e093edbbc0bbd1fd6cd`,
fixes allocator free-list corruption during sweep. The isolated release-mode
[comparison](https://github.com/YunaBraska/SquadSpeak/actions/runs/37616013154)
passes 20 focused repetitions with and without it, so this alone does not prove
the reported crash resolved. The full preceding UI sequence is being restored
for the comparison. Linux CI retains that order and repeats the focused cases
20 times, stopping at the first failure.

CI retains CTest XML, detailed logs, rendered UI evidence and tested packages.
A green run at an older revision does not cover newly added platform code.
Native development builds use local Homebrew dependencies. Release packages
separately verify the macOS 13 deployment baseline. Current capture and codec
results are recorded below.

The 2026-10-07 public release preparation passes all seven affected local CTest
groups in 273.83 seconds: chat content, channel controls, translation sources,
device handoff, release metadata, appcasts and publication. The publication
suite was then extended to four passing cases covering an unused version,
existing release/tag, API failure, non-main dispatch, incomplete asset sets and
failed uploads. These execute the workflow's actual shell steps with only the
GitHub API boundary substituted. They do not count as a native platform run.
The first public regular release is authorized before physical checks under
U139; known Markdown and hardware/load limitations remain documented.

The 2026-10-07 chat regression first reproduced mouse-wheel events failing to
move loaded messages and code lacking a distinct background. The repair keeps
native ListView scrolling and the existing Markdown parser. Local verification
passes `chat_content_contract` (20 rows), `channel_controls` (244 rows) and
`translation_sources` in 268.92 seconds. After the final quote-contrast adjustment,
the focused UI run passes seven rows covering both wheel directions, wheel input
over message text, edge pagination, light/dark appearance at 360/560 pixels,
long-code wrapping and exact clipboard indentation. All four rendered Markdown
views were inspected. This is local Qt/macOS evidence, not a new platform matrix.

The subsequent Markdown review reproduced loss of nested-list indentation and
table structure when embedding an image: serializing the entire message through
Qt's Markdown writer changed unrelated syntax. Image embedding now replaces only
the image syntax and verifies that the parsed document otherwise stays identical.
The content contract passes 44 rows in 21.97 seconds; 18 focused UI rows pass in
13.28 seconds. Coverage includes headings, numbered/nested/task lists, tables,
quotes, reference links, code URL exclusions, unsafe links, image references,
copying and compact light/dark layouts. A 300-image-syntax code example remains
unchanged; the 26 focused content rows complete in 537 ms locally. No new
dependency or platform-matrix result is claimed.

One separate Qt 6.11.3 rendering issue remains: an escaped character in image alt
text, such as `![a \] label](attachment:...)`, can produce several image objects
instead of one. The image-reference repair preserves source text but does not
correct this parser behavior. Plain alt text and balanced nested brackets pass;
escaped-alt rendering still needs a regression and correction before claiming
complete image Markdown coverage.

The device handoff added on 2026-10-07 reuses these reports without treating
them as physical acceptance. `acceptance_handoff` passes its seven public-command
cases locally through CTest in 1.23 seconds: missing/failing/skipped evidence,
partial receipts, scoped reuse, source deletion/addition, OS/device/runtime
changes, malformed and future-dated receipts, atomic preservation on input
failure, Windows-specific steps and inert embedded script text. The same cases
also pass with Python encoding warnings treated as errors. Report JavaScript
parses successfully; browser rendering and the download interaction have not
been verified in a browser. Generated Mac
and Windows instruction previews contain no completed human observations.
The new test is registered in the existing cross-platform CTest suite; no new
native matrix or physical-device pass is claimed for this tooling-only change.

The standalone Windows handoff now adds a portable `Start.cmd`, two isolated
muted/deafened profiles and German connection instructions to a separate CI
artifact. The launcher uses `--settings` to make both otherwise tray-only apps
visible. The extended `acceptance_handoff` passes nine public-command cases on
macOS in 1.43 seconds, including empty device descriptions to complete on the
PC, invalid packages and refusal to overwrite existing profiles. Encoding-warning
checks also pass. YAML parsing succeeds; local actionlint did not complete and
was stopped. The Windows job is configured to smoke-test both profiles through
the packaged executable; this now passes in `37611022410`. That proves seeded
packaged startup, not physical audio or a complete two-app UI flow.
Browser interaction for the changed HTML remains unverified.

The `17dbfdd` channel refactor passes all 31 local CTest groups with
`--parallel 2` in 1121.47 seconds, with no failures or skipped groups. Four
focused discovery cases also pass, including repeated destruction during an
unfinished TLS scan. The source-translation check preserves the existing
catalog context. Load/video tests run alone and desktop fixtures share a lock.

The successful Windows dependency preparation takes 13 minutes 37 seconds; its
immediate repetition reuses the prefix in less than one second. That measures
dependency preparation within one job, not an entire app build or cache download.
The initial Windows cache-test assertion misused PowerShell positional arguments.
`999953e` names the path and pattern explicitly; application code is unchanged.

The Intel timing failure occurred with per-peer media debug logging enabled.
Local ARM comparisons with and without this logging both pass: 100 callbacks
over 2002/2001 ms, 25,200 received packets, no reported gaps and 96 audible
frames at each listener. This does not establish the cause of the Intel failure.
Release timing runs now use normal application logging; packet-delivery and
realtime playout thresholds are unchanged. Intel passes at `1db25a6`: 100 callbacks
over 2029 ms, 25,200 received packets, no reported gaps and 95/96 audible frames
at the two measured listeners. This does not isolate debug logging as the cause
of the earlier failure or replace sustained cross-device acceptance.

A deliberate 400 ms event-loop stall reproduces the old UDP-active assertion
failure: it expires the 300 ms liveness probe without resetting the connection.
The corrected test passes after a fresh acknowledgement and explicitly rejects
new signaling. It retains the original UDP recovery and reliable-audio checks;
no production timeout or transport behavior changed. The corrected contract
also passes hosted Linux ARM64/x64 and macOS ARM64/Intel verification.

The separate corrupted-packet fixture could likewise expire its UDP probe
before sending. A controlled 400 ms stall reproduces delivery over the valid
TLS path (`reliableAudio` count one), bypassing the deliberately corrupt UDP
relay. At `39cc683` the fixture awaits fresh UDP acknowledgements and asserts
both zero reliable deliveries and one relay packet before testing rejection.
All 18 local transport rows pass in 46.10 seconds. This test-only correction
also passes hosted macOS ARM at `dbf9e24`; other current hosted
platform results are recorded above. Transport and timeout behavior is unchanged.

The `645edc5` ARM package downloaded from CI also passes all 111 bundle checks
on the local Mac: the macOS 13 binary baseline, signatures, packaged dependencies,
GUI smoke, image worker and two headless starts. A separate 182-second headless
run answers status requests every five seconds with no personal user or clients.
RSS changes from 58,672 to 58,544 KiB; process CPU time advances by 0.18 seconds
between the first and last samples. This is an idle server measurement, not a
media load test.

## Selected output as an echo reference

The monitor introduced at `1f4ab1b` uses CoreAudio taps (macOS 14.2+),
WASAPI endpoint loopback and PulseAudio/PipeWire-Pulse monitors. It includes
other processes on the selected device, never records or transmits the reference,
retains at most 100 ms of mono PCM and joins its native worker on stop. Existing
WebRTC AEC3 processes the reference. Device changes and discontinuities reset AEC.
App-only playback remains the reference when system monitoring is unavailable,
including macOS 13; input settings show the active scope.

| Check | Evidence and limit |
| --- | --- |
| Native output monitoring | macOS passes three capture cases in 17.87 seconds: missing-device/restart handling, two playback processes in one device mix, stop/restart, external speech and encrypted selected-window sharing. |
| Speech echo reduction | A separate Qt process plays the licensed eight-second `speech1.wav`. The actual captured device mix supplies AEC3; a simulated 80 ms, 0.4-gain microphone reflection leaves 0.00001052 of its energy after convergence (about 49.8 dB reduction). This does not prove the physical microphone/speaker loop. |
| Existing audio processing | The audio contract passes in 2.21 seconds; corpus/audio controls/spectrum/channel controls pass four groups in 359.92 seconds. Drift and double-talk gates are retained. |
| UI and translations | Two focused UI cases pass; six source/archive/translation/startup/audio/video groups pass in 10.68 seconds. All 799 messages exist in 54 catalogs plus source English; all 55 Apple locale folders contain the local-capture explanation. |
| Installed A/B apps | Updated self-contained test bundles preserve both profiles and join the same channel. Input preview shows `Speaker audio`; disabling/re-enabling echo cancellation shuts down/restarts the reference. Closing preview removes the monitor worker. Both microphone and speaker outputs remain muted after testing. |

Repeated half-second pure-tone probes leave about 80 percent of reflection
energy; adding low broadband noise leaves about 59 percent. These are not
speech-quality passes. One early native startup produced no frames in ten seconds;
later native runs passed. Physical listening, startup stability, clock drift
and device changes still need their applicable acceptance evidence.

The PulseAudio event pump initially drained only about 284,000 samples in
12 seconds under PipeWire. Removing its fixed per-event sleep in favor of a
bounded event-driven poll restores the required continuous speech reference.
At `00d188b`, Linux ARM64/x64 each pass five PulseAudio repetitions
(94.18/93.41 seconds) and three PipeWire-Pulse repetitions (46.17/45.66 seconds).
Sanitizers pass the same repetitions in 95.95/47.06 seconds. The eight-second
continuous-reference requirement and echo-reduction threshold are unchanged.
The joined `std::thread` also compiles with Xcode 16.4.

The sanitizer run caught a 368-byte SCTP allocation when a peer aborted while
`sctp_copy_it_in` released the association lock. The pinned correction in
`cmake/Media.cmake` frees the unqueued message after reacquiring that lock; no
application retention or leak suppression was added. The regression closes
twenty connected data peers during sending. All 18 local transport rows pass
in 45.53 seconds; the native leak probe reports no leaks for the twenty-cycle
case. The complete Linux sanitizer run at `00d188b` also passes.

## macOS codec allocation and deployment

Native `leaks` found two lost 64-byte frame nodes while the public video
contract changed quality tiers. FFmpeg 7.1.5 did not free a VideoToolbox callback
node when the OS dropped a frame. The macOS build applies upstream
[04ff86d](https://github.com/FFmpeg/FFmpeg/commit/04ff86d0355a58c3d02dbf562ccb708942a29435)
to the same pinned version and deploys one shared runtime for the app and Qt.
The build search path prioritizes this runtime; deployment replaces SDK copies
before signing. Package checks reject duplicates and a missing patched version.

The exact same `leaks` command fails with the original Qt runtime (two 64-byte
`av_mallocz` roots) and passes with the patched runtime. Both runs pass all four
video rows. Only two named Apple startup allocations are excluded:
`-[LNProcessInstanceRegistryClient makeXPCConnection]` and
`AVControlCenterAudioPreferredMicrophoneModePreferenceKey`. These exclusions
leave the codec regression visible; the full reports are retained by CI.
This is a scoped native memory check, not a universal leak-free claim.

With the patched build, video/radio contracts pass in 208.59 seconds. Native
capture passes all three cases (five Qt Test rows) in 17.03 seconds, including
external speech echo residual energy of 0.000012408. The installed development
bundle passes all 111 binary, signature, dependency, headless and image-worker
checks against the local macOS 27 baseline. At `1db25a6`, both ARM/Intel bundles
and their extracted archives pass all 111 checks against the macOS 13 baseline.
That revision failed the independent Intel memory gate; the corrected
`dbf9e24` and `645edc5` runs pass on both architectures.

Intel's twelve failed hardware-encoder opens reveal another FFmpeg queue-node
leak: `vtenc_populate_extradata` allocates a node before session creation, but
only frees it on success. Twelve quality-tier opens leak twelve 48-byte nodes
while the software fallback passes the functional contract. The pinned patch
now frees a still-owned node on either result; successful frame submission
already sets that pointer to NULL. This uses the existing codec/runtime and
adds no suppression. The exact Intel native-memory regression passes at
`dbf9e24` and again at `645edc5`. The corrected ARM runtime passes the same four video
rows in 24.06 seconds under `leaks`, with only the two named Apple exclusions.
The source/archive/notice contracts pass; patch application accepts the original
and already-patched source, and rejects either changed cleanup anchor.

Three additional complete native capture runs pass all five Qt Test rows each
in 17.00, 16.86 and 16.29 seconds. Speech echo residual energy is between
0.00000980 and 0.00001174; none reproduces the earlier empty startup reference.

The native capture memory probe exposed a test dependency on the mouse position:
the captured pointer could cover the single sampled pixel. The receiver now
requires colored fixture content at six of nine distributed points within the
startup deadline. The same instrumented run passes all five rows in 17.27 seconds;
only the two Apple allocation stacks named above remain excluded. A failed test
can still produce a zero `leaks` exit status, so the macOS gate independently
requires the wrapped Qt Test success summary.

A longer codec run decodes all 1,800 4K frames in each of the two source formats,
then completes every quality change: 175.32 seconds total, maximum RSS 294.6 MiB
and peak physical footprint 239.7 MiB. Encoding plus decoding the 4K sequences
takes 76.84/77.80 seconds, so this same-process measurement is not a sustained
4K/30 acceptance pass. The two connected, muted app windows separately stay
within 0.17/0.05 MiB RSS variation in the final minute of a 150-second observation;
mean CPU is 6.1/5.1 percent of one core with the windows visible. This short sample
does not replace a long device soak.

The `.2` runtime also passes the local video and radio contracts together
(207.59 seconds), all 111 installed-bundle checks and the five native capture
rows under `leaks` (17.61 seconds). The latter reports only the two already-named
Apple exclusions; external-speech echo residual energy is 0.00001216.
Updated A/B test copies
retain their existing device identities and profiles: both rejoin Bea's channel,
show the same two members and exchange a new formatted message. Microphones and
speakers remain muted. The replaced bundles were removed after signature and
UI verification, recovering 456 MiB.

## Selected-application capture

At `d1467ae`, the isolated Wayland portal starts and capture crashes before any
frame reaches SquadSpeak. The native stack is Qt's PipeWire frame copy. The
installed wlr portal 0.8.1 sets buffer flags to zero; PipeWire 1.6 maps those
without read permission. Upstream wlr commit `4598313` identifies this exact
fault. The fixture now pins the checksum-verified 0.8.4 release containing that
fix. At `2922dbd`, both Linux architectures get past the former unreadable-buffer
crash but fail to advertise an active screen to the viewer, followed by a teardown
crash. At `72c48b2`, the first video, explicit audio, silence and stop checks
pass. Restart fails because Qt never closes the first portal session; the
backend rejects the reused request path. The session closes only at process
exit. The focused Qt backport also avoids a 181-byte introspection allocation
reported by the leak checker. Its repeated start/stop and early-cancellation
fixture passes at `483a0a8`; no crash or leak is suppressed.

At `257039d`, ARM capture successfully cancels the initial portal session, but
the next session cannot connect to PipeWire. The fixture's readiness query
activates a second portal with the root-owned runtime directory inherited by
the session bus. Its log records that wrong directory and permission failures.
The isolated runtime environment is now set before starting the bus; readiness
checks the portal's registered name without activating another process. At
`483a0a8`, the native Wayland fixture passes on ARM/x64 and with sanitizers.

At `645edc5`, the instrumented Wayland fixture passes all five rows, including
early cancellation, three capture restarts, explicit computer-audio delivery,
silence after disabling audio and Supporter revocation. LSan then finds a
separate 181-byte allocation in Qt's dynamic `QDBusInterface` construction.
The capture backport now calls the same portal methods directly, removing both
dynamic interface objects and explicitly disconnecting closed-session signals.
At `483a0a8`, the full sanitizer rerun passes all 29 groups and the native
Wayland fixture. The failing leak check remains in the earlier run artifacts.

`capture_tests` launches two independent applications with real audio outputs:
500 Hz and a four-times-stronger 1000 Hz tone. Each selected window reaches an
authenticated viewer. The receiver decodes production Opus, discards codec
startup, then compares both frequencies over 400 ms. Both selections must retain
the selected tone at more than ten times the unrelated tone's amplitude.
A single transient playback frame was insufficient for a stable measurement.

The same fixture verifies video content, audio enable/disable, source switching,
Supporter revocation, playback-stream recreation and selected-window closure. Temporary producers
have explicit stop signals and a final timeout. macOS uses a distinct signed
bundle launched through LaunchServices for the unrelated application; directly
spawning cloned executables can attribute their sound to the same application.
Five consecutive native macOS runs pass, followed by a final successful run
after tightening producer readiness and cleanup.

The fixture exposed a ScreenCaptureKit startup race: enabling audio before
`startCapture` completed could leave video working without any audio packets.
The original code fails at the second source. The fix retains the latest audio
choice until startup finishes; no fixed sleep was added to production or test.

Windows uses WASAPI process loopback, a cancellable worker and at most 100 ms
of mono PCM. A selected window includes only its process tree; full-screen
capture excludes SquadSpeak. A selected-app failure never substitutes computer
output. At `24b3744`, a hash-pinned, signed VB-CABLE endpoint supplies the
otherwise missing output on the disposable Windows Server 2022 runner.
All five native capture rows pass in 18.409 seconds: missing-device handling,
mixed-process output monitoring, selected-app isolation, encrypted delivery,
stop/restart and source closure. The speech fixture leaves 0.00000796046 of
the simulated reflection energy using the actual Windows playback reference.
This establishes the native API path, not physical room acoustics or Windows 10
execution. Kernel signing remains enabled; the driver is CI equipment and is
never bundled with SquadSpeak. The same run fails `desktop_smoke` after its
15-second startup deadline; recording and RHI startup checks pass. On a fresh
runner at `08abcf3`, all three pass in 4.74/1.62/1.65 seconds with unchanged
startup code and deadlines. GUI startup from the extracted ZIP also succeeds;
seven packaged headless rows pass in 2.092 seconds. The earlier timeout has not
recurred; its cause is not established, so it is not described as a fixed defect.
The official Scream 4.0 package was checked on the same Windows runner in
[37447377221](https://github.com/YunaBraska/SquadSpeak/actions/runs/37447377221).
All three catalogs fail Authenticode validation because of an expired
certificate without a timestamp. No driver was installed. The temporary
signature-only workflow was removed after preserving its result.

Linux uses local X11 window identity and individual PulseAudio sink-input
monitors. PID reuse is checked against process birth time. Missing window
identity previously rejected audio. U134 now permits a labelled computer-audio
choice; the updated fixture requires silence until explicitly enabled after
deleting the producer's PID property. Stream changes are reconciled through subscriptions; PCM queues
and monitored-stream counts are bounded. X11/PulseAudio compilation and isolation
pass on ARM64 and x64; the original PulseAudio implementation also passes
ASan/UBSan/LSan. The new PipeWire-Pulse fixture initially received no packets:
native streams supplied a client ID but no per-stream process ID. Capture now
resolves the process from the owning audio client for both backends, with
bounded enumeration and unchanged process-tree checks. Five PulseAudio and
three PipeWire-Pulse repetitions pass on each architecture at `028a401`,
including selected-app isolation, stream restart and missing-identity rejection.
The owning-client change also passes the sanitizer run at `00d188b`.
The Wayland ScreenCast portal
does not supply a selected window's owning application identity. Qt 6.11.3's
portal helper also requests only monitor video (`types = 1`); it cannot provide
the promised window picker. U134 permits computer audio where application-only
capture is unavailable. The current implementation offers that labelled scope
through the existing Pulse stream path, with audio initially off. Native Wayland
portal/video/audio proof passes at `483a0a8` on ARM/x64 and with sanitizers.
U135 explicitly accepts monitor-only Wayland selection for the first release;
individual-window selection follows later.

Reproduce the native macOS fixture after granting Screen Recording permission:

```sh
cmake --build build/native-release-sdk --target capture_tests --parallel 4
build/native-release-sdk/capture_tests.app/Contents/MacOS/capture_tests -v1
```

Windows/Linux register `capture_contract` in CTest. Linux CI supplies Xvfb,
Openbox and an isolated PulseAudio null sink. Store builds omit publishing;
receive-only video contracts remain enabled.

## Installed app and permission checks

Qt found the microphone status plugin but omitted its native request handler
because the bundle plist still contained unexpanded XML fragments. Expanding
those fragments before Qt's CMake finalizer fixes the missing request path.
The unchanged older archive fails the new package check; the corrected local
package passes all 111 binary checks plus startup and headless restart. The
check inspects the native request selector after stripping, not a removable
symbol-table entry.

Both installed test copies, SquadSpeak A and B, show live built-in microphone
spectra. A bundled speech sample played into that microphone makes each app
receive the other's speaking state. Receiving speakers were muted during that
test to avoid feedback. This proves capture/transmission, not listening quality
or acoustic latency. Both microphones and speaker outputs are muted after testing.

The 2026-10-06 echo report came from both copies sharing the built-in microphone
and loudspeakers while joined to the same channel. The receiving copy's sound
could re-enter the transmitting copy's microphone; that older build had no reference
for the other process's playback. The new device monitor includes that playback.
The original observation is not proof of a protocol return
to the sender or an AEC failure between separate computers. New test-copy
profiles now start with both microphone and speaker outputs muted. Keep receiver
speakers muted for microphone/transmission checks, or use headphones for
listening. Two copies on one Mac do not replace a two-device acoustic test.

Two isolated packaged profiles also pass nearby search, Add, Inspect, Approve,
Markdown delivery, voice join, Kick, rejoin, Block, Unblock and restored chat.
These checks originally used `81575a0`; they do not certify newer media code.
The pending-name regression now verifies that an expired discovery entry does
not replace a known channel name with "Channel". Real local chat delivery was
checked again on 2026-10-06. macOS shows notification sound/banner permission
for both test copies; visible notification delivery and Focus behavior are not
established by that permission alone.

Multicast still reports a send error on this development Mac. Bounded TCP
search finds the other local process. CI separately checks discovery across
network changes; neither result proves every router's multicast policy.

## Audio processing and resource measurements

| Check | Measured result | Limit of the claim |
| --- | --- | --- |
| 64 clients, four simultaneous senders, five minutes | 3,780,000 packets; both sampled listeners render 14,996 audible frames out of 15,004. Fixture CPU: 212.94 seconds over 315.72 wall seconds. Median RSS: 178,560 KiB in both early and late windows. | Host, clients and two mixers share one process. This is not host-only CPU or all-device capacity. |
| 16 simultaneous senders, requested 120 seconds | 6,048,000 packets; 5,977/5,980 audible frames out of 6,006. Actual streaming time: 147.08 seconds. | Functional delivery passes, real-time target does not. The release assertion now compares rendered frames with elapsed wall time. |
| 64 simultaneous senders | Two short runs miss the fixture's timer deadline; UDP send calls dominate the sampled profile. | Failure, not proof of sustained real-time capacity. |
| Idle packaged headless server, 91.93 seconds | 0.20 CPU seconds; RSS 61,152-61,344 KiB; no personal participants; clean exit. | Bounded idle observation, not a long active-call soak. |
| Automatic input correction, 20 recorded voices and 80 clean/rumble cases | Clean correlation 1.0; minimum noisy-case SI-SDR improvement 3.20 dB against a fixed 20 Hz cut. Three DSP paths process eight seconds in at most 97 ms. | Synthetic rumble mixtures, not every microphone or room. |
| Digital AEC with -200/0/+200 ppm drift and overlapping near-end tone | Wanted amplitude 93.7-95.4% of input; wanted energy 85.6-96.5% of overlap output; remaining echo energy 4.6-5.8% of input. | Tone fixtures do not replace spoken double-talk or real device clocks. |
| Duplex acoustic feedback simulation, three recorded voices, 20/80 ms reflection delay, PTT and voice activation | Input correction, Opus and automatic receiver volume included; the local run's post-speech playback energy is 28.91-34.21 dB lower with AEC during PTT. Voice activation reaches silence in all six cases. All 12 cases retain audible wanted speech; disabling AEC sustains the loop. | Simulated separate rooms with valid per-device playback references, not two applications sharing physical speakers. |

The real-time fixture now stops its playout clock when input stops, before
waiting for final network delivery. It still requires at least 95% actual
packet delivery and 90% real-time and audible playout. Final lost UDP packets
cannot report their own sequence gap; bounded drain replaces an impossible
complete-accounting assertion. A separate-host-thread experiment did not
establish a reliable load improvement and was discarded. The corrected four-sender run receives all 25,200 packets, renders 100 frames
in 2,005 ms and keeps 97 audible frames at both sampled listeners. The same
measurement still rejects 16 senders: 1,000 frames in 24,718 ms against 1,235
expected. That performance limitation remains open.

The audio corpus contains 40 licensed source excerpts with provenance and
hashes. The expanded run processes 8,521 mixtures and 25,563 filter variants;
fast CI retains a bounded regression set. Music with vocals and competing
speakers remain difficult. SI-SDR is not a substitute for intelligibility.

Spoken playback-reference cases improve SI-SDR by 0.50-5.36 dB without drift.
Three of eight original drift mixtures regress, worst -1.05 dB. Two additional
spoken pairs at -100/0/+100 ppm improve by 5.01/1.86 dB without drift, but only
-0.20 to +0.22 dB with drift. These measurements retain the weakness openly.
These older fixtures use SquadSpeak playback as their reference. The selected-output
monitor above additionally covers other applications on the same output device;
independent voices in the room are not a playback reference. Device changes, stale
references and partial output writes have separate contract coverage.

FFmpeg `adeclip` was evaluated offline against known clean references: 36 cases,
three voices, six clipping thresholds, 10/55 ms windows. Damaged-signal SI-SDR
changes range from -0.26 to +6.75 dB at 10 ms and -5.50 to +15.35 dB at 55 ms.
Three severe cases exceed 30 seconds for eight seconds of input. It was not
integrated into live capture: quality can regress and runtime is not bounded.
DeepFilterNet3 and GTCRN comparisons likewise do not establish target-speaker
isolation and are not application runtime dependencies.

Reproduction and corpus licensing: [audio fixtures](../tests/audio/README.md).
Extended public-entrypoint runs:

```sh
QTEST_FUNCTION_TIMEOUT=600000 SQUAD_SOAK_SECONDS=300 SQUAD_SOAK_SPEAKERS=4 \
  build/native-release-sdk/channel_tests sixtyFourClientsHaveOneConsistentRosterAndAudioFanout
SQUAD_VIDEO_SOAK_SECONDS=120 \
  build/native-release-sdk/video_tests roundTripAndQualityChanges:native-rgb
```

## Continuous audio delivery

A continuous-input regression reproduces timer starvation in `MediaTransport`: every
accepted frame restarted the five-millisecond playout timer. A stream arriving
before that deadline could postpone output indefinitely. The receiver now starts
that timer only when it is stopped or changes interval. The regression fails
before the change and passes after it; all 19 local transport rows pass in 45.26
seconds. Queue bounds, packet validation and loss handling are unchanged. This
is not a general fanout performance fix: the separate 64-client/16-sender
measurement still misses realtime playout (1,000 frames over 24,560 ms).

The release build passes a 60-second eight-sender run with 64 clients:
1,512,000 received packets, zero reported gaps, 3,000 output callbacks over
60,035 ms and 2,995 audible frames at each measured listener. Sampling a
16-sender run puts 1,668 of 2,934 main-thread samples in UDP `sendto`, through
the synchronous host relay. All clients share this process and local network
stack; this identifies the hot path, not a distributed host capacity limit.
The test logs when media starts so profiling can exclude identity generation.

A ten-minute repeat with `QTEST_FUNCTION_TIMEOUT=1200000`,
`SQUAD_SOAK_SECONDS=600` and `SQUAD_SOAK_SPEAKERS=8` passes:
15,120,000 received packets, zero reported gaps, 30,000 output callbacks over
600,451 ms and 29,995 audible frames at each measured listener. Median RSS
falls from 185,648 KiB at 60-120 seconds to 181,520 KiB at 540-600 seconds.
The measured active interval uses 694.32 CPU seconds over 590.64 wall seconds,
about 1.18 cores for the host and all 64 clients together. This does not measure
a standalone host or replace cross-device and physical listening tests.

## Video, memory ownership and backpressure

The 4K soak completes 3,600 frames and a down/up quality cycle. The 4K phase
takes 156.39 seconds, with mean H.264 encode/decode times of 37.66/5.69 ms.
Median RSS is 252,288 KiB at 30-60 seconds and 252,304 KiB at 90-150 seconds;
sampled peak is 283,520 KiB. This proves sustained processing with no observed
late RSS growth, not 4K/30 throughput on that machine. Quality must adapt to
actual encoding and receiver budgets.

GuardMalloc reproduced an FFmpeg packed-RGBA overwrite at widths not matching
its SIMD store alignment. A 360-pixel row could receive 368 pixels of stores.
The decoder now uses FFmpeg's 64-byte-aligned allocation, with QImage owning
its cleanup and no extra pixel copy. Edge-pixel/ownership cases at widths
18/360/642/638 pass across decoder destruction, resolution changes and Qt detach.
The old x64 decoder terminates with signal 11; the corrected decoder completes
20 frames. Intel CI repeats the cases with GuardMalloc.

CPU-backed RGB frames avoid Qt's unnecessary thread-local QRhi conversion;
transformed and other formats retain Qt conversion. Color, alpha, stride,
rotation, mirroring, bottom-up and YUV tests remain. The corresponding Linux
ASan/UBSan run passes 29 groups without a new suppression.

Video work retains one active decode and one newest pending frame. Encoding
waits for receiver demand. Public TLS tests cover slow decoders, viewer
replacement, malformed frames, independent quality adaptation and simultaneous
voice playout. The 64-viewer test uses real codec frames and two measured
decoders, not 64 simultaneous decoders or a physical shared uplink.

Headless stdout backpressure previously blocked the network event loop. Public
subprocess tests now verify partial writes, bounded pending replies, input EOF
and closed readers while an independent client sends chat. Windows tests cover
both Qt overlapped pipes and synchronous handles. All six Windows scenarios
pass at `0c41c55`; the superseded run has been removed.
Linux sanitizer verification includes an intentional leak canary; its expected
failure confirms that leak detection remains active.

## Storage, compatibility, UI and distribution

- Encrypted SQLite history tests cover more than 2,000 messages, paging,
  UTC expiry, restart, legacy migration, concurrent writers, retry conflicts,
  disk failure and ciphertext/index tampering. Image cleanup holds the writer
  lock across reference checks and deletion. TLS/CLI/UI tests cover every
  retention duration and old-client projection. Package probes reopen the same
  encrypted database with only installed runtime libraries.
- Protocol tests exercise unknown additive fields/messages, optional capabilities,
  malformed input and authenticated peer identity. Unknown features do not
  disable baseline voice/chat. A forged capability does not bypass admission.
- Twenty member portraits plus System use five physical state rows. Four dense
  sheets retain all 480 original frame crops; overlay badges and speaking frames
  are separate UI elements. Public QML tests check every dense frame at 48 px,
  grayscale offline state, quiet timing, bounded renderer lifetime and scrolling.
  The locked-portrait assertion reacquires the current PathView delegate and
  retains screenshots; Linux confirms the former stale-delegate failure is fixed.
  U136 adds 108 rendered geometry comparisons across the catalog, shapes and
  sizes. The original Retina crop and non-square stretching fail these checks.
  One Canvas renderer now preserves aspect and converts the full physical-pixel
  buffer to grayscale. Local macOS checks pass at 100/150/200/300 percent scale
  with software rendering, and at 200 percent with Metal. Controlled-clock
  tests verify irregular holds, varied movement, adjacent-frame continuity,
  synchronized member/chat views and independent timing for different members.
  `ctest --test-dir build/native-release-sdk -R '^(channel_controls|avatar_scale_.*|desktop_smoke)$' --output-on-failure`
  passes all four groups in 294.74 seconds. The native Metal run passes 111 rows.
  The full UI check also exposed a discovery-test race; it now awaits the existing
  coalesced list update. No discovery behavior or avatar artwork changed.
- All 55 languages are selectable. The 54 non-English catalogs cover 799 extracted
  sources and preserve CLI tokens; technical coverage is not native-speaker review.
- Sparkle replacement tests install and relaunch a disposable version-2 app from
  a signed local feed, and reject altered archives/feeds while retaining version 1.
  Both hosted Mac architectures pass that contract in run 37375875885. Production
  signing configuration alone does not prove the next release's generated feeds.
- Qt 6.11.3/FFmpeg 7.1.5 are pinned for macOS/Windows; Linux uses distro packages.
  Binary notices are checked against the actual SDK records. All 24 dependency
  source payloads match the archive manifest hashes. The private `fc4651a` release
  draft includes the checked dependency-source archive.
- Lemon Squeezy's real test checkout charges no money and issues a tax-inclusive
  12 EUR annual pass. The approved provider test activates three devices, reuses
  a saved receipt, rejects a fourth, frees/reuses a slot, then removes every test
  activation. Independent validation confirms 0/3 slots used. Live merchant
  approval and production product/checkout identifiers remain external requirements.

## Transport comparison and primary references

The comparison used the same TLS-authenticated topology for both paths:
source -> host -> two receivers. Six seconds of licensed speech became
300 Opus frames. One downlink had seeded 80 ms delay, 5% loss and 512 kbit/s.

| Path | Clean delivery p95 | Impaired delivery p95 / maximum | Impaired decoded frames |
| --- | --- | --- | --- |
| Existing TLS | 1.56 ms | 200.91 / 597.36 ms | 300/300 |
| Authenticated UDP candidate | 23.05 ms | 95.79 / 194.95 ms | 289/300 |

A subsequent mixer run renders 290 non-silent frames for impaired TLS and 289
for UDP; both healthy neighbors render all 300. Packet latency alone does not
measure perceived quality. The measured impaired-path advantage supported U112's
integration: production now negotiates DTLS-SRTP voice and unordered DataChannel
video through libdatachannel. It is not the complete libwebrtc video stack or its
congestion controller. TLS remains authenticated signaling/control and the bounded
media path when UDP cannot work. ICE-TCP did not provide a working replacement
in the pinned libjuice experiment. Midstream UDP/TLS recovery is tested separately.

Useful primary references:

- [PulseAudio per-stream monitoring](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/WritingVolumeControlUIs/)
- [Windows application loopback](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [Apple ScreenCaptureKit audio scope](https://developer.apple.com/videos/play/wwdc2022/10155/)
- [Wayland ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) and [WebRTC transports](https://www.rfc-editor.org/rfc/rfc8835.html)
- [WebRTC Audio Processing API](https://webrtc.googlesource.com/src/+/refs/heads/main/api/audio/audio_processing.h)
- [FFmpeg declipping](https://www.ffmpeg.org/ffmpeg-filters.html#adeclip)
- [Qt security advisories](https://wiki.qt.io/List_of_known_vulnerabilities_in_Qt_products)
- [Sparkle signing](https://sparkle-project.org/documentation/)
- [Lemon Squeezy test mode](https://docs.lemonsqueezy.com/help/getting-started/test-mode)
