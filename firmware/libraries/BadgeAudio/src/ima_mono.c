#include "ima_mono.h"
#include <stdint.h>

// IMA ADPCM's fixed step and index tables; block state resets at each WAV block.
static const int steps[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
  73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
  408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
  1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
  5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
  18500,20350,22385,24623,27086,29794,32767
};
static const int shifts[8] = {-1,-1,-1,-1,2,4,6,8};

size_t ima_mono_decode(const uint8_t *block, size_t bytes, int16_t *pcm, size_t capacity) {
  if (!block || !pcm || bytes < 4 || bytes > (SIZE_MAX - 1) / 2 + 4) return 0;
  size_t samples = 1 + (bytes - 4) * 2;
  if (capacity < samples || block[2] > 88 || block[3] != 0) return 0;
  int raw = block[0] | ((int)block[1] << 8);
  int predictor = raw >= 32768 ? raw - 65536 : raw;
  int index = block[2];
  pcm[0] = (int16_t)predictor;
  for (size_t i = 1; i < samples; ++i) {
    unsigned code = (block[4 + (i - 1) / 2] >> (((i - 1) & 1) * 4)) & 15;
    int step = steps[index];
    // Match the local FFmpeg 8 IMA WAV path: multiply before truncating.
    int delta = ((2 * (int)(code & 7) + 1) * step) >> 3;
    predictor += (code & 8) ? -delta : delta;
    if (predictor < -32768) predictor = -32768;
    if (predictor > 32767) predictor = 32767;
    index += shifts[code & 7];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    pcm[i] = (int16_t)predictor;
  }
  return samples;
}
