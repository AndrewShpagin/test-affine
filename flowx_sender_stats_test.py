#!/usr/bin/env python3
"""Compare sender JPEG statistics with real UDP datagrams, including loss=100%."""
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading


def run(sender, folder, strips, codec='jpeg', loss=0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        sock.settimeout(.1)
        packets, stopped = [], threading.Event()

        def collect():
            while not stopped.is_set():
                try:
                    packets.append(sock.recv(65536))
                except socket.timeout:
                    pass

        collector = threading.Thread(target=collect)
        collector.start()
        config = {'source': {'type': 'folder', 'path': str(folder), 'fps': 20, 'loop': False},
                  'codec': {'keyframe_bytes': 4000, 'keyframe_period': 5, 'grayscale': True,
                            'strips': strips, 'keyframe_codec': codec},
                  'udp': {'host': '127.0.0.1', 'port': sock.getsockname()[1]}}
        path = folder.parent / 'sender.json'
        path.write_text(json.dumps(config))
        try:
            result = subprocess.run([sender, str(path), '--loss-percent', str(loss)],
                                    capture_output=True, text=True, timeout=15)
            # Drain the final datagrams before shutting down the collecting thread.
            stopped.wait(.15)
        finally:
            stopped.set()
            collector.join(timeout=2)
        assert result.returncode == 0, result.stdout + result.stderr
        line = next(line for line in result.stdout.splitlines() if line.startswith('FlowX sender stopped:'))
        match = re.search(r'jpeg-chunks=(\d+) jpeg-chunk-avg=([\d.]+)B', line)
        assert match, line
        count, average = int(match[1]), float(match[2])
        sizes = re.search(r'jpeg-chunk-max=(\d+)B jpeg-chunk-over-target=(\d+) jpeg-hard-dropped=(\d+)', line)
        assert sizes, line
        maximum, overshoots, hard_drops = map(int, sizes.groups())
        assert hard_drops == 0, line
        if loss == 100:
            assert not packets and count > 0 and 0 < average <= maximum <= 65507, line
            assert overshoots > 0, 'fixture never exceeded soft target'
        elif codec != 'jpeg':
            assert packets and count == 0 and average == maximum == overshoots == 0, line
        else:
            chunks = [p for p in packets if (p[2] & 15) in (1, 4)]
            assert any((p[2] & 15) == 2 for p in packets), 'test did not produce PATCH datagrams'
            assert count == len(chunks) and count > 0, (line, len(chunks))
            assert abs(average - sum(map(len, chunks)) / len(chunks)) <= .051, line
            assert maximum == max(map(len, chunks)), line
            assert overshoots == sum(len(p) > 1300 for p in chunks), line
            if strips:
                assert overshoots > 0, 'fixture never exceeded soft target'
        print(f'PASS: JPEG datagram mean, strips={strips}, codec={codec}, loss={loss}: {count}, {average}B')


def main(sender):
    sender = str(Path(sender).resolve())
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp) / 'images'
        folder.mkdir()
        width, height = 160, 96
        pixels = bytes((x * 37 + y * 53 + (x ^ y) * 11) % 256
                       for y in range(height) for x in range(width) for _ in range(3))
        bmp = (b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54) +
               struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
        for i in range(16):
            (folder / f'{i:03}.bmp').write_bytes(bmp)
        run(sender, folder, True)
        run(sender, folder, False)
        run(sender, folder, True, loss=100)
        run(sender, folder, False, codec='jpeg2000')


if __name__ == '__main__':
    main(sys.argv[1])
