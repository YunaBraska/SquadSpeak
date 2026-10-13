#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SquadLanguageCallback)(void* context, int64_t request, const char* eventJson);

// Calls and callbacks run on the main thread. Input strings/buffers are borrowed
// for the call. Event JSON is borrowed for the callback. Destroy cancels all work.
void* squad_language_create(SquadLanguageCallback callback, void* context);
void squad_language_destroy(void* handle);
const char* squad_language_capabilities(void* handle);
void squad_language_refresh(void* handle);
void squad_language_prepare(void* handle, int64_t request, const char* source, const char* target, int32_t speech);
void squad_language_translate(void* handle, int64_t request, const char* source, const char* target, const char* text);
void squad_language_listen(void* handle, int64_t request, const char* source);
void squad_language_audio(void* handle, int64_t request, const float* mono48000, int count);
void squad_language_cancel(void* handle, int64_t request);

#ifdef __cplusplus
}
#endif
