#!/usr/bin/env python3
"""Local UDP -> real receiver -> frame.jpg/stream.mjpg regression (stdlib only)."""
import http.client
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(fn, timeout=5):
    end = time.monotonic() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.monotonic() >= end:
            raise AssertionError('Timed out waiting for receiver state')
        time.sleep(.01)


def run(receiver, fixture, mode):
    udp_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    config = {'udp': {'bind': '127.0.0.1', 'port': udp_port},
              'http': {'bind': '127.0.0.1', 'port': http_port, 'jpeg_quality': 85},
              'decoder': {'fill_gaps': mode != 'raw', 'smooth_fill': mode == 'smooth'}}
    # Omitted options must enable smoothing in existing receiver configs.
    if mode == 'smooth':
        del config['decoder']
    with tempfile.TemporaryDirectory() as temp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        path = Path(temp) / 'receiver.json'
        path.write_text(json.dumps(config))
        with (Path(temp) / 'receiver.log').open('w+') as log:
            process = subprocess.Popen([receiver, str(path)], stdout=log, stderr=log)
            def request(path):
                conn = http.client.HTTPConnection('127.0.0.1', http_port, timeout=3)
                try:
                    conn.request('GET', path)
                    response = conn.getresponse()
                    return response.status, response.read()
                finally:
                    conn.close()
            def status():
                code, body = request('/status.json')
                assert code == 200
                return json.loads(body)
            def started():
                assert process.poll() is None, 'receiver exited during startup'
                try:
                    return status()
                except OSError:
                    return None
            sent = 0
            def send(data):
                nonlocal sent
                udp.sendto(data, ('127.0.0.1', udp_port))
                sent += 1
            def region(index, frame=100, stream=7):
                data = bytearray(fixture['regions'][index]['wire'])
                struct.pack_into('<II', data, 4, stream, frame)
                send(data)
            def patch(frame, key, stream=7):
                data = bytearray(51)
                struct.pack_into('<HBBIIQHHHB', data, 0, 0x5846, 0x42, 0, stream, frame,
                                 123456, (frame-key) & 65535, fixture['width'], fixture['height'], 0)
                struct.pack_into('<6f', data, 27, 1, 0, 0, 0, 1, 0)
                send(data)
            def drained():
                return wait_for(lambda: status()['udp']['datagrams'] >= sent)
            def frame_is(frame, stream=7):
                state = status()
                value = state['frame']
                return state if value and value['frame_id'] == frame and value['stream_id'] == stream else None
            def mjpeg():
                conn = http.client.HTTPConnection('127.0.0.1', http_port, timeout=3)
                try:
                    conn.request('GET', '/stream.mjpg')
                    response = conn.getresponse()
                    assert response.status == 200
                    assert 'multipart/x-mixed-replace' in response.getheader('Content-Type')
                    assert response.readline().strip() == b'--flowxframe'
                    headers = {}
                    while True:
                        line = response.readline().strip()
                        if not line:
                            break
                        key, value = line.split(b':', 1)
                        headers[key.lower()] = value.strip()
                    body = response.read(int(headers[b'content-length']))
                    assert body[:2] == b'\xff\xd8' and body[-2:] == b'\xff\xd9'
                    return headers, body
                finally:
                    conn.close()
            try:
                wait_for(started)
                assert request('/frame.jpg')[0] == 503
                samples = [s for s in fixture['concealment'] if s['mode'] == mode]
                region(0); drained()
                assert status()['frame'] is None, 'first region flashed a partial key'
                # Keep delivering duplicates across the deadline; they must not
                # keep the reference hidden or create extra presentations.
                end = time.monotonic() + .18
                while time.monotonic() < end:
                    region(0); time.sleep(.015)
                state = frame_is(100)
                assert state, 'duplicate traffic extended the quiet timeout'
                assert state['decode']['frames'] == 1
                code, first = request('/frame.jpg')
                assert code == 200 and first == bytes(samples[0]['jpeg']), 'HTTP concealment mismatch'
                headers, streamed = mjpeg()
                assert headers[b'x-flowx-frame-id'] == b'100' and streamed == first
                encodes = status()['http']['jpeg_encodes']
                request('/frame.jpg')
                assert status()['http']['jpeg_encodes'] == encodes, 'same frame encoded again'

                for i in range(1, len(fixture['regions'])):
                    region(i)
                drained()
                assert status()['decode']['frames'] == 1, 'late key replayed an old frame'
                assert request('/frame.jpg')[1] == first, 'late data mutated published pixels'
                patch(101, 100); wait_for(lambda: frame_is(101))
                assert request('/frame.jpg')[1] != first, 'PATCH missed the late reference'
                patch(101, 100); region(0); drained()
                assert status()['decode']['frames'] == 2, 'duplicate/stale frame published'

                for i in samples[1]['indices']:
                    region(i, 110)
                drained()
                assert frame_is(101), 'replacement key flashed before burst ended'
                patch(111, 110); state = wait_for(lambda: frame_is(111))
                assert state['decode']['frames'] == 4 and state['decode']['keyframes'] == 2
                headers, streamed = mjpeg()
                assert headers[b'x-flowx-frame-id'] == b'111'
                assert streamed == bytes(samples[1]['jpeg']), 'MJPEG PATCH used a different filled reference'

                # A newer key releases its predecessor; a complete key has no wait.
                region(0, 120); drained(); assert frame_is(111)
                region(0, 130); drained(); assert frame_is(120)
                for i in range(1, len(fixture['regions'])):
                    region(i, 130)
                drained(); assert frame_is(130), 'complete key waited for timeout'
                # New streams discard pending timers and reject retired senders.
                region(0, 140); drained()
                region(0, 1, 8); region(1, 140, 7); drained()
                assert frame_is(130), 'stream switch flashed an incomplete key'
                wait_for(lambda: frame_is(1, 8))
                region(0, 0xfffffffe, 9); patch(1, 0xfffffffe, 9)
                wait_for(lambda: frame_is(1, 9))
                print(f"PASS: MJPEG {mode}, shuffle={bool(fixture['tile_map'])}, UDP/HTTP bytes, "
                      'quiet/PATCH/next-key/completion, duplicates, late recovery, streams, wrap')
            except Exception:
                log.flush(); log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()


if __name__ == '__main__':
    fixture = json.loads(Path(sys.argv[2]).read_text())
    for mode in ('raw', 'nearest', 'smooth'):
        run(str(Path(sys.argv[1]).resolve()), fixture, mode)
