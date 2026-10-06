#!/usr/bin/env python3
"""The .bap container: packed grayscale video for the badge, encoder and reference decoder.

The device decodes this format with firmware/libraries/BadApple; this module is
the other half of that pair and the reference the C decoder is tested against.

Layout is a 32-byte little-endian header followed by one continuous stream. A
frame is not length-prefixed: the decoder stops when it has produced
stride*height bytes, which is all linear playback needs and keeps the encoder
free to run the entropy stage across frame boundaries. That matters, because
measurement showed the LZ stage recovers the inter-frame redundancy by matching
against the previous frame's codes.

The run-length stage is byte-compatible with hackffm/ESP32_BadApple, so
its published frames can be re-encoded into this container for measurement.
"""
import struct
import zlib

MAGIC = b'BAP1'
HEADER_BYTES = 32

CODEC_RAW = 0       # packed 1bpp, no entropy stage at all
CODEC_RLE = 1       # run-length only
CODEC_RLE_HS = 2    # run-length, then heatshrink over the whole stream

FLAG_XOR = 1        # each frame is stored XORed against the previous one

ESCAPES = {0x00: 0x55, 0xFF: 0xAA}


BIT_DEPTHS = (1, 2, 4, 8)


def stride_of(width, bpp=1):
    """Bytes per row. Pixels are packed MSB first, the leftmost in the high bits."""
    return (width * bpp + 7) // 8


def frame_bytes_of(width, height, bpp=1):
    return stride_of(width, bpp) * height


def gray_rgb565(value, bpp):
    """Grey level `value` of 2**bpp, as RGB565. The source is a silhouette, so
    colour buys nothing; only the grey ramp matters."""
    g = 255 * value // ((1 << bpp) - 1)
    return ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3)


# --- run-length stage -------------------------------------------------------

def rle_encode_frame(buf, out):
    """Encode one frame. Runs of 1 or 2 stay literal, as in hackffm/ESP32_BadApple."""
    i, n = 0, len(buf)
    while i < n:
        c = buf[i]
        if c not in ESCAPES:
            out.append(c)
            if c in (0x55, 0xAA):   # an escape byte appearing as data
                out.append(0)
            i += 1
            continue
        j = i + 1
        while j < n and j - i < 32767 and buf[j] == c:
            j += 1
        run = j - i
        if run <= 2:
            out.extend([c] * run)
        else:
            out.append(ESCAPES[c])
            if run <= 127:
                out.append(run)
            else:
                out.extend([(run & 0x7F) | 128, run >> 7])
        i = j


def rle_encode(frames):
    out = bytearray()
    for frame in frames:
        rle_encode_frame(bytes(frame), out)
    return bytes(out)


def rle_decode(buf, limit=None):
    """Inverse of rle_encode. 0x55 escapes runs of 0x00, 0xAA runs of 0xFF."""
    out = bytearray()
    i = 0
    while i < len(buf) and (limit is None or len(out) < limit):
        c = buf[i]; i += 1
        if c not in (0x55, 0xAA):
            out.append(c); continue
        value = 0x00 if c == 0x55 else 0xFF
        n = buf[i]; i += 1
        if n == 0:                      # escape byte appearing as a literal
            out.append(c)
        elif n < 0x80:
            out.extend(bytes([value]) * n)
        else:                           # runs above 127 spill into a second byte
            out.extend(bytes([value]) * ((n & 0x7F) | (buf[i] << 7))); i += 1
    return bytes(out)


# --- inter-frame stage ------------------------------------------------------

def xor_frames(frames):
    out = [bytes(frames[0])]
    for prev, cur in zip(frames, frames[1:]):
        out.append(bytes(a ^ b for a, b in zip(prev, cur)))
    return out


def unxor_frames(frames):
    out = [bytes(frames[0])]
    for cur in frames[1:]:
        out.append(bytes(a ^ b for a, b in zip(out[-1], cur)))
    return out


# --- container --------------------------------------------------------------

def pack_header(store_w, store_h, scale, codec, frame_count, payload,
                hs_window=11, hs_lookahead=4, flags=0, fps_num=30, fps_den=1, bpp=1):
    return struct.pack('<4sHHBBBBBBHHIII H', MAGIC, store_w, store_h, scale, bpp,
                       codec, hs_window, hs_lookahead, flags, fps_num, fps_den,
                       frame_count, len(payload), zlib.crc32(payload) & 0xFFFFFFFF, 0)


def parse_header(blob):
    if len(blob) < HEADER_BYTES or blob[:4] != MAGIC:
        raise ValueError('not a .bap file')
    (_, w, h, scale, bpp, codec, window, lookahead, flags, fps_num, fps_den,
     count, payload_bytes, crc, _) = struct.unpack('<4sHHBBBBBBHHIII H',
                                                   blob[:HEADER_BYTES])
    return dict(store_w=w, store_h=h, scale=scale, bpp=bpp, codec=codec,
                hs_window=window, hs_lookahead=lookahead, flags=flags,
                fps_num=fps_num, fps_den=fps_den, frame_count=count,
                payload_bytes=payload_bytes, crc32=crc,
                stride=stride_of(w, bpp), frame_bytes=frame_bytes_of(w, h, bpp))


def encode(frames, store_w, store_h, scale=2, codec=CODEC_RLE_HS, xor=False,
           hs_window=11, hs_lookahead=4, fps_num=30, fps_den=1, bpp=1):
    """frames: packed frames, pixels MSB first, stride_of(store_w, bpp) per row."""
    if bpp not in BIT_DEPTHS:
        raise ValueError(f'bpp must be one of {BIT_DEPTHS}')
    want = frame_bytes_of(store_w, store_h, bpp)
    for i, frame in enumerate(frames):
        if len(frame) != want:
            raise ValueError(f'frame {i} is {len(frame)} bytes, expected {want}')
    staged = xor_frames(frames) if xor else frames
    if codec == CODEC_RAW:
        payload = b''.join(bytes(f) for f in staged)
    else:
        payload = rle_encode(staged)
        if codec == CODEC_RLE_HS:
            import heatshrink2
            payload = heatshrink2.compress(payload, window_sz2=hs_window,
                                           lookahead_sz2=hs_lookahead)
    header = pack_header(store_w, store_h, scale, codec, len(frames), payload,
                         hs_window, hs_lookahead, FLAG_XOR if xor else 0,
                         fps_num, fps_den, bpp)
    return header + payload


def decode(blob):
    """Reference decode. Returns (header dict, list of packed frames)."""
    h = parse_header(blob)
    payload = blob[HEADER_BYTES:HEADER_BYTES + h['payload_bytes']]
    if zlib.crc32(payload) & 0xFFFFFFFF != h['crc32']:
        raise ValueError('payload crc32 mismatch')
    if h['codec'] == CODEC_RLE_HS:
        import heatshrink2
        flat = rle_decode(heatshrink2.decompress(
            payload, window_sz2=h['hs_window'], lookahead_sz2=h['hs_lookahead']))
    elif h['codec'] == CODEC_RLE:
        flat = rle_decode(payload)
    else:
        flat = payload
    n, fb = h['frame_count'], h['frame_bytes']
    if len(flat) < n * fb:
        raise ValueError(f'stream holds {len(flat)} bytes, need {n * fb}')
    frames = [flat[i * fb:(i + 1) * fb] for i in range(n)]
    return h, (unxor_frames(frames) if h['flags'] & FLAG_XOR else frames)


# --- synthetic material -----------------------------------------------------

def synthetic_frames(width, height, count, bpp=1):
    """Deterministic silhouette-like frames: large solid shapes that move a
    little each frame, so neighbouring frames stay similar the way real
    footage does. Used by the tests and the decode benchmark, so that neither
    needs copyrighted material. Above 1 bit the shapes get a soft edge, so the
    grey levels are actually exercised rather than left at the two extremes."""
    stride = stride_of(width, bpp)
    levels = (1 << bpp) - 1
    frames = []
    for t in range(count):
        buf = bytearray(stride * height)
        cx = width // 2 + int(width * 0.28 * _wave(t, 37))
        cy = height // 2 + int(height * 0.22 * _wave(t, 23))
        r = max(4, int(min(width, height) * (0.30 + 0.06 * _wave(t, 53))))
        bar = (t * 3) % max(1, height - 8)
        for y in range(height):
            dy = y - cy
            row = y * stride
            inside_bar = bar <= y < bar + 6
            for x in range(width):
                dx = x - cx
                d2 = dx * dx + dy * dy
                if inside_bar or x < 3 or d2 < (r - 2) ** 2:
                    value = levels
                elif d2 < r * r:                       # soft edge, mid greys
                    value = levels - (levels * (d2 - (r - 2) ** 2)
                                      // max(1, r * r - (r - 2) ** 2))
                else:
                    value = 0
                if value:
                    bit = x * bpp
                    shift = 8 - bpp - (bit & 7)
                    buf[row + (bit >> 3)] |= value << shift
        frames.append(bytes(buf))
    return frames


def _wave(t, period):
    """Triangle wave in [-1, 1]; avoids importing math for the shape tables."""
    phase = (t % period) / period
    return 4 * abs(phase - 0.5) - 1
