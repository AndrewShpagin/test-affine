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


def run(receiver, fixture, mode, playback=None, jitter=None, live=False):
    udp_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    config = {'udp': {'bind': '127.0.0.1', 'port': udp_port},
              'http': {'bind': '127.0.0.1', 'port': http_port, 'jpeg_quality': 85},
              'decoder': {'fill_gaps': mode != 'raw', 'smooth_fill': mode == 'smooth'}}
    # Omitted options must enable smoothing in existing receiver configs.
    if mode == 'smooth':
        del config['decoder']
    if playback is not None:
        config['playback'] = playback
    if jitter is not None:
        config['udp'].update(jitter)
    with tempfile.TemporaryDirectory() as temp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        path = Path(temp) / 'receiver.json'
        path.write_text(json.dumps(config))
        with (Path(temp) / 'receiver.log').open('w+') as log:
            process = subprocess.Popen([receiver, str(path)], stdout=log, stderr=log)
            def request(path, method='GET', body=None):
                conn = http.client.HTTPConnection('127.0.0.1', http_port, timeout=3)
                try:
                    conn.request(method, path, body, {'Content-Type': 'application/json'} if body is not None else {})
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
                return bytes(data)
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
                expected_playback = playback or {'keyframe_wait_ms': 100, 'playout_ms': 60}
                assert status()['playback'] == expected_playback
                code, script = request('/flowx.js')
                assert code == 200
                defaults = script.split(b'\n', 1)[0]
                assert defaults.startswith(b'globalThis.FLOWX_PLAYBACK_DEFAULTS=')
                assert json.loads(defaults.split(b'=', 1)[1].rstrip(b';')) == expected_playback
                assert request('/frame.jpg')[0] == 503
                if live:
                    def controls():
                        code, body = request('/decoder.json')
                        assert code == 200
                        return json.loads(body)

                    def change(patch=None, path='/decoder.json', method='POST'):
                        code, body = request(path, method, json.dumps(patch) if patch is not None else None)
                        assert code == 200, body
                        result = json.loads(body)
                        wait_for(lambda: controls()['applied_revision'] >= result['revision'])
                        return controls()

                    initial = controls()
                    assert initial['fill_gaps'] and initial['smooth_fill']
                    assert initial['revision'] == initial['applied_revision'] == 0
                    page = request('/mjpg.html')
                    assert page[0] == 200 and b'/decoder.json' in page[1] and b'<img' in page[1]
                    original_browser = request('/flowx.js')[1]

                    # Settings take effect before any data and keep the smoothing preference.
                    off = change({'fill_gaps': False})
                    assert not off['fill_gaps'] and off['smooth_fill'] and not off['smooth_fill_active']
                    assert status()['frame'] is None
                    references = {m: [s for s in fixture['concealment'] if s['mode'] == m][1]
                                  for m in ('raw', 'nearest', 'smooth')}
                    for i in references['raw']['indices']:
                        region(i)
                    drained(); wait_for(lambda: frame_is(100))
                    assert request('/frame.jpg')[1] == bytes(references['raw']['jpeg'])
                    count = status()['decode']['frames']

                    # Keep one real MJPEG connection open across all setting changes.
                    connection = http.client.HTTPConnection('127.0.0.1', http_port, timeout=4)
                    try:
                        connection.request('GET', '/stream.mjpg')
                        stream = connection.getresponse()
                        assert stream.status == 200

                        def next_jpeg():
                            while True:
                                line = stream.readline()
                                assert line, 'MJPEG connection closed during live update'
                                if line.strip() == b'--flowxframe':
                                    break
                            headers = {}
                            while True:
                                line = stream.readline().strip()
                                if not line:
                                    break
                                key, value = line.split(b':', 1)
                                headers[key.lower()] = value.strip()
                            return headers, stream.read(int(headers[b'content-length']))

                        assert next_jpeg()[1] == bytes(references['raw']['jpeg'])
                        for target in ('nearest', 'smooth', 'raw', 'smooth'):
                            before = status()['frame']
                            change({'fill_gaps': target != 'raw', 'smooth_fill': target == 'smooth'})
                            after = status()['frame']
                            assert after['frame_id'] == before['frame_id'] == 100
                            assert after['sequence'] > before['sequence']
                            assert status()['decode']['frames'] == count, 'redraw counted as a new video frame'
                            wanted = bytes(references[target]['jpeg'])
                            assert request('/frame.jpg')[1] == wanted, 'live concealment/cache mismatch'
                            headers, jpeg = next_jpeg()
                            assert headers[b'x-flowx-frame-id'] == b'100' and jpeg == wanted
                    finally:
                        connection.close()

                    # A no-op does not redraw. Reject the whole request on any bad field.
                    before = status()['frame']['sequence']; same = controls()
                    change({'fill_gaps': True})
                    assert controls()['revision'] == same['revision']
                    assert status()['frame']['sequence'] == before
                    for body in ('broken', '[]', '{"fill_gaps":0}', '{"smooth_fill":"false"}',
                                 '{"fill_gaps":false,"unknown":true}'):
                        assert request('/decoder.json', 'POST', body)[0] == 400
                        assert controls() == same
                    for path in ('/setparam/fill_gaps/maybe', '/setparam/fill_gaps',
                                 '/setparam/fill_gaps/false/unknown/true'):
                        assert request(path)[0] == 400
                        assert controls() == same
                    change(path='/setparam/fill_gaps/0/smooth_fill/1', method='GET')
                    assert not controls()['fill_gaps'] and controls()['smooth_fill']
                    change({'fill_gaps': True}, method='PUT')
                    assert controls()['smooth_fill_active']
                    assert status()['decoder'] == controls()
                    assert request('/flowx.js')[1] == original_browser, 'native controls changed browser defaults'

                    # Redrawing a PATCH keeps its ID; restarting the stream keeps live options.
                    patch(101, 100); wait_for(lambda: frame_is(101))
                    change({'fill_gaps': False})
                    assert frame_is(101) and status()['frame']['keyframe_id'] == 100
                    assert request('/frame.jpg')[1] == bytes(references['raw']['jpeg'])
                    region(0, 1, 8); wait_for(lambda: frame_is(1, 8))
                    raw_first = next(s for s in fixture['concealment'] if s['mode'] == 'raw')
                    assert request('/frame.jpg')[1] == bytes(raw_first['jpeg'])
                    assert not controls()['fill_gaps']
                    print(f"PASS: live MJPEG controls, shuffle={bool(fixture['tile_map'])}, "
                          'idle redraw, persistent stream, validation, PATCH IDs, stream reset')
                    return
                if jitter is not None:
                    begin = time.monotonic()
                    region(0)
                    if jitter['jitter_min_ms']:
                        time.sleep(jitter['jitter_min_ms']/2000)
                        assert status()['udp']['datagrams'] == 0, 'packet released before simulated latency'
                    state = wait_for(lambda: frame_is(100))
                    assert time.monotonic()-begin >= (jitter['jitter_min_ms']+playback['keyframe_wait_ms'])/1000-.01
                    assert state['frame']['capture_timestamp_us'] == 123456, 'jitter changed capture timestamp'
                    # Drain late regions, then a PATCH must use the recovered reference.
                    for i in range(1, len(fixture['regions'])):
                        region(i)
                    drained()
                    assert status()['decode']['frames'] == 1, 'jitter/late data replayed key'
                    wire_patch = patch(101, 100); wait_for(lambda: frame_is(101)); drained()
                    sim = status()['udp']['sim_jitter']
                    assert sim['min_ms'] == jitter['jitter_min_ms'] and sim['max_ms'] == jitter['jitter_max_ms']
                    assert sim['seed'] == jitter['jitter_seed'] and sim['scheduled'] == sim['delivered'] == sent
                    assert sim['queued'] == sim['overflow'] == 0
                    headers, body = mjpeg()
                    assert headers[b'x-flowx-frame-id'] == b'101' and body == request('/frame.jpg')[1]
                    # The browser transport must receive the same delayed,
                    # unmodified packets, including their capture timestamps.
                    conn = http.client.HTTPConnection('127.0.0.1', http_port, timeout=3)
                    try:
                        conn.request('GET', '/flowx.bin'); response = conn.getresponse()
                        assert response.status == 200
                        head = response.read(12); assert head[:4] == b'FXB1'
                        size = struct.unpack_from('<I',head,8)[0]
                        record = head+response.read(size-12)
                        count = struct.unpack_from('<H',record,24)[0]
                        packets, pos = [], 28
                        for _ in range(count):
                            length = struct.unpack_from('<H',record,pos)[0]; pos += 2
                            packets.append(record[pos:pos+length]); pos += length
                        expected = [bytes(r['wire']) for r in fixture['regions']]+[wire_patch]
                        assert pos == len(record) and sorted(packets) == sorted(expected), 'browser packet bytes changed'
                        if jitter['jitter_min_ms'] != jitter['jitter_max_ms']:
                            assert packets != expected, 'receiver did not reorder random-delay packets'
                    finally:
                        conn.close()
                    print(f"PASS: receive jitter {jitter}, deadlines, late recovery, MJPEG and raw browser packets")
                    return
                if playback is not None:
                    delay = playback['keyframe_wait_ms'] / 1000
                    first_at = time.monotonic()
                    region(0); drained()
                    if delay == 0:
                        assert frame_is(100), 'zero wait failed to publish first packet'
                    else:
                        assert status()['frame'] is None
                        time.sleep(delay*.55)
                        region(1); drained()
                        time.sleep(delay*.2)
                        assert status()['frame'] is None, 'configured assembly wait ended too early'
                        # The later real packet must not push the deadline back.
                        wait_for(lambda: frame_is(100), max(.01, first_at+delay+.08-time.monotonic()))
                    assert status()['decode']['frames'] == 1
                    region(2); drained()
                    assert status()['decode']['frames'] == 1, 'late packet replayed key'
                    patch(101, 100); wait_for(lambda: frame_is(101))
                    region(0, 110); patch(111, 110)
                    wait_for(lambda: frame_is(111), .2)
                    for i in range(len(fixture['regions'])):
                        region(i, 120)
                    drained()
                    assert frame_is(120), 'complete key did not bypass configured delay'
                    print(f"PASS: receiver playback {playback}, first-packet deadline, zero wait, "
                          'HTTP browser defaults, PATCH/completion bypass')
                    return
                samples = [s for s in fixture['concealment'] if s['mode'] == mode]
                region(0); drained()
                assert status()['frame'] is None, 'first region flashed a partial key'
                # Keep delivering duplicates across the first-packet deadline;
                # they cannot keep the reference hidden or create extra presentations.
                end = time.monotonic() + .18
                while time.monotonic() < end:
                    region(0); time.sleep(.015)
                state = frame_is(100)
                assert state, 'duplicate traffic extended the assembly deadline'
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
                      'deadline/PATCH/next-key/completion, duplicates, late recovery, streams, wrap')
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
    receiver = str(Path(sys.argv[1]).resolve())
    for mode in ('raw', 'nearest', 'smooth'):
        run(receiver, fixture, mode)
    run(receiver, fixture, 'smooth', live=True)
    for playback in ({'keyframe_wait_ms': 0, 'playout_ms': 0}, {'keyframe_wait_ms': 300, 'playout_ms': 120}):
        run(receiver, fixture, 'smooth', playback)
    for jitter in ({'jitter_min_ms':80,'jitter_max_ms':80,'jitter_seed':1},
                   {'jitter_min_ms':0,'jitter_max_ms':80,'jitter_seed':42}):
        run(receiver, fixture, 'smooth', {'keyframe_wait_ms':30,'playout_ms':40}, jitter)
    # Reject invalid timing settings before opening network listeners.
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / 'invalid.json'
        for playback in (None, {'keyframe_wait_ms': -1}, {'keyframe_wait_ms': 1001},
                         {'keyframe_wait_ms': 1.5}, {'keyframe_wait_ms': '100'},
                         {'keyframe_wait_ms': True}, {'playout_ms': 201}, {'playout_ms': -1}):
            path.write_text(json.dumps({'udp': {}, 'http': {}, 'playback': playback}))
            result = subprocess.run([receiver, str(path)], capture_output=True, text=True, timeout=5)
            assert result.returncode != 0 and 'playback' in result.stderr, 'invalid playback accepted'
        for udp in ({'jitter_min_ms':-1}, {'jitter_max_ms':1001}, {'jitter_min_ms':20,'jitter_max_ms':10},
                    {'jitter_max_ms':1.5}, {'jitter_max_ms':True}, {'jitter_seed':-1}, {'jitter_seed':4294967296}):
            path.write_text(json.dumps({'udp':udp,'http':{}}))
            result = subprocess.run([receiver,str(path)],capture_output=True,text=True,timeout=5)
            assert result.returncode != 0 and 'udp.jitter_' in result.stderr, 'invalid jitter config accepted'
