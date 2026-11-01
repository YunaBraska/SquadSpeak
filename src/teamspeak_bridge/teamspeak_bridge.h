#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One caller owns each handle. Do not race close with another operation.
 * Commands/audio return 1 when queued, 0 when invalid, disconnected or full.
 * next copies one packet, returns 0 if empty, or -required_size without consuming
 * the packet. Packet 1 is UTF-8 JSON. Packet 2 is TeamSpeak client/sequence BE16,
 * codec U8, then raw Opus. Packet 3 is Mumble session BE32, sequence BE64, flags U8,
 * then raw Opus. Audio of length zero signals the end of a talk burst and may
 * use a null pointer. close joins the worker and releases every queued packet. */
int ss_ts_identity(uint8_t *out, size_t capacity);
void *ss_ts_open(const uint8_t *options_json, size_t length);
/* Mumble uses the same options/event/command/audio handle contract as ss_ts_open.
 * identityPem contains the persistent client certificate and private key. */
void *ss_mumble_open(const uint8_t *options_json, size_t length);
int ss_ts_command(void *handle, const uint8_t *command_json, size_t length);
/* TS3 allows 500 bytes including its 13-byte header and 3-byte audio header. */
enum { SS_TS_MAX_AUDIO = 484 };
/* Mumble's Opus payload limit for standard voice packets. */
enum { SS_MUMBLE_MAX_AUDIO = 1275 };
int ss_ts_audio(void *handle, const uint8_t *opus, size_t length);
int ss_ts_next(void *handle, uint8_t *out, size_t capacity);
void ss_ts_close(void *handle);

#ifdef __cplusplus
}
#endif
