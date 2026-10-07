# Release readiness

Review date: 2026-10-07. Behavior: [specification](specs/voice-chat.md). Measurements and test history: [verification](verification.md).

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
| 24h/7d/30d expiry, replay, pagination, host image storage | Done | `ChatHistory`, image worker and `ChatPanel`; content/channel/UI contracts. Markdown rendering has a remaining edge case below |
| Long Supporter retention | Done | 90/180/360 days in Channel Info and headless; encrypted transactional SQLite history without an aggregate message cap, bounded paging and cleanup, legacy reader projection, 30-day fallback for new receipts after entitlement expiry. Storage, TLS, CLI, UI and packaged SQL-driver checks pass in the native matrix |
| Input/output selection and per-device profiles | Done | `AudioModel`, `AudioProfiles`; audio and controls contracts |
| Spectrum, manual/automatic input cuts and gain, clipping warning | Done | `AudioProcessor`, `SpectrumView`; independent per-microphone Auto flags, conservative rumble reduction and peak protection. 20 recorded voices across 80 clean/rumble cases, persistence and UI contracts. Raw ADC damage remains visible; offline reconstruction is evaluated separately |
| Remote control without local participation | Done | `LocalChannel`, `VoiceSession`; multi-client channel and UI contracts |
| Receiver loudness normalization and music attenuation | Done | `VoiceMixer`, output profiles; mixer/audio/UI contracts |
| Negotiated UDP audio and video with TLS fallback | Done | `MediaTransport`, `LocalChannel`; authenticated ICE/DTLS-SRTP voice and an unordered WebRTC DataChannel for screen frames. Real TLS/UDP impairment, fallback/recovery, maximum-frame, malformed-frame, 64-client and lifetime checks. Quality remains independent per receiver; physical network/load acceptance is below |
| Radio catalog, CRUD, reconnect and shared chat-only bot | Done | `RadioPlayer`; radio/channel/headless/UI contracts. Station availability is external and can change |
| Language selection and app-authored translations | Done | All 55 agreed languages are selectable: English plus 54 catalogs, with 799 extracted sources each. Session/source/UI contracts and localized desktop/headless startup pass; CLI/JSON tokens stay stable. Technical checks do not imply native-speaker certification |
| Headless configuration and bot administration | Done | `headless.cpp`; subprocess contracts, bounded history and persisted password commands; no personal user or local audio |
| Additive protocol extensions and audio-state badges | Done | SS-165/166; authenticated future-message, malformed-message and portrait contracts |
| Bundled avatars and variable-frame playback | Done | Twenty member portraits plus System, exactly five physical state rows, three clips per state, shared quiet timing, separate status overlays and static grayscale offline portraits. Four sheets contain 112-128 drawn frames; seventeen retain their two-frame clips. Fresh QML renders verify all 480 dense frames at 48 px, and the catalog/scroll/state/crop contracts pass. Tone-responsive expressions are not provided; they were a feasibility request, not a claim of emotion recognition |
| Desktop builds, packages and automated contracts | Done | Reproducible workflows, installed-package probes and native contracts exist for macOS ARM/Intel, Windows and Linux ARM/x64. All seven jobs pass at `08abcf3`, including native Windows application capture with a signed virtual audio endpoint and GUI/headless startup from extracted packages. An earlier Windows startup timeout did not recur; its cause remains unestablished. Exact runs and package evidence are in the verification record |

## Release work with explicit exit conditions

| Gate | Status | Exit condition and current gap |
| --- | --- | --- |
| Markdown image rendering | Known limitation | Ordinary formatting, compact tables/lists and lossless image-reference replacement pass content/UI checks. Qt 6.11.3 can split an escaped image alt label into multiple image objects. Plain labels work. Correct that rendering without a second Markdown parser, collapsing intentionally repeated images or changing unrelated text; see the Markdown review in the verification record |
| Network and sustained-load acceptance | In progress | U112's measured comparison is complete and the UDP paths are integrated. A ten-minute 64-client/eight-sender native soak delivers 15,120,000 packets with audible playback, zero reported gaps and no late sampled RSS growth. Intel at `1db25a6` passes the short 64-client realtime target (100 callbacks over 2029 ms); an earlier timing failure remains in the evidence history. Longer 16-sender runs and the combined single-process 64-sender stress miss real-time targets. Extend automated impairment and lifecycle evidence to sustained shared-uplink and cross-device runs, measuring actual playout, loss/jitter/bandwidth and CPU/RSS. Preserve independent receiver quality and voice priority; packet arrival is not acoustic latency |
| Real audio/video quality | In progress | Prove playback-reference AEC with clock drift/double-talk, device reconnect, Bluetooth headsets, capture loss/recovery, measured A/V synchronization, sustained 4K/30 upper-tier load and overlap listening. Linux X11 application isolation/restart passes five PulseAudio and three PipeWire-Pulse repetitions per architecture on ARM64/x64 at `028a401`. Windows Server 2022 process-loopback isolation and encrypted delivery pass with the signed CI endpoint at `24b3744`; native Windows 10 execution remains unverified. U134 permits labelled computer audio when app-only capture is unavailable; the native Wayland portal/audio fixture passes on Linux ARM/x64 and with sanitizers at `483a0a8`. Selected-output echo monitoring has macOS and Linux ARM/x64 mixed-process/speech proof, including repeated PulseAudio/PipeWire capture and sanitizers. Windows mixed-process monitoring and speech-reference echo reduction pass the same native fixture. Noise/transient filtering must retain wanted speech; known music/vocal weaknesses are not target-speaker isolation |
| Compact UI and event feedback | In progress | Shared controls, themes, About/logo, copyable info and compact/RTL scrolling have public UI evidence. Fresh avatar galleries also verify all dense frames at display size. U128 routes matching kick/ban events to remaining listeners and provides a separate System-announcement cue. Public TLS, PCM-resource and output/deafen checks pass; both installed Mac test copies have working microphone access and enabled notification permissions; visible notification/Focus delivery remains to be checked |
| Resource and security review | In progress | Complete sustained device/video soak and CPU/RSS/handle measurements, dependency/license review and decoder-hardening review. The release workflow requires a pinned dependency-source asset; local failure-path tests and an independent check of the real CI archive pass. The green `fc4651a` release workflow attached the checked dependency-source archive to its private draft. Image decoding has process isolation and limits, not an OS sandbox. Repeated viewer replacement now retains one active decode and one newest pending frame; the real-TLS slow-codec regression and sanitizer suites pass. Encoding now also waits for receiver demand instead of wasting codec work while all viewers await their previous frame. Preserve encrypted atomic persistence and speech priority; no universal leak-free claim |
| Supporter sales | Blocked | The maintainer reported Lemon Squeezy merchant rejection on 2026-10-07. Sales require renewed approval or an explicitly selected replacement provider, plus live IDs/checkout URL. Free desktop publication is independent. Test purchase, three slots, fourth-slot denial, reuse, deactivation and shared desktop/headless slot have evidence; live sales do not |
| Direct desktop distribution | In progress | The private `2026.10.6` preview at `08abcf3` contains all five desktop packages, verified production-key macOS archive/appcast signatures, checksums, matching dependency sources and the generated Homebrew cask. All seven verification jobs and the complete release workflow pass. Remaining: stable publication and its Homebrew tap entry; physical acceptance above and native Windows 10 execution. macOS packages remain ad-hoc signed; Developer ID/notarization and Windows publisher signing require the corresponding accounts. UTC date version is shared and SemVer-shaped. U125 selects GPL-3.0-only with dependency/media notices retained |
| Mobile and Store delivery | In progress | Native background hosting/capture/input and tray replacement need device proof; app accounts/signing/store setup follow the accepted direct-release-first order. Store builds remain free-only. Do not claim iOS/iPadOS/Android parity before it is implemented and verified |

Bluetooth audio devices are distinct from Bluetooth-only app networking, which is not implemented. See O-06/O-11 for platform scope. A feature cannot move to the completed table until its stated exit evidence exists; marking more rows `Done` is not a substitute for finishing them.

## Explicitly after the first release

| Work | Status | Scope owner |
| --- | --- | --- |
| Enrolled main-speaker isolation | Planned | U124, O-03; select a usable licensed real-time model before offering local enrollment/download |
| Individual-window sharing on Wayland | Planned | U135; first release shares the whole screen with optional computer audio. Window selection needs a portal-backed implementation and GNOME/KDE window-capture evidence |
| User-supplied avatars | Planned | U121; format, transfer, storage and import contract remain deferred |
| Voice changer and optional speech clarity processing | Planned | U126, SS-167/168, O-24; local, simple controls, audible bypass; no guaranteed studio-quality or voice-identity conversion |
| Small multiplayer Channel games | Planned | U129, SS-169, O-26; choose the first game after release. Multiplayer Snake and ICQ-era games are inspiration; no game engine or exact catalog is selected |

## Next implementation order

1. Finish automated sustained load, shared-uplink impairment and measured A/V timing/resource checks. Physical hearing, device and Apple notification observations use the [short prepared handoff](development.md#device-checks-with-limited-human-time), retaining scoped prior results under U138. Do not wait for user availability to run technical tests. Measure conversation output, not only packet delivery.
2. Close direct distribution gates, then mobile/Store delivery; merchant approval, signing accounts and device access remain separate from implementation work.

Keep one owner and execution path per capability. Do not split `LocalChannel` merely to reduce lines; an extraction must remove existing responsibilities. Translation catalogs and meaningful negative tests are data/evidence, not implementation bloat.
