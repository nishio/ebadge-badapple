#!/usr/bin/env python3
"""Read video flash in bounded chunks; retry reads only, preserving byte comparison."""
from pathlib import Path
import argparse,hashlib,subprocess

def verify_chunks(expected,offset,reader,chunk_size=512*1024,retries=3):
    digest=hashlib.sha256()
    for pos in range(0,len(expected),chunk_size):
        wanted=expected[pos:pos+chunk_size]
        for attempt in range(1,retries+1):
            try:
                actual=reader(offset+pos,len(wanted))
                if actual!=wanted:raise ValueError(f'Byte mismatch or short read at {offset+pos:#x}')
                break
            except (OSError,ValueError,subprocess.SubprocessError) as error:
                print(f'readback retry address={offset+pos:#x} attempt={attempt}/{retries}: {error}',flush=True)
                if attempt==retries:raise
        digest.update(actual)
        print(f'verified_readback {pos+len(wanted)}/{len(expected)} bytes',flush=True)
    assert digest.hexdigest()==hashlib.sha256(expected).hexdigest()
    return digest.hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('port');p.add_argument('offset',type=lambda s:int(s,0));p.add_argument('clip');a=p.parse_args()
    root=Path(__file__).resolve().parents[1];clip=Path(a.clip);expected=clip.read_bytes()
    assert expected[:4]==b'BAP1' and 0<=a.offset<a.offset+len(expected)<=33554432
    print('readback strategy=persistent-stub chunk_bytes=524288 retries=3 source_sha256='+hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),flush=True)
    import esptool
    from esptool.util import FatalError
    class Reader:
        esp=None
        def disconnect(self,reset=False):
            if self.esp is not None:
                try:
                    if reset:esptool.reset_chip(self.esp,'hard-reset')
                finally:self.esp._port.close();self.esp=None
        def __call__(self,address,size):
            try:
                if self.esp is None:
                    self.esp=esptool.detect_chip(a.port)
                    if self.esp.CHIP_NAME!='ESP32-S3':raise ValueError('Expected ESP32-S3')
                    self.esp=esptool.run_stub(self.esp)
                    self.esp.change_baud(921600)
                    esptool.attach_flash(self.esp)
                # Keep a single stub connection. Reset only after the full check;
                # reconnect after an actual I/O failure, not between every chunk.
                return esptool.read_flash(self.esp,address,size,no_progress=True)
            except (FatalError,OSError) as error:
                self.disconnect()
                raise OSError(str(error)) from error
    reader=Reader()
    try:sha=verify_chunks(expected,a.offset,reader)
    finally:reader.disconnect(reset=True)
    print(f'verified {len(expected)} bytes at {a.offset:#x} by independent chunked readback; sha256={sha}',flush=True)
if __name__=='__main__':main()
