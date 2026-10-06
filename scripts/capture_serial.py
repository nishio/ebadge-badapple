#!/usr/bin/env python3
"""Capture without asserting DTR/RTS; optionally request RAM stats after silence."""
import argparse
import datetime
import time
import serial

ap = argparse.ArgumentParser()
ap.add_argument('port')
ap.add_argument('output')
ap.add_argument('--seconds', type=float, default=10)
ap.add_argument('--until-loops', type=int, default=0,
                help='finish after complete profile timing lines for this loop count; seconds is a deadline')
ap.add_argument('--quiet-seconds', type=float, default=0,
                help='hold open without reading, then send ? to request RAM stats')
ap.add_argument('--query-at-end', action='store_true',
                help='request current and last profile before closing the same connection')
a = ap.parse_args()
s = serial.Serial(port=None, baudrate=115200, timeout=0.2)
s.dtr = False
s.rts = False
s.port = a.port
with open(a.output, 'wb') as f, s:
    def record(data):
        f.write(data)
        f.flush()
    record(('# capture ' + datetime.datetime.now().astimezone().isoformat() + '\n').encode())
    if a.quiet_seconds:
        record(f'# unread for {a.quiet_seconds}s\n'.encode())
        time.sleep(a.quiet_seconds)
        # Drain any old output before asking for the retrospective counters.
        record(s.read(s.in_waiting))
        s.write(b'?')
    end = time.monotonic() + a.seconds
    captured = bytearray()
    matched = not a.until_loops
    while time.monotonic() < end:
        data = s.read(max(1, s.in_waiting))
        record(data)
        if a.until_loops:
            captured.extend(data)
            marker = f'profile last loops={a.until_loops} '.encode()
            pos = captured.find(marker)
            if pos >= 0 and b'parts last ' in captured[pos:] and captured[pos:].count(b'\n') >= 3:
                matched = True
                break
    if not matched:
        record(b'# ERROR: loop completion not observed before deadline\n')
        raise SystemExit(1)
    if a.query_at_end:
        s.write(b'?')
        query_end = time.monotonic() + 1
        while time.monotonic() < query_end:
            record(s.read(max(1, s.in_waiting)))
    record(('# end ' + datetime.datetime.now().astimezone().isoformat() + '\n').encode())
