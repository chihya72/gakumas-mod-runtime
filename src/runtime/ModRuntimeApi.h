#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define GMR_CALL __cdecl
#else
#define GMR_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t GmrResult;

enum {
    GMR_OK = 0,
    GMR_E_INVALID_ARGUMENT = 1,
    GMR_E_API_VERSION = 2,
    GMR_E_NOT_INITIALIZED = 3,
    GMR_E_MOD_NOT_FOUND = 4,
    GMR_E_MANIFEST_INVALID = 5,
    GMR_E_ACCESS_DENIED = 6,
    GMR_E_IO = 7,
    GMR_E_CONCURRENT_CHANGE = 8,
    GMR_E_INTERNAL = 9,
};

enum {
    GMR_API_VERSION_1 = 1,
};

typedef struct GmrOwnedBuffer {
    void* data;
    size_t size;
} GmrOwnedBuffer;

typedef struct GmrRuntimeApiV1 {
    uint32_t structSize;
    uint32_t apiVersion;

    GmrResult (GMR_CALL* getModsJson)(GmrOwnedBuffer* output);
    void (GMR_CALL* freeBuffer)(void* data);
    GmrResult (GMR_CALL* setModEnabled)(const char* modIdUtf8, uint8_t enabled);
    void (GMR_CALL* writeLog)(uint32_t level, const char* component, const char* messageUtf8);
} GmrRuntimeApiV1;

typedef GmrResult (GMR_CALL* GmrGetRuntimeApiV1Fn)(GmrRuntimeApiV1* output);

GmrResult GMR_CALL GmrGetRuntimeApiV1(GmrRuntimeApiV1* output);

#ifdef __cplusplus
}
#endif
