# Reproducible audio corpus

40 repository PCM16/48 kHz mono excerpts provide 20 distinct English narrators,
18 music recordings and two environmental sounds. The files occupy about 36 MiB.
`manifest.json` owns source URLs, source/archive-member/fixture SHA-256 hashes,
crop positions, source sample rates, speaker IDs and the mixing grid. Every
source has an attribution notice in `licenses/`. The private microphone
recording is not included. This is evaluation data, not training data.

Three deterministic synthetic click trains (1, 5 and 20 ms pulses) additionally
exercise brief broadband disturbances. Their xorshift seed is 0x51a7; intervals
and decaying noise are generated in `corpus_tests.cpp`, with durations listed
in the manifest. They are not recordings of keyboards and do not validate a
keyboard classifier. No extra asset download or storage is needed.

The extended suite contains 9,790 deterministic cases:

- 9,660 mixtures: 20 voices x 23 interference sources x 7 SNRs x 3 input levels.
- 60 clean-speech controls and 69 interference-only controls at those levels.
- One silence control.

Each case passes through the real `VoiceMixer::encode -> receive -> render`
path at 0%, 50% and 100% suppression: 29,370 app variants. These are combinations
of 40 recordings and three synthesized signals, not thousands of independent
people or recordings.

## Sources and redistribution

Speech comes from [LibriSpeech SLR12](https://www.openslr.org/12/), CC BY 4.0.
The original three narrators are Garth Comira, Anders Lankford and Heather
Barnett, supplied through the [librosa example collection](https://librosa.org/doc/0.11.0/recordings.html).
Seventeen additional, distinct speakers come from the pinned `test-clean`
archive. Their published speaker IDs, reader names and utterance paths are
recorded in the manifest and individual notices. Each speech crop lasts eight
seconds. All speech sources are 16 kHz; upsampling does not restore missing
high frequencies or make these full-band microphone recordings.

| Fixture | Recording and attribution | Coverage | License |
| --- | --- | --- | --- |
| instrumental | Vibe Ace - Kevin MacLeod | Instrumental | CC BY 3.0 |
| song | Let's Go Fishin' - Karissa Hobbs | Song with vocals | CC BY 3.0 |
| rock-lead | Take the Lead - Kevin MacLeod | Rock | CC BY 4.0 |
| rock-rawk | Summon the Rawk - Kevin MacLeod | Rock | CC BY 4.0 |
| electro-sketch | Electro (Sketch) - Kevin MacLeod | Electronic | CC BY 4.0 |
| electro-cloud | Cloud Dancer - Kevin MacLeod | Electronic/EDM | CC BY 4.0 |
| techno-cut | Cut Trance - Kevin MacLeod | Techno/trance crossover | CC BY 4.0 |
| techno-chacha | Future Cha Cha - Kevin MacLeod | Techno/disco crossover | CC BY 4.0 |
| swing-hot | Hot Swing - Kevin MacLeod | Swing | CC BY 4.0 |
| swing-vermouth | Sweeter Vermouth - Kevin MacLeod | Swing | CC BY 4.0 |
| pop-poofy | Poofy Reel - Kevin MacLeod | Instrumental pop | CC BY 4.0 |
| pop-blown | Blown Away - No Percussion - Kevin MacLeod | Instrumental pop | CC BY 4.0 |
| funk-stick | I Got a Stick Feat James Gavins - Kevin MacLeod; vocals James Gavins; arrangement/instruments Bryan Teoh | Funk with vocals | CC BY 4.0 |
| jazz-apero | Apero Hour - Kevin MacLeod | Jazz | CC BY 4.0 |
| metal-mega | Megasong - Emma_MA | Metal/hard rock | CC0 |
| metal-frozen | White and Frozen World - The Oracle / Kaiser | Metal recording with vocals | CC0 |
| trance-charm | The Charm 68 - The Cynic Project; vocals Julie | Trance recording with vocals | CC0 |
| rap-intune | In Tune - Kellee Maize | Rap recording with vocals | CC BY 4.0 |
| robin | Bird Whistling, Robin, Single, 13 - InspectorJ | Nature; two-second loop | CC BY 3.0 |
| traffic | Audio sound of a busy traffic in Porthacourt - ElemePodcast | Traffic | CC BY 4.0 |

Categories follow publisher descriptions and refer to the source recordings;
they are not independently audited genre or vocal-activity annotations.
Music excerpts last 12 seconds. Crop positions and all transformations are
listed in each source notice. No source separation has been applied to them.

The newer Kevin MacLeod recordings use the publisher's
[current catalog license](https://incompetech.com/agent-section/). Kellee Maize's
[official license declaration](https://www.kelleemaize.com/exclusive-music)
grants CC BY 4.0 for her music. Her source file is a Commons mirror of the FMA
recording; its older embedded NC-ND tag differs from the current artist grant.
The notice preserves that distinction instead of treating the tag as the grant.
The three CC0 recordings come from their creators' OpenGameArt pages, linked in
the notices. Original downloads stay in the ignored build cache.

[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/),
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) and
[CC0](https://creativecommons.org/publicdomain/zero/1.0/) are separate from the
application's GPL-3.0-only license. Keep this file, the manifest and source
notices with redistributed fixtures or listening exports. No contributor
endorsement is implied.

## Run

The fast regression suite contains 74 cases: the original three
voices, four recorded and three synthetic interference sources, SNR -5/0/+10 dB, clean speech, pure
interference and silence. It requires no downloads, Python, FFmpeg, microphone
or audio device after building:

```sh
cmake --build build --target corpus_tests -j 6
ctest --test-dir build --output-on-failure -R '^audio_corpus$'
```

Run the complete matrix explicitly with Python 3 (standard library only):

```sh
python3 tests/audio/run.py --binary build/corpus_tests \
  --output build/audio-expanded --workers 4
```

An optimized CMake Release build reduces runtime. `--workers` bounds CPU and
memory use; each worker runs an independent partition. The default is at most
four workers. The matrix is intentionally not part of every default CTest run.
`--suite smoke` also exercises the batch runner with the 74 quick cases.

`report.json` contains every completed case and filter variant. `summary.json`
reports SI-SDR gain by music category, SNR and absolute input level, the worst
mixtures and clean-speech retention flags. Worker logs and individual reports
remain in a unique run subdirectory. The runner verifies the full set of case
IDs, source identities, levels, filter variants, manifest hash and process exit
codes. A missing, duplicate, partial or failed run exits nonzero. A directory
lock prevents simultaneous runs from overwriting one another; after a forced
process kill, verify that no worker is running before removing a stale lock.

`status: complete` means execution and coverage passed. It does not mean the
filter sounds good. Quality diagnostics are reported separately, including
negative improvements. The original smoke suite retains its hard clean-speech
guards. The new stress cases expose their retention failures as diagnostics;
their quality targets have not been calibrated or accepted by listening tests.

## Mixing and measurements

Every case lasts 12 seconds. Speech occupies seconds 2-10; interference spans
the whole case. The short bird fixture loops. RMS and comparisons use seconds
2-10, including natural speech pauses. The extended grid uses:

- SNR -15, -10, -5, 0, +5, +10 and +20 dB: negative means louder interference.
- Mixture peaks -30, -18 and -6 dBFS: quiet, medium and loud digital input.

First scale the two stems to the selected SNR. Then apply one common gain to
reach the selected mixture peak. This preserves SNR while changing actual
input level; the report checks both axes. Clean and interference-only controls
use the same peak levels. Digital levels do not describe sound pressure in a
room, a microphone distance or perceived loudness.

The smoke suite preserves its original recipe: speech RMS 0.04, interference
scaled to SNR, common headroom gain only when the mixture peak exceeds 0.8.
All seven original fixture hashes and all 44 original case IDs are unchanged.

The app path checks packet delivery, mute enforcement, finite bounded samples,
silence and noise-only amplification. Receiving-side automatic normalization is
off to keep its behavior separate from suppression. The smoke clean-speech
guards require -12 to +6 dB relative level and correlation above 0.6; they catch
large regressions, not subtle audible damage.
The synthetic smoke fixtures additionally require at least 10 dB of click-only
attenuation at full suppression and no precodec SI-SDR loss for their speech
mixtures. These are fixture regression limits, not a promise for real keyboards.

A separate measurement applies the pinned RNNoise library before Opus, with
its 20 ms delay compensated. Zero-mean SI-SDR compares the result to clean
speech. This waveform diagnostic is not perceived quality, intelligibility or
speaker identification, and its scale invariance can hide attenuation. Check
clean-speech level/correlation diagnostics as well. Applying SI-SDR across Opus
would confound codec phase changes with suppression. Noise-only reduction does
not prove equal reduction during simultaneous speech. The app codec/mixer path
is checked independently; the library measurement does not replace it.

Export one exact stress case for listening, including clean speech,
interference, mixture, app outputs and pre-codec outputs:

```sh
SQUAD_CORPUS_SUITE=extended SQUAD_CORPUS_EXPORT=1 \
  SQUAD_CORPUS_OUTPUT=build/audio-listening \
  build/corpus_tests mixtures:speech-ls61-metal-frozen-snr-10-peak-18
```

The echo corpus separately exercises licensed music and two simultaneous-speaker
pairs through the production echo canceller, including +/-100 ppm reference
drift. Export wanted voice, simulated microphone input and processed output:

```sh
SQUAD_CORPUS_EXPORT=1 SQUAD_CORPUS_OUTPUT=build/echo-listening \
  build/corpus_tests playbackEcho:speech1-speech-ls6930-30ms-100ppm
```

Exports preserve actual level differences. They are not loudness-matched.
Selecting a single Qt test row intentionally creates a partial report; use the
batch runner for verified whole-suite coverage.

This set does not prove German-language quality, learned main-voice isolation,
multiple desired speakers, real-room echo cancellation, clipping
recovery, hardware clock drift, real device latency or cross-platform performance. It
also is not a blind listening test. Several tracks by one composer and excerpts
from narrated books cannot represent all music and conversational voices.

## Rebuild the fixtures

Only regeneration needs Python 3, FFmpeg and optionally network access:

```sh
python3 tests/audio/prepare.py --cache build/corpus-sources
```

The importer verifies pinned originals and notices before decoding. LibriSpeech
uses one shared archive, its published MD5 plus pinned SHA-256, and separately
pinned FLAC members. The SHA-256 checks are enforced; the publisher MD5 is kept
as provenance. Members are read explicitly, never extracted by archive paths.
Existing verified cache files allow offline regeneration. Every conversion is
checked before any repository fixture is replaced. Corrupt or short sources
therefore do not publish a partially validated corpus.

A different decoder may change PCM bytes and fail verification. Review changes
before explicitly using `--refresh-fixture-hashes`. Ordinary tests never refresh
hashes. The manifest records the decoder used for the persisted PCM. Rebuilding
requires the roughly 330 MiB original speech archive and the complete original
music files in the ignored cache; only short WAV excerpts belong in the repo.

## Input protection and reconstruction

`corpus_tests automaticInput` exercises all 20 recorded voices with clean input
and deterministic 25/40/60 Hz rumble. It compares adaptation against the fixed
20 Hz baseline, checks voice preservation and at least 3 dB SI-SDR improvement
for every mixture, and writes measurements to `report.json` under `input_cases`.
The normal audio contract separately checks raw clipping, peak headroom, recovery,
sample rates, invalid input and per-device manual overrides. No microphone or
external service is used.

An optional offline comparison measures FFmpeg's autoregressive reconstruction:

```sh
python3 tests/audio/run.py --declipping-ffmpeg /path/to/ffmpeg --output build/declipping
```

It uses the three checksum-verified smoke voices, six clipping thresholds and
10/55 ms windows with overlap-save. `declipping.json` records improvements,
regressions and timeouts; a timeout is a failed candidate, not restored audio.
Temporary PCM files are removed automatically. The app does not depend on this
executable or libavfilter. These measurements do not establish live latency or
perceived quality. Algorithm reference: [FFmpeg adeclip](https://ffmpeg.org/ffmpeg-filters.html#adeclip).
