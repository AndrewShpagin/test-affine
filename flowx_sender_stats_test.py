#!/usr/bin/env python3
"""Compare sender JPEG/MTU statistics with real UDP datagrams, including loss=100%."""
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading


def run(sender, folder, strips, codec='jpeg', loss=0, mtu=None, host='127.0.0.1', fps=20):
    family = socket.AF_INET6 if ':' in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        sock.bind((host, 0))
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
        config = {'source': {'type': 'folder', 'path': str(folder), 'fps': fps, 'loop': False},
                  'codec': {'keyframe_bytes': 4000, 'keyframe_period': 5, 'grayscale': True,
                            'strips': strips, 'keyframe_codec': codec},
                  'udp': {'host': host, 'port': sock.getsockname()[1]}}
        if mtu is not None:
            config['udp']['mtu'] = mtu
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
        reports = [report for report in result.stdout.splitlines()
                   if report.startswith(('frames captured=', 'FlowX sender stopped:'))]
        if fps == 5:
            assert len(reports) > 1, 'periodic report not exercised'
        for report in reports:
            atomic = re.search(r'udp-atomic-limit=(\d+)B udp-over-atomic=([\d.]+)%', report)
            assert atomic, report
            limit, percent = int(atomic[1]), float(atomic[2])
            assert limit == (1500 if mtu is None else mtu) - (48 if family == socket.AF_INET6 else 28), report
            if loss == 100:
                # The fixture contains >1300-byte chunks, even though none reach UDP.
                assert 0 < percent < 100, report
            else:
                sent = int(re.search(r' packets=(\d+)', report)[1])
                assert sent > 0 and sent <= len(packets), (report, len(packets))
                received = packets[:sent]
                expected = 100 * sum(len(p) > limit for p in received) / len(received)
                assert abs(percent - expected) <= .0051, (report, expected)
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
        print(f'PASS: datagram stats, strips={strips}, codec={codec}, loss={loss}, host={host}: '
              f'{count}, {average}B, atomic={limit}B, over={percent}%')
        return packets


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
        packets = run(sender, folder, True)
        run(sender, folder, False)
        run(sender, folder, True, loss=100, mtu=1280)
        run(sender, folder, False, codec='jpeg2000')
        run(sender, folder, True, mtu=1280, fps=5)
        # Exactly at the threshold is atomic; one byte above must count.
        maximum = max(map(len, packets))
        boundary = run(sender, folder, True, mtu=maximum + 28)
        assert max(map(len, boundary)) == maximum, 'boundary fixture changed'
        run(sender, folder, True, mtu=maximum + 27)
        try:
            with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
                probe.bind(('::1', 0))
        except OSError:
            print('SKIP: IPv6 loopback unavailable')
        else:
            run(sender, folder, True, host='::1')


if __name__ == '__main__':
    main(sys.argv[1])
