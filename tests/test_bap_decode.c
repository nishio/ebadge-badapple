// Decodes a .bap with the firmware's own C decoder and compares every frame
// against a reference dump produced by scripts/bap.py. Driven by
// tests/verify_bap.py, which generates both files from synthetic frames, so no
// copyrighted material is needed to test the decoder.
//
//   test_bap_decode CLIP.bap REFERENCE.raw [RENDERED.raw]
//
// With a third argument it also expands frame 0 through the render LUT and
// writes the big-endian RGB565 buffer there, so the host can check the pixels
// the panel would actually receive.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bap_decode.h"
#include "bap_render.h"

typedef struct {
  const uint8_t *data;
  size_t len, pos;
} source;

static size_t source_read(void *ctx, uint8_t *dst, size_t len) {
  source *s = (source *)ctx;
  size_t n = s->len - s->pos < len ? s->len - s->pos : len;
  memcpy(dst, s->data + s->pos, n);
  s->pos += n;
  return n;
}

static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)malloc((size_t)size);
  if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
    fprintf(stderr, "cannot read %s\n", path);
    exit(2);
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: %s CLIP.bap REFERENCE.raw [RENDERED.raw]\n", argv[0]);
    return 2;
  }
  size_t blob_len, ref_len;
  uint8_t *blob = slurp(argv[1], &blob_len);
  uint8_t *ref = slurp(argv[2], &ref_len);

  bap_header h;
  if (bap_parse_header(blob, blob_len, &h) != 0) { printf("FAIL header\n"); return 1; }
  if (blob_len != BAP_HEADER_BYTES + h.payload_bytes) { printf("FAIL payload size\n"); return 1; }
  if (ref_len != (size_t)h.frame_count * h.frame_bytes) { printf("FAIL reference size\n"); return 1; }
  if (bap_crc32(0, blob + BAP_HEADER_BYTES, h.payload_bytes) != h.crc32) { printf("FAIL crc32\n"); return 1; }

  source src = {blob + BAP_HEADER_BYTES, h.payload_bytes, 0};
  bap_decoder *d = bap_open(&h, source_read, &src);
  uint8_t *frame = (uint8_t *)malloc(h.frame_bytes);
  if (!d || !frame) { printf("FAIL alloc\n"); return 1; }

  for (uint32_t i = 0; i < h.frame_count; ++i) {
    if (bap_next_frame(d, frame) != 0) { printf("FAIL decode frame %u\n", i); return 1; }
    if (memcmp(frame, ref + (size_t)i * h.frame_bytes, h.frame_bytes) != 0) {
      printf("FAIL mismatch at frame %u\n", i);
      return 1;
    }
  }
  // A frame past the end must fail rather than return stale or partial data.
  if (bap_next_frame(d, frame) == 0) {
    printf("FAIL decoded past the end\n");
    return 1;
  }
  // Looping: rewinding the source and the decoder must reproduce frame 0.
  src.pos = 0;
  bap_rewind(d);
  if (bap_next_frame(d, frame) != 0 || memcmp(frame, ref, h.frame_bytes) != 0) {
    printf("FAIL rewind\n");
    return 1;
  }
  if (argc == 4) {
    uint16_t lut[BAP_LUT_MAX];
    bap_build_lut(lut, h.bpp);
    int outW = h.store_w * h.scale, outH = h.store_h * h.scale;
    uint16_t *out = (uint16_t *)malloc((size_t)outW * outH * sizeof(uint16_t));
    if (!out) { printf("FAIL render alloc\n"); return 1; }
    for (int y = 0; y < h.store_h; ++y)
      bap_expand_row(frame + (size_t)y * h.stride, out + (size_t)y * h.scale * outW,
                     h.store_w, h.bpp, h.scale, outW, lut);
    FILE *f = fopen(argv[3], "wb");
    if (!f || fwrite(out, sizeof(uint16_t), (size_t)outW * outH, f) !=
                  (size_t)outW * outH) { printf("FAIL render write\n"); return 1; }
    fclose(f);
    free(out);
  }
  bap_close(d);
  free(frame); free(ref); free(blob);
  printf("ok %ux%u codec=%u flags=%u %u frames\n", h.store_w, h.store_h, h.codec,
         h.flags, h.frame_count);
  return 0;
}
