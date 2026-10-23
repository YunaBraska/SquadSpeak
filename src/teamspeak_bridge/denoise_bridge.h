#pragma once

// Local evaluation only. Each handle belongs exclusively to one capture stream.
// Calls must not overlap. Audio is interleaved at 48 kHz, 960 frames per call.
// Input, output and SNR storage must be disjoint. Audio buffers cover all frames
// and channels. SNR receives the minimum estimate across both 10 ms halves of
// the most speech-like channel, in dB. It is assigned only on success.
// The returned signal has 1440 frames of delay. Null/false reports a failure.
extern "C" {
void* squad_denoise_create(int channels);
void squad_denoise_destroy(void* state);
bool squad_denoise_reset(void* state);
bool squad_denoise_process(void* state, const float* input, float* output, float* snr);
}
