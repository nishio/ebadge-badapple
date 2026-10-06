// Turns a packed .bap frame into the RGB565 rows the panel wants.
//
// The panel is fed with draw16bitBeRGBBitmap, which DMAs the buffer out
// untouched, so the buffer has to hold big-endian pixels. At 1 bpp that was
// free: 0x0000 and 0xFFFF are the same either way. Greys are not, so the
// lookup table stores the bytes already swapped and the expansion stays a
// table read -- no per-pixel swap, which would cost about 22 ms a frame.
#ifndef BAP_RENDER_H
#define BAP_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAP_LUT_MAX 256   // entries for the deepest supported format, 8 bpp

// Fills lut[0 .. (1<<bpp)-1] with the grey ramp as byte-swapped RGB565.
// The source is a silhouette, so grey is the whole palette.
void bap_build_lut(uint16_t *lut, uint8_t bpp);

// Expands one packed row into `scale` identical output rows starting at dst,
// each `pitch` pixels apart, magnifying every pixel `scale` times across.
void bap_expand_row(const uint8_t *row, uint16_t *dst, int width, uint8_t bpp,
                    int scale, int pitch, const uint16_t *lut);

#ifdef __cplusplus
}
#endif

#endif
