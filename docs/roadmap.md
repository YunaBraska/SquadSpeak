# Release readiness

Updated: 2026-10-25. Behavior: [specification](specs/voice-chat.md).

Under U139, the first regular desktop release follows successful automated
platform and package checks; physical device observations follow the public
download. The qualification work and known limitations below remain visible.
Mobile and Store delivery follow the agreed desktop-first order. `Done` means
the named contract has implementation and automated evidence on the tested
configuration, not certification of every platform or physical setup.

## Completed contracts

| Contract | Status | Production owner and public evidence |
| --- | --- | --- |
| Device identity, pinned TLS, encrypted bounded storage | Done | `TlsIdentity`, `ChatHistory`, `LocalChannel`; channel and headless contracts |
| Local discovery and bounded Add scan | Done | `LocalChannel`; real multi-process discovery checks and channel contracts |
| Requests, preapproval, passwords, kick/ban/unban | Done | `LocalChannel`; channel, headless and UI contracts |
| Saved channels, auto-join, one personal voice channel | Done | `LocalChannel`, `VoiceSession`, `Channels.qml`; channel/session/UI contracts |
| Ten owned channels, shared port, global media limits | Done | Root/child `LocalChannel` and shared radio budget; protocol/CLI/UI contracts |
| 24h/7d/30d expiry, replay, pagination, host image storage | Done | `ChatHistory`, image worker and `ChatPanel`; content/channel/UI contracts. |
| Long Supporter retention | Done | 90/180/360 days in Channel Info and headless; encrypted transactional SQLite history without an aggregate message cap, bounded paging and cleanup, legacy reader projection, 30-day fallback for new receipts after entitlement expiry. Storage, TLS, CLI, UI and packaged SQL-driver checks pass in the native matrix |
| Input/output selection and per-device profiles | Done | `AudioModel`, `AudioProfiles`; audio and controls contracts |
| Spectrum, manual/automatic input cuts and gain, clipping warning | Done | `AudioProcessor`, `SpectrumView`; independent per-microphone Auto flags, conservative rumble reduction and peak protection. 20 recorded voices across 80 clean/rumble cases, persistence and UI contracts. Raw ADC damage remains visible; offline reconstruction is evaluated separately |
| Remote control without local participation | Done | `LocalChannel`, `VoiceSession`; multi-client channel and UI contracts |
| Receiver loudness normalization and music attenuation | Done | `VoiceMixer`, output profiles; mixer/audio/UI contracts |
| Negotiated UDP audio and video with TLS fallback | Done | `MediaTransport`, `LocalChannel`; authenticated ICE/DTLS-SRTP voice and an unordered WebRTC DataChannel for screen frames. Real TLS/UDP impairment, fallback/recovery, maximum-frame, malformed-frame, 64-client and lifetime checks. Quality remains independent per receiver; physical network/load acceptance is below |
| Radio catalog, CRUD, reconnect and shared chat-only bot | Done | `RadioPlayer`; radio/channel/headless/UI contracts. Station availability is external and can change |
| Language selection and app-authored translations | Done | All 55 agreed languages are selectable: English plus 54 catalogs, with complete source coverage. Session/source/UI contracts and localized desktop/headless startup pass; CLI/JSON tokens stay stable. Technical checks do not imply native-speaker certification |
| Headless configuration and bot administration | Done | `headless.cpp`; subprocess contracts, bounded history and persisted password commands; no personal user or local audio |
| Additive protocol extensions and audio-state badges | Done | SS-165/166; authenticated future-message, malformed-message and portrait contracts |
| Bundled avatars and variable-frame playback | Done | Twenty member portraits plus System, exactly five physical state rows, three clips per state, shared quiet timing, separate status overlays and static grayscale offline portraits. Four sheets contain 112-128 drawn frames; seventeen retain their two-frame clips. Fresh QML renders verify all 480 dense frames at 48 px, and the catalog/scroll/state/crop contracts pass. Tone-responsive expressions are not provided; they were a feasibility request, not a claim of emotion recognition |
| Desktop builds, packages and automated contracts | Done | Reproducible workflows, installed-package probes and native contracts cover macOS ARM/Intel, Windows and Linux ARM/x64, including Windows application capture with a signed virtual audio endpoint and GUI/headless startup from extracted packages. CI retains the tested revision, reports and packages together. Publication requires every platform job to pass for the release revision. |

## Release work with explicit exit conditions

| Gate | Status | Exit condition and current gap |
| --- | --- | --- |
| Markdown image rendering | Done | Escaped, formatted and entity-bearing image descriptions render one image per Markdown occurrence. Intentional repeats and code examples remain intact. The existing Qt parser validates each correction. Content tests and compact QML send/display tests cover the path without another Markdown dependency |
| Network and sustained-load acceptance | In progress | U112's measured comparison is complete and the UDP paths are integrated. A ten-minute native mono soak with 64 clients and eight simultaneous senders passes the per-listener delivery and audible-playout thresholds. The ten-minute stereo variant also passes with the production adaptive receive buffer on the development Mac. It records packet loss and scheduling underruns, not lossless delivery or acoustic latency. Delayed-owner regressions preserve network arrival times and drain contiguous received audio without discarding a valid burst. Reconnect tests reuse UDP ports while the old native connection is still shutting down. Forged ICE requests cannot take over an authenticated endpoint. A three-minute run with 128 clients and eight simultaneous stereo senders also passes after fixing private-contact persistence recovery. Longer 16-sender runs and the combined single-process 64-sender stress still miss real-time targets. Shared-uplink and cross-device qualification remains. The 64-participant admission limit is removed. Large-roster and 70-source checks do not establish 70 simultaneous speakers or unlimited capacity on every host. Hosted Intel checks complete delivery with controlled pacing, 32 clients and two simultaneous speakers, alongside the unchanged 72-participant admission check. Other release runners retain realtime gates with 64 clients and four simultaneous speakers |
| Real audio/video quality | In progress | Prove playback-reference AEC with clock drift/double-talk, Bluetooth headsets, capture loss/recovery, measured A/V synchronization, sustained 4K/30 upper-tier load and overlap listening. A physical Shure MV7+ USB disconnect/reconnect restores input and preview without an app restart. Installed Mac copies now exchange window video in both directions, show live changes and stop when the selected source closes. Renewing the app-specific screen-recording entries replaced their obsolete test-certificate grants. A 9,000-frame 4K codec run decodes every frame and passes downward/upward tier transitions, but takes 406 seconds, about 22 frames/s rather than 30. Sampled process RSS stays between 233 and 248 MiB during the run. Linux X11 application isolation/restart passes five PulseAudio and three PipeWire-Pulse repetitions per architecture on ARM64/x64 at `028a401`. Windows Server 2022 process-loopback isolation and encrypted delivery pass with the signed CI endpoint at `24b3744`; native Windows 10 execution remains unverified. U134 permits labelled computer audio when app-only capture is unavailable; the native Wayland portal/audio fixture passes on Linux ARM/x64 and with sanitizers at `483a0a8`. Selected-output echo monitoring has macOS and Linux ARM/x64 mixed-process/speech proof, including repeated PulseAudio/PipeWire capture and sanitizers. Windows mixed-process monitoring and speech-reference echo reduction pass the same native fixture. Noise/transient filtering must retain wanted speech; known music/vocal weaknesses are not target-speaker isolation |
| Compact UI and event feedback | In progress | Shared controls, themes, About/logo, copyable info and compact/RTL scrolling have public UI evidence. Fresh avatar galleries also verify all dense frames at display size. U128 routes matching kick/ban events to remaining listeners and provides a separate System-announcement cue. Public TLS, PCM-resource and output/deafen checks pass; both installed Mac test copies have working microphone access and enabled notification permissions. macOS logs confirm notification delivery and Focus suppression. Visible banners remain unverified because active screen capture suppresses them |
| Resource and security review | In progress | Complete sustained device/video soak and CPU/RSS/handle measurements, dependency/license review and decoder-hardening review. The release workflow requires a pinned dependency-source asset; local failure-path tests and an independent check of the real CI archive pass. The green `fc4651a` release workflow attached the checked dependency-source archive to its private draft. Image decoding has process isolation and limits, not an OS sandbox. Repeated viewer replacement now retains one active decode and one newest pending frame; the real-TLS slow-codec regression and sanitizer suites pass. Encoding now also waits for receiver demand instead of wasting codec work while all viewers await their previous frame. Preserve encrypted atomic persistence and speech priority; no universal leak-free claim |
| Supporter sponsorship | In progress | GitHub sign-in, exact-tier yearly eligibility, encrypted cross-product cache, seven-day offline limit, token rotation, contributor exemption and desktop/headless integration are implemented and tested through local HTTPS. The old license-key/device-slot path is removed. The shared OAuth app is registered. Installed-app GitHub sign-in, maintainer recognition, refresh and restart persistence pass. Recipient-scoped queries avoid unrelated organization access restrictions. The selected tier stays unpublished and its in-app checkout link is hidden until a real buyer completes payment and activation. No buyer is currently available. GitHub sign-in and maintainer access remain available. See [O-21](specs/voice-chat.md). Free desktop publication is independent |
| Direct desktop distribution | In progress | The public `2026.10.19` release contains the desktop packages, signed update metadata, checksums, matching dependency sources and a generated Homebrew cask. The `01b4e5f` snapshot passes all seven verification jobs. The tap entry and a release containing the current corrections remain pending. Developer ID and notarization credentials are now configured, with their workflow integration under separate verification. Physical acceptance above, native Windows 10 execution and Windows publisher signing remain separate gates. UTC date version is shared and SemVer-shaped. U125 selects GPL-3.0-only with dependency/media notices retained |
| Mobile and Store delivery | In progress | Native iPhone/iPad Simulator and Android emulator builds run the shared UI and core contracts. The installed iPhone and iPad apps were inspected in native portrait and landscape layouts. iPhone-to-Mac chat, approval, history and iPad identity persistence work. The Simulator keychain identity now has a native creation/reload regression. Native microphone preview/restart and live capture with playback, encoded output and mute/restart pass on both iPhone and iPad simulators under iOS 26. These checks do not establish acoustic quality or physical-device behavior. Image sanitation uses the approved bounded in-process path on iOS; Android's decoder boundary and minimum version remain undecided. Android's five contract groups and packaged restart pass with independent process-crash checks. The Chinese glyph check fails in the available iOS 26 runtime, also reproduced without Qt on native arm64. Native background hosting, screen capture, permissions and physical input/output still need implementation or device proof. Store setup follows direct distribution and Store builds remain free-only. These results do not establish mobile parity |

Bluetooth audio devices are distinct from Bluetooth-only app networking, which is not implemented. See O-06/O-11 for platform scope. A feature cannot move to the completed table until its stated exit evidence exists; marking more rows `Done` is not a substitute for finishing them.

## TeamSpeak interoperability

U173 capture quality and keyboard suppression remain under investigation.
Automatic activation uses 240 ms pre-roll, as approved to preserve word beginnings,
and learns its threshold from the same processed signal used for admission.
One bounded capture pacer releases frames before protocol selection, preserving
speech through the smaller transport and playback queues without changing PCM.
PTT has no activation pre-roll. The extended corpus covers 498 activation cases
with twenty voices, brief speech, two keyboards, breathing controls and mono/stereo.
The longer pre-roll can admit more brief noise and adds up to 180 ms onset delay
compared with the previous setting. Native conversation acceptance remains separate.
Strong manual suppression can still attenuate brief speech details. Passing
transport checks alone does not establish capture quality. The TS bridge
boundary retains encoded packets unchanged, with fullband decoding and the
expected level in both Opus channel modes. Silent WOT comparisons now use the
same clean male/female recording through the installed A/B copies and original
client, with level-matched listening clips. Replaying the recorded packet and
playback timing preserves all 813 received frames exactly after correcting loss
concealment across drained speech pauses. Mono/stereo regressions also preserve
decoded samples through repeated 160 ms jitter. These checks do not establish
subjective equivalence or reproduce every microphone condition. Strong breathing
overlapping a word onset remains explicitly deferred. Receive-side loss
concealment stays bounded without rejecting the next valid packet after a long gap. The
missing-UID private-message case resolves the known roster identity and preserves
the conversation and reply target. The installed macOS app and native TeamSpeak
3 client exchange channel and private messages in both directions. Normal app
quit produces an immediate disconnect in TeamSpeak. Native TS clients use stable UID-derived portraits and a separate TS badge. SquadSpeak clients exchange selected avatar IDs through TS metadata, including live profile updates in participants and cached messages. Both installed Mac copies show matching portraits.

Direct TS3 is integrated through the pinned ReSpeak Rust library. The macOS
public-entrypoint test connected two separate identities to the owner's servers,
verified private text, decoded Opus audio at the receiver, blocked released
PTT and observed no sender loopback. Channel text and password handling pass the
local contracts. Live channel text, private messages and recovery after a test-only
UDP interruption pass, including preserved mute state and private history.
The installed macOS UI connects through the address field and renders TS3
channels. Incremental channel discovery preserves saved auto-join targets. Both
installed copies can change WOT channels themselves, and restart auto-join is
verified. The macOS package passes its dependency check.
The TS3 bridge and protocol contracts also pass on Windows and Linux. Live-server
voice and reconnect checks above were performed on macOS. Native Windows/Linux
device observations follow the desktop packages; mobile TS3 remains unqualified.

Not supported: TS3 file transfer, whisper lists, permission administration,
Speex/CELT channels, identity-file import, server avatars, pokes and server
management. The current library rejects servers requiring an identity security
level above 20. Only one TS3 server can be connected at a time. TS3 history is a
bounded in-session cache, without server history or offline delivery. SquadSpeak
images, radio hosting, screen sharing and receiver-specific quality negotiation
do not become TS3 protocol features.

## Explicitly after the first release

| Work | Status | Scope owner |
| --- | --- | --- |
| Enrolled main-speaker isolation | Planned | U124, O-03; select a usable licensed real-time model before offering local enrollment/download |
| Individual-window sharing on Wayland | Planned | U135; first release shares the whole screen with optional computer audio. Window selection needs a portal-backed implementation and GNOME/KDE window-capture evidence |
| User-supplied avatars | Planned | U121; format, transfer, storage and import contract remain deferred |
| Voice changer and optional speech clarity processing | Planned | U126, SS-167/168, O-24; local, simple controls, audible bypass; no guaranteed studio-quality or voice-identity conversion |
| Small multiplayer Channel games | Planned | U129, SS-169, O-26; choose the first game after release. Multiplayer Snake and ICQ-era games are inspiration; no game engine or exact catalog is selected |
| Host-created chat polls | Planned | U170, SS-182, O-29; chat "+", single/multiple choice, editable options, aggregate results and closing. Reuse chat infrastructure. Resolve the remaining lifecycle rules before implementation |

## Next implementation order

Continue U173 with capture-stage and codec comparisons and sustained resource
qualification. New media admission pauses during shared audio backlog or slow screen encoding. Large
member lists use bounded, acknowledged transfers without interrupting chat. On-demand channel search filters host, channel, address and private
contacts without changing voice participation or drafts. Unchanged native/TS
presence and roster payloads are suppressed per connection. Reconnect sends the current state again. Native TS text
interoperability and normal shutdown are checked. Audio settings preserve live
conversation, with exclusive Voice activation/PTT controls and a combined
threshold meter. Unread marks follow the foreground chat position, and opening
a chat does not imply voice participation.

1. Finish automated sustained load, shared-uplink impairment and measured A/V timing/resource checks. Physical hearing, device and Apple notification observations use the [short prepared handoff](development.md#device-checks-with-limited-human-time), retaining scoped prior results under U138. Do not wait for user availability to run technical tests. Measure conversation output, not only packet delivery.
2. Close direct distribution gates, then mobile/Store delivery; merchant approval, signing accounts and device access remain separate from implementation work.

Keep one owner and execution path per capability. Do not split `LocalChannel` merely to reduce lines; an extraction must remove existing responsibilities. Translation catalogs and meaningful negative tests are data/evidence, not implementation bloat.
