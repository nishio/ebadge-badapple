#!/usr/bin/env python3
"""Create a local-only full-length AAP1 mono IMA asset paired to a BAP video."""
import argparse,hashlib,json,struct,subprocess,zlib
from pathlib import Path
from ima_wav import read_ima_wav
from bap import parse_header

def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('video',type=Path)
    p.add_argument('--output-dir',type=Path,default=Path('build/av-audio'))
    p.add_argument('--preset',choices=['original','bass-headroom'],default='original');a=p.parse_args()
    video=a.video.read_bytes();assert video[:4]==b'BAP1'
    h=parse_header(video);fps=h['fps_num'];den=h['fps_den'];frames=h['frame_count'];payload=h['payload_bytes'];crc=h['crc32']
    assert len(video)==32+payload and zlib.crc32(video[32:])==crc
    rate=22050;samples=round(frames*den*rate/fps)
    out=a.output_dir;out.mkdir(parents=True,exist_ok=True)
    wav=out/'full.wav'
    if wav.exists() or (out/'full.aap').exists():
        p.error('Output exists; use a fresh --output-dir to preserve prior assets')
    filters=f'aresample={rate},apad,atrim=end_sample={samples}'
    if a.preset=='bass-headroom':
        # Explicit half-sum downmix and float processing avoid implicit +3 dB
        # downmix gain and integer clipping before the preattenuation/EQ.
        filters=f'pan=mono|c0=0.5*c0+0.5*c1,aformat=sample_fmts=fltp,volume=-6dB,{filters},bass=g=3:f=350:t=q:w=0.707:r=f32'
    cmd=['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-y','-i',str(a.source),'-vn','-ac','1','-ar',str(rate),'-af',filters,'-c:a','adpcm_ima_wav',str(wav)]
    subprocess.run(cmd,check=True);info,data=read_ima_wav(wav)
    assert info['samples']==samples and info['block_bytes']==1024 and info['block_samples']==2041,info
    words=[int.from_bytes(b'AAP1','little'),1,rate,samples,1024,2041,len(data),zlib.crc32(data),crc,len(video),frames,fps,den,0,0,0]
    asset=struct.pack('<16I',*words)+data;(out/'full.aap').write_bytes(asset)
    meta={'command':cmd,'source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),'video_sha256':hashlib.sha256(video).hexdigest(),'video_bytes':len(video),'video_crc':crc,'audio_payload_bytes':len(data),'audio_asset_bytes':len(asset),'audio_sha256':hashlib.sha256(asset).hexdigest(),'relative_offset':(len(video)+4095)&~4095,'valid_samples':samples,'rate':rate,'duration_s':samples/rate,'info':info}
    meta['preset']=a.preset
    (out/'asset.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta,indent=2))
if __name__=='__main__':main()
