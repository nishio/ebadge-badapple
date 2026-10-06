#include "bap_decode.h"

#include <stdlib.h>
#include <string.h>

#include "heatshrink_decoder.h"

#define BAP_IN_BYTES 1024     // compressed bytes pulled from the source at once
#define BAP_STAGE_BYTES 1024  // run-length codes staged between LZ and frame

struct bap_decoder {
  bap_header h;
  bap_read_fn read;
  void *ctx;
  heatshrink_decoder *hsd;  // NULL unless codec is BAP_CODEC_RLE_HS
  uint8_t *in;
  size_t in_len, in_pos;
  uint8_t *stage;
  size_t stage_len, stage_pos;
  uint8_t *prev;  // NULL unless BAP_FLAG_XOR
  int source_done;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int bap_parse_header(const uint8_t *blob, size_t len, bap_header *out) {
  if (!blob || !out || len < BAP_HEADER_BYTES) return -1;
  if (memcmp(blob, "BAP1", 4) != 0) return -1;
  bap_header h;
  h.store_w = rd16(blob + 4);
  h.store_h = rd16(blob + 6);
  h.scale = blob[8];
  h.bpp = blob[9];
  h.codec = blob[10];
  h.hs_window = blob[11];
  h.hs_lookahead = blob[12];
  h.flags = blob[13];
  h.fps_num = rd16(blob + 14);
  h.fps_den = rd16(blob + 16);
  h.frame_count = rd32(blob + 18);
  h.payload_bytes = rd32(blob + 22);
  h.crc32 = rd32(blob + 26);
  if (h.bpp != 1 && h.bpp != 2 && h.bpp != 4 && h.bpp != 8) return -1;
  if (h.store_w == 0 || h.store_h == 0) return -1;
  if (h.codec > BAP_CODEC_RLE_HS) return -1;
  if (h.codec == BAP_CODEC_RLE_HS && (h.hs_window < 4 || h.hs_window > 15)) return -1;
  if (h.fps_num == 0 || h.fps_den == 0) return -1;
  h.stride = (uint16_t)(((uint32_t)h.store_w * h.bpp + 7) / 8);
  h.frame_bytes = (uint32_t)h.stride * h.store_h;
  *out = h;
  return 0;
}

bap_decoder *bap_open(const bap_header *header, bap_read_fn read, void *ctx) {
  if (!header || !read) return NULL;
  bap_decoder *d = (bap_decoder *)calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->h = *header;
  d->read = read;
  d->ctx = ctx;
  d->in = (uint8_t *)malloc(BAP_IN_BYTES);
  d->stage = (uint8_t *)malloc(BAP_STAGE_BYTES);
  if (!d->in || !d->stage) { bap_close(d); return NULL; }
  if (header->codec == BAP_CODEC_RLE_HS) {
    d->hsd = heatshrink_decoder_alloc(BAP_IN_BYTES, header->hs_window,
                                      header->hs_lookahead);
    if (!d->hsd) { bap_close(d); return NULL; }
  }
  if (header->flags & BAP_FLAG_XOR) {
    d->prev = (uint8_t *)calloc(1, header->frame_bytes);
    if (!d->prev) { bap_close(d); return NULL; }
  }
  return d;
}

void bap_close(bap_decoder *d) {
  if (!d) return;
  if (d->hsd) heatshrink_decoder_free(d->hsd);
  free(d->prev);
  free(d->stage);
  free(d->in);
  free(d);
}

void bap_rewind(bap_decoder *d) {
  if (!d) return;
  if (d->hsd) heatshrink_decoder_reset(d->hsd);
  d->in_len = d->in_pos = d->stage_len = d->stage_pos = 0;
  d->source_done = 0;
  if (d->prev) memset(d->prev, 0, d->h.frame_bytes);
}

// Refills d->stage with run-length codes. Returns 0 when the stream is over.
static int fill_stage(bap_decoder *d) {
  d->stage_pos = 0;
  d->stage_len = 0;
  if (!d->hsd) {
    if (d->source_done) return 0;
    d->stage_len = d->read(d->ctx, d->stage, BAP_STAGE_BYTES);
    if (d->stage_len < BAP_STAGE_BYTES) d->source_done = 1;
    return d->stage_len != 0;
  }
  while (d->stage_len == 0) {
    size_t got = 0;
    HSD_poll_res poll =
        heatshrink_decoder_poll(d->hsd, d->stage, BAP_STAGE_BYTES, &got);
    if (poll < 0) return 0;
    d->stage_len = got;
    if (got) break;
    if (d->in_pos == d->in_len) {  // LZ wants input and we have none staged
      if (d->source_done) {
        // No input left: drain whatever the window still holds, once.
        if (heatshrink_decoder_finish(d->hsd) != HSDR_FINISH_MORE) return 0;
        continue;
      }
      d->in_len = d->read(d->ctx, d->in, BAP_IN_BYTES);
      d->in_pos = 0;
      if (d->in_len < BAP_IN_BYTES) d->source_done = 1;
      if (d->in_len == 0) continue;
    }
    size_t sunk = 0;
    if (heatshrink_decoder_sink(d->hsd, d->in + d->in_pos, d->in_len - d->in_pos,
                                &sunk) < 0)
      return 0;
    d->in_pos += sunk;
  }
  return d->stage_len != 0;
}

static inline int next_byte(bap_decoder *d, uint8_t *out) {
  if (d->stage_pos == d->stage_len && !fill_stage(d)) return 0;
  *out = d->stage[d->stage_pos++];
  return 1;
}

static int take(bap_decoder *d, uint8_t *dst, size_t want) {
  while (want) {
    if (d->stage_pos == d->stage_len && !fill_stage(d)) return 0;
    size_t have = d->stage_len - d->stage_pos;
    size_t n = have < want ? have : want;
    memcpy(dst, d->stage + d->stage_pos, n);
    d->stage_pos += n;
    dst += n;
    want -= n;
  }
  return 1;
}

// 0x55 escapes runs of 0x00, 0xAA runs of 0xFF; a length of 0 means the escape
// byte itself appeared as data. Lengths above 127 spill into a second byte.
static int decode_rle_frame(bap_decoder *d, uint8_t *frame) {
  const uint32_t want = d->h.frame_bytes;
  uint32_t produced = 0;
  while (produced < want) {
    uint8_t c;
    if (!next_byte(d, &c)) return -1;
    if (c != 0x55 && c != 0xAA) {
      frame[produced++] = c;
      continue;
    }
    uint8_t n;
    if (!next_byte(d, &n)) return -1;
    if (n == 0) {
      frame[produced++] = c;
      continue;
    }
    uint32_t run = n & 0x7F;
    if (n & 0x80) {
      uint8_t hi;
      if (!next_byte(d, &hi)) return -1;
      run |= (uint32_t)hi << 7;
    }
    if (run > want - produced) return -1;
    memset(frame + produced, c == 0x55 ? 0x00 : 0xFF, run);
    produced += run;
  }
  return 0;
}

int bap_next_frame(bap_decoder *d, uint8_t *frame) {
  if (!d || !frame) return -1;
  if (d->h.codec == BAP_CODEC_RAW) {
    if (!take(d, frame, d->h.frame_bytes)) return -1;
  } else if (decode_rle_frame(d, frame) != 0) {
    return -1;
  }
  if (d->prev) {
    for (uint32_t i = 0; i < d->h.frame_bytes; ++i) frame[i] ^= d->prev[i];
    memcpy(d->prev, frame, d->h.frame_bytes);
  }
  return 0;
}

// Nibble-table crc32, small enough to keep in flash without a 1 KB table.
uint32_t bap_crc32(uint32_t crc, const uint8_t *data, size_t len) {
  static const uint32_t table[16] = {
      0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
      0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
      0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ table[crc & 0x0F];
    crc = (crc >> 4) ^ table[crc & 0x0F];
  }
  return ~crc;
}
