// Decoder for the .bap container written by scripts/bap.py. Plain C with no
// Arduino dependency, so the same source runs under tests/test_bap_decode.c on
// the host and inside a sketch on the badge.
//
// The payload is one continuous stream: a frame is not length-prefixed, it ends
// when stride*height bytes have been produced. Playback is therefore sequential
// only. Looping means rewinding the source and calling bap_rewind.
#ifndef BAP_DECODE_H
#define BAP_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAP_HEADER_BYTES 32

enum {
  BAP_CODEC_RAW = 0,     // packed 1bpp, no entropy stage
  BAP_CODEC_RLE = 1,     // run-length only
  BAP_CODEC_RLE_HS = 2,  // run-length, then heatshrink over the whole stream
};

#define BAP_FLAG_XOR 0x01  // frames are stored XORed against the previous one

typedef struct {
  uint16_t store_w, store_h;   // bpp is 1, 2, 4 or 8; pixels pack MSB first
  uint8_t scale, bpp, codec, hs_window, hs_lookahead, flags;
  uint16_t fps_num, fps_den;
  uint32_t frame_count, payload_bytes, crc32;
  uint16_t stride;       // bytes per row, derived: ceil(store_w * bpp / 8)
  uint32_t frame_bytes;  // stride * store_h, derived
} bap_header;

// Sequential read of the payload. Returns the number of bytes written to dst;
// a short return means the payload ended.
typedef size_t (*bap_read_fn)(void *ctx, uint8_t *dst, size_t len);

typedef struct bap_decoder bap_decoder;

// Parses and sanity-checks the 32-byte header. Returns 0, or -1 if the blob is
// not a .bap the decoder can play.
int bap_parse_header(const uint8_t *blob, size_t len, bap_header *out);

// read/ctx must deliver the payload, i.e. the bytes after the header.
bap_decoder *bap_open(const bap_header *header, bap_read_fn read, void *ctx);
void bap_close(bap_decoder *d);

// Rewind to the first frame. The caller rewinds its own source first.
void bap_rewind(bap_decoder *d);

// Decodes the next frame into frame[header->frame_bytes]. Returns 0, or -1 on a
// truncated or malformed stream.
int bap_next_frame(bap_decoder *d, uint8_t *frame);

uint32_t bap_crc32(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
