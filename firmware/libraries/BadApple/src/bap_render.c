#include "bap_render.h"

#include <string.h>

void bap_build_lut(uint16_t *lut, uint8_t bpp) {
  int levels = (1 << bpp) - 1;
  for (int v = 0; v <= levels; ++v) {
    int g = 255 * v / levels;
    uint16_t rgb = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    lut[v] = (uint16_t)((rgb >> 8) | (rgb << 8));   // stored big-endian
  }
}

// One inner loop per depth: the shift pattern is fixed per depth, so keeping
// them apart avoids recomputing a bit offset for every pixel.
void bap_expand_row(const uint8_t *row, uint16_t *dst, int width, uint8_t bpp,
                    int scale, int pitch, const uint16_t *lut) {
  // Native-size video has no replication. Decode one packed byte at a time,
  // keeping shifts constant and eliminating the per-pixel scale loop. The
  // generic path below remains for magnification and odd-width tails are safe.
  if (scale == 1) {
    int x = 0;
    if (bpp == 2) {
      for (; x + 4 <= width; x += 4) {
        uint8_t v = row[x >> 2];
        dst[x] = lut[v >> 6]; dst[x+1] = lut[(v >> 4) & 3];
        dst[x+2] = lut[(v >> 2) & 3]; dst[x+3] = lut[v & 3];
      }
      for (; x < width; ++x) dst[x] = lut[(row[x >> 2] >> (6 - 2*(x & 3))) & 3];
    } else if (bpp == 4) {
      for (; x + 2 <= width; x += 2) {
        uint8_t v = row[x >> 1];
        dst[x] = lut[v >> 4]; dst[x+1] = lut[v & 15];
      }
      if (x < width) dst[x] = lut[row[x >> 1] >> 4];
    } else if (bpp == 8) {
      for (; x < width; ++x) dst[x] = lut[row[x]];
    } else {
      for (; x < width; ++x) dst[x] = lut[(row[x >> 3] >> (7 - (x & 7))) & 1];
    }
    return;
  }
  uint16_t *p = dst;
  switch (bpp) {
    case 1:
      for (int x = 0; x < width; ++x) {
        uint16_t c = lut[(row[x >> 3] >> (7 - (x & 7))) & 1];
        for (int s = 0; s < scale; ++s) *p++ = c;
      }
      break;
    case 2:
      for (int x = 0; x < width; ++x) {
        uint16_t c = lut[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3];
        for (int s = 0; s < scale; ++s) *p++ = c;
      }
      break;
    case 4:
      for (int x = 0; x < width; ++x) {
        uint8_t byte = row[x >> 1];
        uint16_t c = lut[(x & 1) ? (byte & 0x0F) : (byte >> 4)];
        for (int s = 0; s < scale; ++s) *p++ = c;
      }
      break;
    default:   // 8 bpp
      for (int x = 0; x < width; ++x) {
        uint16_t c = lut[row[x]];
        for (int s = 0; s < scale; ++s) *p++ = c;
      }
      break;
  }
  for (int s = 1; s < scale; ++s)
    memcpy(dst + (size_t)s * pitch, dst, (size_t)width * scale * sizeof(uint16_t));
}
