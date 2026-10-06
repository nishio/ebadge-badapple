#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// One 4-bit mono IMA WAV block: 4-byte predictor/index header, low nibble first.
// Returns PCM samples written, or zero for malformed input/insufficient output.
size_t ima_mono_decode(const uint8_t *block, size_t bytes, int16_t *pcm, size_t capacity);
#ifdef __cplusplus
}
#endif
