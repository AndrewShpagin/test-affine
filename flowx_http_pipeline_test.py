#!/usr/bin/env python3
"""Exercise real HTTP snapshot -> sender -> UDP -> receiver -> JPEG, stdlib only."""
import http.client
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Snapshot(BaseHTTPRequestHandler):
    requests = 0

    def do_GET(self):
        if self.path != '/current.bmp':
            self.send_error(404)
            return
        Snapshot.requests += 1
        width, height = 160, 96
        # Textured frames translated one pixel per fetch, with aligned BMP rows.
        shift = Snapshot.requests % 8
        pixels = bytes(((x + shift) * 37 + y * 53 + ((x + shift) ^ y) * 11) % 256
                       for y in range(height) for x in range(width) for _ in range(3))
        bmp = (b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54) +
               struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0,
                           len(pixels), 0, 0, 0, 0) + pixels)
        self.send_response(200)
        self.send_header('Content-Type', 'image/bmp')
        self.send_header('Content-Length', str(len(bmp)))
        self.end_headers()
        self.wfile.write(bmp)

    def log_message(self, *_):
        pass


def main(sender, receiver):
    sender, receiver = str(Path(sender).resolve()), str(Path(receiver).resolve())
    udp_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    template = Path(__file__).parent / 'config' / 'flowx_sender_windows.json'
    sender_config = json.loads(template.read_text())
    with HTTPServer(('127.0.0.1', 0), Snapshot) as source, tempfile.TemporaryDirectory() as tmp:
        worker = threading.Thread(target=source.serve_forever, daemon=True)
        worker.start()
        sender_config['source']['url'] = f'http://127.0.0.1:{source.server_port}/current.bmp'
        sender_config['udp'] = {'host': '127.0.0.1', 'port': udp_port}
        sender_config['control']['enabled'] = False
        receiver_config = {'udp': {'bind': '127.0.0.1', 'port': udp_port},
                           'http': {'bind': '127.0.0.1', 'port': http_port}}
        processes, logs = [], []

        def launch(name, exe, config):
            path = Path(tmp) / (name + '.json')
            path.write_text(json.dumps(config))
            log = (Path(tmp) / (name + '.log')).open('w+')
            logs.append((name, log))
            processes.append(subprocess.Popen([exe, str(path)], stdout=log, stderr=log))

        def request(path):
            conn = http.client.HTTPConnection('127.0.0.1', http_port, timeout=2)
            try:
                conn.request('GET', path)
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()

        def wait_for(fn):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                assert all(p.poll() is None for p in processes), 'sender/receiver exited early'
                try:
                    result = fn()
                    if result:
                        return result
                except OSError:
                    pass
                time.sleep(.03)
            raise AssertionError('HTTP-to-UDP pipeline did not publish frames')

        try:
            launch('receiver', receiver, receiver_config)
            wait_for(lambda: request('/status.json')[0] == 200)
            launch('sender', sender, sender_config)

            def ready():
                code, body = request('/status.json')
                assert code == 200
                state = json.loads(body)
                return state if state['decode']['frames'] >= 5 else None

            state = wait_for(ready)
            assert Snapshot.requests >= 5
            assert state['udp']['datagrams'] > 0 and state['udp']['invalid'] == 0
            assert state['decode']['keyframes'] > 0 and state['decode']['patches'] > 0
            assert (state['frame']['width'], state['frame']['height']) == (160, 96)
            code, jpeg = request('/frame.jpg')
            assert code == 200 and jpeg.startswith(b'\xff\xd8') and jpeg.endswith(b'\xff\xd9')
            print('PASS: HTTP snapshots -> real sender -> localhost UDP -> receiver JPEG; '
                  f"{state['decode']['frames']} frames, no invalid datagrams")
        except Exception:
            for name, log in logs:
                log.flush()
                log.seek(0)
                print(name + ' log:\n' + log.read(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            source.shutdown()
            worker.join(timeout=2)
            for _, log in logs:
                log.close()


if __name__ == '__main__':
    main(*sys.argv[1:])
