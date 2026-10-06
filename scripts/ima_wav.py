"""Read a mono 4-bit IMA ADPCM WAV written by FFmpeg (adpcm_ima_wav)."""
import struct
from pathlib import Path


def read_ima_wav(path):
    blob = Path(path).read_bytes()
    if blob[:4] != b'RIFF' or blob[8:12] != b'WAVE' or len(blob) != struct.unpack_from('<I', blob, 4)[0]+8:
        raise ValueError('Expected complete RIFF WAVE')
    chunks={};pos=12
    while pos+8 <= len(blob):
        tag=blob[pos:pos+4];n=struct.unpack_from('<I',blob,pos+4)[0];pos+=8
        if n > len(blob)-pos or tag in chunks: raise ValueError('Truncated or duplicate chunk')
        chunks[tag]=blob[pos:pos+n];pos+=n+(n&1)
    fmt=chunks[b'fmt ']
    if len(fmt)<20: raise ValueError('Missing IMA format extension')
    codec,channels,rate,avg,align,bits,extra,spb=struct.unpack_from('<HHIIHHHH',fmt)
    if (codec,channels,bits,extra)!=(17,1,4,2) or align<4 or spb!=1+2*(align-4):
        raise ValueError('Only 4-bit mono IMA WAV supported')
    data=chunks[b'data'];samples=struct.unpack_from('<I',chunks[b'fact'])[0]
    if len(data)%align or not 0 < samples <= len(data)//align*spb:
        raise ValueError('Invalid block count or fact samples')
    return {'rate':rate,'block_bytes':align,'block_samples':spb,'samples':samples},data
