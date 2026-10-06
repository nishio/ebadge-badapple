#!/usr/bin/env python3
"""Differential test of the firmware decoder against FFmpeg, including the local full song."""
import ctypes,hashlib,json,struct,subprocess,sys,tempfile,wave
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'scripts'))
from ima_wav import read_ima_wav

with tempfile.TemporaryDirectory() as td:
    tmp=Path(td);lib=tmp/'ima.dylib';src=ROOT/'firmware/libraries/BadgeAudio/src/ima_mono.c'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',str(src),'-o',str(lib)],check=True)
    fn=ctypes.CDLL(str(lib)).ima_mono_decode
    fn.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t,ctypes.POINTER(ctypes.c_int16),ctypes.c_size_t];fn.restype=ctypes.c_size_t
    def decode(block,capacity=None):
        cap=capacity if capacity is not None else max(1,1+2*(len(block)-4))
        dst=(ctypes.c_int16*(cap+1))(*([12345]*(cap+1)))
        n=fn((ctypes.c_uint8*len(block)).from_buffer_copy(block),len(block),dst,cap)
        assert dst[cap]==12345
        return n,bytes(dst)[:n*2],list(dst)
    invalid=[b'',b'\0'*3,b'\0\0\x59\0',b'\0\0\0\x01']
    for block in invalid:
        n,_,out=decode(block);assert n==0 and all(x==12345 for x in out)
    assert decode(b'\0\0\0\0\xff',2)[0]==0
    assert decode(b'\0\x80\0\0')[1]==struct.pack('<h',-32768)
    assert decode(b'\xff\x7f\x58\0\x77')[1]==struct.pack('<hhh',32767,32767,32767)
    assert decode(b'\0\x80\x58\0\xff')[1]==struct.pack('<hhh',-32768,-32768,-32768)
    rng=np.random.default_rng(124);samples=rng.integers(-32768,32768,44100,dtype=np.int16)
    pcm=tmp/'synthetic.wav'
    with wave.open(str(pcm),'wb') as w:w.setparams((1,2,22050,len(samples),'NONE',''));w.writeframes(samples.tobytes())
    encoded=tmp/'synthetic-ima.wav'
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(pcm),'-c:a','adpcm_ima_wav',str(encoded)],check=True)
    paths=[encoded]+[Path(p) for p in sys.argv[1:]];results=[]
    for path in paths:
        info,data=read_ima_wav(path);decoded=bytearray()
        for pos in range(0,len(data),info['block_bytes']):
            n,chunk,_=decode(data[pos:pos+info['block_bytes']]);assert n==info['block_samples'];decoded.extend(chunk)
        ref=subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-f','s16le','-acodec','pcm_s16le','-'])
        # RIFF fact sets valid samples; whole ADPCM blocks may include tail padding.
        valid=info['samples']*2;assert len(ref)>=valid and len(decoded)>=valid
        assert decoded[:valid]==ref[:valid],str(path)
        results.append({'input':path.name,'wav_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'valid_samples':info['samples'],'decoded_block_samples':len(decoded)//2,'ffmpeg_samples':len(ref)//2,'valid_pcm_sha256':hashlib.sha256(decoded[:valid]).hexdigest(),'bit_exact':True})
    print(json.dumps({'malformed_and_capacity_guards':'passed','saturation_and_negative_predictor':'passed','comparisons':results},indent=2))
