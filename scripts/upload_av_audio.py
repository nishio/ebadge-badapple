#!/usr/bin/env python3
"""Append AAP1 at the next erase sector after the existing BAP; preserve video."""
import argparse,hashlib,json,struct,zlib
from pathlib import Path
import esptool
from bap import parse_header
from flash_partition import find_partition
from verify_video_readback import verify_chunks

def main():
    p=argparse.ArgumentParser();p.add_argument('port');p.add_argument('video',type=Path);p.add_argument('audio',type=Path)
    p.add_argument('--backup',type=Path,default=Path('build/av-audio/tail-before.bin'))
    p.add_argument('--metadata',type=Path,default=Path('build/av-audio/upload.json'));a=p.parse_args()
    if a.backup.exists() or a.metadata.exists():p.error('Backup or metadata exists; choose fresh paths and preserve previous evidence')
    a.backup.parent.mkdir(parents=True,exist_ok=True);a.metadata.parent.mkdir(parents=True,exist_ok=True)
    video=a.video.read_bytes();h=parse_header(video);audio=a.audio.read_bytes();words=struct.unpack('<16I',audio[:64])
    assert len(video)==32+h['payload_bytes'] and zlib.crc32(video[32:])==h['crc32']
    assert words[0]==int.from_bytes(b'AAP1','little') and words[1]==1 and words[8]==h['crc32'] and words[9]==len(video)
    assert len(audio)==64+words[6] and zlib.crc32(audio[64:])==words[7]
    esp=esptool.detect_chip(a.port);assert esp.CHIP_NAME=='ESP32-S3';esp=esptool.run_stub(esp)
    esp.change_baud(460800);esptool.attach_flash(esp)
    def read(address,size):
        nonlocal esp
        for attempt in range(3):
            try:return esptool.read_flash(esp,address,size,no_progress=True)
            except (esptool.FatalError,OSError) as err:
                print(f'read retry {address:#x} {attempt+1}: {err}',flush=True)
                if attempt==2:raise
                esp._port.close()
                esp=esptool.detect_chip(a.port);assert esp.CHIP_NAME=='ESP32-S3';esp=esptool.run_stub(esp)
                esp.change_baud(460800);esptool.attach_flash(esp)
    try:
        table=read(0x8000,4096);base,limit=find_partition(table,'ffat')
        actual=read(base,32);assert actual==video[:32],'Device video differs'
        offset=(len(video)+4095)&~4095;erase_size=(len(audio)+4095)&~4095
        assert offset+erase_size<=limit and base+offset+erase_size<=0x1ff0000
        backup=a.backup
        if backup.exists():raise ValueError('Backup exists; inspect previous upload before retrying')
        before=bytearray()
        for pos in range(0,erase_size,131072):before.extend(read(base+offset+pos,min(131072,erase_size-pos)))
        with backup.open('xb') as f:f.write(before)
        print(f'append base={base:#x} relative={offset} absolute={base+offset:#x} bytes={len(audio)} erase_bytes={erase_size} video_end={base+len(video):#x}',flush=True)
        esptool.write_flash(esp,[(base+offset,str(a.audio))])
        sha=verify_chunks(audio,base+offset,read,chunk_size=131072)
        assert read(base,32)==actual
        meta={'absolute_offset':base+offset,'relative_offset':offset,'asset_bytes':len(audio),'erase_bytes':erase_size,'ffat_size':limit,'remaining_bytes_after_asset':limit-offset-len(audio),'asset_sha256':sha,'readback_match':True,'video_header_unchanged':True,'prior_tail_sha256':hashlib.sha256(before).hexdigest()}
        meta.update({'audio_path':str(a.audio),'backup_path':str(backup)})
        with a.metadata.open('x') as f:f.write(json.dumps(meta,indent=2)+'\n')
        print(json.dumps(meta,indent=2))
    finally:
        esptool.reset_chip(esp,'hard-reset');esp._port.close()
if __name__=='__main__':main()
