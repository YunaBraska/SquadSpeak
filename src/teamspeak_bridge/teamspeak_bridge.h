#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One caller owns each handle. Do not race close with another operation.
 * Commands/audio return 1 when queued, 0 when invalid, disconnected or full.
 * next copies one packet, returns 0 if empty, or -required_size without consuming
 * the packet. Packet 1 is UTF-8 JSON. Packet 2 is client/sequence BE16, codec U8,
 * then raw Opus. close joins the worker and releases every queued packet. */
int ss_ts_identity(uint8_t *out, size_t capacity);
void *ss_ts_open(const uint8_t *options_json, size_t length);
int ss_ts_command(void *handle, const uint8_t *command_json, size_t length);
int ss_ts_audio(void *handle, const uint8_t *opus, size_t length);
int ss_ts_next(void *handle, uint8_t *out, size_t capacity);
void ss_ts_close(void *handle);

#ifdef __cplusplus
}
#endif
