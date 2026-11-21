#!/usr/bin/env python3
"""Feed fragmented USB records through a PTY into the real Cabana stream."""
import binascii
import os
from pathlib import Path
import pty
import struct
import subprocess
import tempfile
import termios
import time


def encode(raw):
    raw += struct.pack('<H', binascii.crc_hqx(raw, 0xffff))
    out = bytearray(b'\0')
    code_at, code = 0, 1
    for byte in raw:
        if byte == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
    out[code_at] = code
    return bytes(out) + b'\0'


def frame(bus, sequence, timestamp, address, data, flags=0, boot=123):
    return encode(struct.pack('<BBBBIIQIB', 1, 0, bus, flags, boot, sequence, timestamp, address, len(data)) + data)


def main():
    root = Path(__file__).resolve().parents[1]
    # Compare the wire implementation with the sibling firmware when available.
    for firmware_header in root.parent.glob('*/main/capture/capture_protocol.h'):
        common_code = lambda data: data[data.index(b'#define DK_VERSION'):]
        assert common_code(firmware_header.read_bytes()) == common_code(
            (root / 'src/core/streams/custom_usb_serial_protocol.h').read_bytes())
    master, slave = pty.openpty()
    with tempfile.TemporaryDirectory(prefix='custom-usb-serial-', dir=root / 'build') as directory:
        process = subprocess.Popen([str(root / 'build/test_custom_usb_serial_stream'), os.ttyname(slave), directory],
                                   env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'},
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # QSerialPort configures the PTY to raw mode when open. Wait for
            # that observable state rather than racing application startup.
            deadline = time.monotonic() + 5
            while termios.tcgetattr(slave)[3] & termios.ICANON:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError('Serial reader did not open the PTY')
                time.sleep(.01)
            stream = b'boot text\0' + b'\x01' * 200 + b'\0'
            stream += frame(0, 0xfffffffe, 1000, 0x132, b'12345678')
            stream += frame(1, 1, 1100, 0x1abcde, bytes(range(64)), 13)
            stream += frame(0, 0xffffffff, 1200, 0x200, b'\0' * 8, 2)
            stream += frame(0, 0, 1300, 0x201, b'\xff')
            stream += frame(0, 2, 1400, 0x202, b'\0\xff')
            stream += encode(struct.pack('<BBBBIIQIII', 1, 1, 0, 0, 123, 2, 1500, 1, 2, 3))
            stream += frame(0, 1, 100, 0x203, b'\x01', boot=124)
            # Exercise arbitrary serial boundaries, including a one-byte read.
            start = 0
            for size in (1, 7, 2, 29, 1, 64, 3, 5, 200, len(stream)):
                chunk = stream[start:start + size]
                if chunk:
                    os.write(master, chunk)
                    start += len(chunk)
                    time.sleep(.005)
            stdout, stderr = process.communicate(timeout=8)
            print(stdout.decode())
            if process.returncode:
                raise AssertionError(f'exit={process.returncode}: {stderr.decode()}')
            assert not list(Path(directory).iterdir()), 'Live USB must not write recording files'
            print('Serial integration: both buses, FD, RTR, fragmentation, corruption, sequence wrap/gap, reboot, no disk recording passed')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    os.close(master)
    os.close(slave)


if __name__ == '__main__':
    main()
