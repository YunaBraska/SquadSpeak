# Release readiness

Updated: 2026-10-29. Behavior: [specification](specs/voice-chat.md).

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
| App Store content safety | Open | U178 excludes central moderation and blanket word censorship. Native channel bans and private blocks exist. Resolve O-31 with App Review, including reporting and external-provider safeguards, before claiming Store readiness. The [review draft](app-review.md) names current limits and submission prerequisites. Direct distribution remains separate |
| Markdown image rendering | Done | Escaped, formatted and entity-bearing image descriptions render one image per Markdown occurrence. Intentional repeats and code examples remain intact. The existing Qt parser validates each correction. Content tests and compact QML send/display tests cover the path without another Markdown dependency |
| Network and sustained-load acceptance | In progress | Local ten-minute mono/stereo soaks pass with 64 clients and eight speakers. A three-minute 128-client/eight-speaker stereo run passes. Longer 16-speaker runs and 64 simultaneous speakers still miss real-time targets. Intel CI checks controlled delivery with 32 clients/two speakers and admission beyond 64. Other release runners retain realtime 64-client/four-speaker gates. Shared-uplink and cross-device qualification remains. There is no hard 64-person admission cap |
| Real audio/video quality | In progress | Physical Shure USB reconnection and bidirectional Mac window sharing pass. Linux X11/Wayland and Windows Server 2022 native capture/isolation fixtures pass. Windows 10 execution, Bluetooth, playback-reference clock drift/double-talk, measured A/V synchronization and sustained 4K/30 remain unverified. The 9,000-frame 4K codec run decodes every frame and adapts quality, but averages about 22 fps, with sampled RSS 233 to 248 MiB. U134 allows explicitly labelled computer audio where app-only capture is unavailable. Strong breath overlapping a word onset remains deferred |
| Compact UI and event feedback | In progress | Public UI contracts cover keyboard/wheel input, compact/RTL layouts, text scaling, contrast, reduced motion, private blocks and host management. Mac microphone access, notification delivery and Focus suppression have device evidence. Visible notification banners remain unverified because active screen capture suppresses them. Simulator layouts do not establish physical mobile usability |
| Resource and security review | In progress | Complete sustained device/video CPU, RSS and handle measurements. Input limits, authenticated transport, encrypted atomic history, decoder process isolation and bounded media queues have tests. Isolation is not an OS sandbox in direct desktop builds. Private history visibility and cleanup use the committed database, including restart and storage-failure cases. The source package must contain matching dependency sources and licenses. No universal leak-free claim |
| Supporter sponsorship | In progress | GitHub sign-in, exact-tier eligibility, contributor exemption, encrypted shared cache, seven-day offline limit and desktop/headless integration have local HTTPS tests. Installed Mac sign-in, refresh and restart persistence pass. The tier and checkout link remain unpublished until a real buyer completes payment and activation. No buyer is available. See [O-21](specs/voice-chat.md). Free desktop distribution is independent |
| Direct desktop distribution | In progress | Stable `2026.10.19` remains public. The latest verified main snapshot is `2026.10.20-SNAPSHOT.40883699982` from `7db37fb`. Stable workflows require Developer ID signing, notarization, signed update metadata, matching sources and package checks. The generated cask is not yet in `YunaBraska/homebrew-tap`. A stable release containing current corrections and Windows publisher signing remain pending. Current authorization is snapshot-only |
| Mobile and Store delivery | In progress | iPhone/iPad Simulator and Android emulator builds exercise shared UI/core paths. Apple simulators cover portrait/landscape, cross-device chat, identity persistence and microphone lifecycle. They do not establish physical audio quality. Native background hosting, screen capture, consistent 44-point touch targets and physical permissions/input/output need implementation or device proof. Android decoder isolation/minimum version remain undecided. The iOS 26 Chinese-glyph failure also reproduces outside Qt. Mac App Store packaging and the [listing](app-store/metadata.md) are prepared, but signed sandbox behavior and App Review safety approval remain gates. Store builds are free-only |

Bluetooth audio devices are distinct from Bluetooth-only app networking, which is not implemented. See O-06/O-11 for platform scope. A feature cannot move to the completed table until its stated exit evidence exists; marking more rows `Done` is not a substitute for finishing them.

## TeamSpeak interoperability

U173 capture quality and keyboard suppression remain under investigation.
Automatic activation uses 240 ms pre-roll, as approved to preserve word beginnings,
and learns its threshold from the same processed signal used for admission.
One bounded capture pacer releases frames before protocol selection, preserving
speech through the smaller transport and playback queues without changing PCM.
The capture buffer and decoded queue also retain short, simultaneous callback
stalls. Unchanged discovery heartbeats no longer rebuild the channel UI.
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
quit produces an immediate disconnect in TeamSpeak. Native TS clients use stable UID-derived portraits and a separate TeamSpeak client icon. SquadSpeak clients exchange selected avatar IDs through TS metadata, including live profile updates in participants and cached messages. Both installed Mac copies show matching portraits.

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

The next provider is Mumble (U176), using the shared host/channel/chat interface.
Define authentication, voice/chat support and protocol differences before implementation.
Trillian is a navigation reference.

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
