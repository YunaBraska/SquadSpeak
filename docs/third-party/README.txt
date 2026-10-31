SquadSpeak third-party notices

This directory is installed with release packages as THIRD_PARTY_NOTICES.
The license texts below are copied from the exact source revisions used by the
release build:

- OpenSSL 3.6.5: OPENSSL-LICENSE.txt
- Opus 1.6.1: OPUS-COPYING.txt
- libsamplerate 0.2.2: LIBSAMPLERATE-COPYING.txt
- RNNoise: RNNOISE-COPYING.txt
- QtKeychain 0.16.0: QTKEYCHAIN-COPYING.txt
- WebRTC Audio Processing 2.1: WEBRTC-APM-COPYING.txt, WEBRTC-LICENSE.txt,
  WEBRTC-PATENTS.txt and the WEBRTC third-party component texts.
  Local change: cmake/audio-processing-echo.patch exposes residual echo
  suppression sensitivity and bounds high-band gain during identified echo
  to prevent receiver amplification from sustaining acoustic feedback.
- Abseil 20240722.0 (statically linked into the private audio library):
  ABSEIL-LICENSE.txt
- libdatachannel 0.24.6: LIBDATACHANNEL-LICENSE.txt; pinned libjuice, libsrtp,
  plog and usrsctp submodules: LIBJUICE-LICENSE.txt, LIBSRTP-LICENSE.txt,
  PLOG-LICENSE.txt and USRSCTP-LICENSE.txt. Exact source hashes are in
  cmake/source_archives.json.
- Sparkle 2.10.0 (configured direct macOS updates only): SPARKLE-LICENSE.txt

Qt and its multimedia plugin are supplied by the selected Qt SDK. macOS
rebuilds the matching FFmpeg 7.1.5 runtime with the VideoToolbox lifetime
fixes recorded in QT-FFMPEG-ATTRIBUTION.txt; Windows uses the SDK runtime.
Linux Qt 6.10.2 builds rebuild the matching Multimedia and Qml libraries with
the portal and QML garbage-collector fixes recorded in that file; other
libraries remain system packages. The exact Qt Multimedia and Qt Declarative
6.10.2 source archives and checksums are in cmake/source_archives.json.
The package must use a Qt SDK whose SBOM and license terms are available to the
recipient. The macOS ARM64 Qt 6.11.3 kit used for local release checks identifies Qt
modules, bundled third-party components, and the FFmpeg 7.1.5 backend in its
SBOM. Complete SPDX records for the selected Qt modules are installed under
QT-SBOM/. The records are copied without lossy extraction, including their
copyright and license fields. The repository records describe the original
ARM64 SDK, not the rebuilt FFmpeg or the exact files in Intel or Linux packages.
The Windows package replaces them
with the same module records from its installed Windows SDK, preserving its
platform-specific components and copyrights. Its package check requires a
record for every bundled Qt DLL. The SBOMs retain their own
build metadata; the runtime component license set is explicitly listed in
QT-SBOM/SPDX-LICENSE-IDS.txt and excludes QT_TOOL records.

The QtSvg record is included in the same official Qt 6.11.3 SDK. The exact
XSVG component notice remains QT-XSVG-LICENSE.txt; its text matches
src/svg/LICENSE.XSVG.txt in the corresponding source kit. The macOS package
check requires a record for every bundled Qt framework.

The complete standard license and exception texts for that set are installed
under QT-SBOM/licenses/. The texts come from the SPDX License List at
https://spdx.org/licenses/ and are pinned by the manifest. The FFmpeg runtime
license expression and copyright are recorded in the qtmultimedia SBOM. This
Qt SDK's selected license terms govern the shipped build. The package does
not present alternative licensing modes as interchangeable permissions.

Linux packages use distribution-provided Qt, codec, FFmpeg, OpenSSL, and
samplerate libraries. They ship the private WebRTC audio library and the direct static RNNoise,
QtKeychain and Abseil notices above, while the installed distribution packages remain responsible for their
own notices. macOS packages bundle the corresponding runtime libraries and
carry the pinned source license texts plus the Qt SBOM records.

Release downloads include squadspeak-third-party-sources.tar.gz alongside the
application packages. Its manifest pins the original dependency source archives,
Qt 6.11.3 module sources and Abseil build patch by SHA256. Its README links the
exact application revision and platform build scripts, including the QtKeychain,
usrsctp and FFmpeg corrections. These sources are a separate download, not extra files loaded
by the app. The release workflow requires this asset before creating a draft.

The notice files are upstream texts and provenance records, not a statement
about the project's own license or a legal opinion about the complete SDK.

TeamSpeak interoperability uses ReSpeak tsclientlib at
ee3bc6f45a7137db7793ba5593a321df400d53e5 (MIT OR Apache-2.0).
RUST-NOTICES.txt contains dependency declarations and upstream notices for the
locked Cargo dependency graph, plus Rust standard-library attribution.
Cargo.lock owns registry checksums. build_sources.py includes their source
archives and the pinned ReSpeak archive in the release source package.

RustSec RUSTSEC-2026-0150 marks audiopus_sys 0.2.2 as unmaintained and identifies
a CMake 4 incompatibility in its bundled Opus build. TeamSpeak.cmake always
supplies OPUS_LIB_DIR, linking the same external Opus as the application instead
of building that bundled copy. The advisory is informational, not a reported
security vulnerability. The unmaintained binding remains a dependency risk.
Reference: https://rustsec.org/advisories/RUSTSEC-2026-0150.html

Mumble interoperability uses mumble-protocol-2x 0.6.0 with protobuf 3.7.2 for control messages and
voice framing. UDP encryption uses the corrected Mumble 1.5.939 implementation,
not the Rust crate cipher. MUMBLE-LICENSE.txt covers the vendored source and
upstream regression tests. src/teamspeak_bridge/mumble-crypt/UPSTREAM.md records
the limited portability changes. The application source archive includes them.
