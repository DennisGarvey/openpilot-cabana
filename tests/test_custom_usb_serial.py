#!/usr/bin/env python3
"""Feed fragmented USB records through a PTY into the real Cabana stream."""
import binascii
import os
from pathlib import Path
import pty
import select
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


def decode(wire):
    raw = bytearray()
    while wire:
        code = wire[0]
        assert 0 < code <= len(wire)
        raw.extend(wire[1:code])
        wire = wire[code:]
        if wire and code != 255:
            raw.append(0)
    assert binascii.crc_hqx(raw[:-2], 0xffff) == struct.unpack('<H', raw[-2:])[0]
    return raw[:-2]


def frame(bus, sequence, timestamp, address, data, flags=0, boot=123, kind=0):
    return encode(struct.pack('<BBBBIIQIB', 1, kind, bus, flags, boot, sequence, timestamp, address, len(data)) + data)


def read_record(master, pending):
    deadline = time.monotonic() + 2
    while True:
        while b'\0' in pending:
            record, _, remaining = pending.partition(b'\0')
            pending[:] = remaining
            if record:
                return decode(record)
        ready, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
        assert ready, 'No USB transmit request'
        pending.extend(os.read(master, 256))


def tx_result(master, kind, bus, sequence, code):
    os.write(master, encode(struct.pack('<BBBBIIQB', 1, kind, bus, 0, 124, sequence, 102, code)))


def main():
    root = Path(__file__).resolve().parents[1]
    # Compare the wire implementation with the sibling firmware when available.
    for firmware_header in root.parent.glob('*/main/capture/capture_protocol.h'):
        common_code = lambda data: data[data.index(b'#define DK_VERSION'):]
        assert common_code(firmware_header.read_bytes()) == common_code(
            (root / 'src/core/streams/custom_usb_serial_protocol.h').read_bytes())
    feedback = os.environ.get('CABANA_TEST_TX_FEEDBACK', 'ack')
    master, slave = pty.openpty()
    with tempfile.TemporaryDirectory(prefix='custom-usb-serial-', dir=root / 'build') as directory:
        process = subprocess.Popen([str(root / 'build/test_custom_usb_serial_stream'), os.ttyname(slave), directory],
                                   env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen', 'CABANA_TEST_TX_FEEDBACK': feedback},
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # QSerialPort configures the PTY to raw mode when open. Wait for
            # that observable state rather than racing application startup.
            deadline = time.monotonic() + 5
            while termios.tcgetattr(slave)[3] & termios.ICANON:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError('Serial reader did not open the PTY')
                time.sleep(.01)
            pending = bytearray()
            start_record = read_record(master, pending)
            version, kind, bus, flags, boot, sequence, session, opcode = struct.unpack('<BBBBIIQB', start_record)
            assert (version, kind, bus, flags, boot, sequence, opcode) == (1, 12, 0, 0, 0, 0, 0) and session

            def session_record(opcode, session_id=session, boot_id=0):
                return struct.pack('<BBBBIIQB', 1, 12, 0, 0, boot_id, 0, session_id, opcode)

            keepalive = session_record(2)
            saw_keepalive = False
            def read_tx_record():
                nonlocal saw_keepalive
                while (record := read_record(master, pending)) == keepalive:
                    saw_keepalive = True
                return record

            stale = frame(0, 99, 10, 0x777, bytes([255]))
            os.write(master, stale + encode(session_record(1, session ^ 1, 123)) + stale +
                     encode(session_record(1, boot_id=123)))
            stream = b'boot text\0' + b'\x01' * 200 + b'\0'
            stream += frame(0, 0xfffffffe, 1000, 0x132, b'12345678')
            stream += frame(1, 1, 1100, 0x1abcde, bytes(range(64)), 13)
            stream += frame(0, 0xffffffff, 1200, 0x200, b'\0' * 8, 2)
            stream += frame(0, 0, 1300, 0x201, b'\xff')
            stream += frame(0, 2, 1400, 0x202, b'\0\xff')
            stream += encode(struct.pack('<BBBBIIQIII', 1, 1, 0, 0, 123, 2, 1500, 1, 2, 3))
            stream += frame(0, 1, 100, 0x203, b'\x01', boot=124)
            stream += frame(1, 1, 110, 0x22222, b'\xa5' * 64, 13, boot=124)
            stream += frame(1, 2, 120, 0x22222, b'\xa5' * 63 + b'\xa4', 13, boot=124)
            stream += encode(struct.pack('<BBBBIIQ6H', 1, 9, 0, 0, 124, 0, 130, 123, 234, 345, 456, 567, 678))
            # Exercise arbitrary serial boundaries, including a one-byte read.
            start = 0
            for size in (1, 7, 2, 29, 1, 64, 3, 5, 200, len(stream)):
                chunk = stream[start:start + size]
                if chunk:
                    os.write(master, chunk)
                    start += len(chunk)
                    time.sleep(.005)
            repeat = os.environ.get('CABANA_TEST_TX_REPEAT')
            automatic = bool(repeat and repeat.startswith('automatic'))
            tx_result(master, 3, 1, 0, 0)
            request = read_tx_record()
            version, kind, bus, flags, boot, sequence, timestamp, address, length = struct.unpack(
                '<BBBBIIQIB', request[:25])
            expected_kind = 7 if automatic else (4 if repeat else 2)
            assert (version, kind, bus, flags, boot, address, length) == (1, expected_kind, 1, 13, 124, 0x22222, 64)
            assert sequence and request[25:89] == b'\x12\x35' + b'\xa5' * 62
            if repeat:
                expected_mask = b'\xff\xf0' + b'\0' * 61 + (b'\0' if feedback else b'\x01')
                assert request[89:153] == expected_mask
                assert request[153:] == (b'\0' * 63 + b'\x01' if feedback else b'')
                assert (timestamp & 0xffffffff, timestamp >> 32) == (0 if automatic else (500 if feedback else 5), 1000)
                assert kind == (7 if automatic else 4)
                result_kind = 8 if automatic else 6
                tx_result(master, result_kind, 1, sequence, 7)
                if feedback == 'ack':
                    os.write(master, frame(1, sequence, 140, 0x22222, request[25:89], flags=13, boot=124, kind=11))
                    tx_result(master, 10, 1, sequence, 1)
                    os.write(master, frame(1, sequence, 150, 0x22222, b'4' + request[26:89],
                                           flags=13, boot=124, kind=11))
                    tx_result(master, 10, 1, sequence, 1)
                control = lambda opcode: struct.pack('<BBBBIIQB', 1, 5, 1, 0, 124, sequence, 0, opcode)
                assert read_tx_record() == control(0)
                if repeat.endswith('cancel'):
                    while (record := read_tx_record()) == control(0):
                        pass
                    assert record == control(1)
                    tx_result(master, result_kind, 1, sequence, 9)
                else:
                    assert read_tx_record() == control(0)
                    tx_result(master, result_kind, 1, sequence, 8)
                assert not pending.strip(b'\0')
            else:
                assert not pending.strip(b'\0'), 'Unexpected second TX request'
                tx_result(master, 3, 1, sequence, 4 if 'CABANA_TEST_TX_FIFO_FULL' in os.environ else 1)
                if feedback:
                    tx_result(master, 10, 0, sequence, 3)  # Same sequence on the wrong bus.
                    time.sleep(.25)
                    if feedback == 'unconfirmed':
                        os.write(master, encode(struct.pack('<BBBBIIQBIII', 1, 10, 1, 0, 124, sequence,
                                                            102, 2, 0x30, 1 << 18, 8 << 8)))
                    else:
                        if feedback == 'ack':
                            os.write(master, frame(1, sequence, 140, 0x22222, request[25:89], flags=13,
                                                   boot=124, kind=11))
                        tx_result(master, 10, 1, sequence, {'ack': 1, 'unknown': 3}[feedback])
            assert read_tx_record() == session_record(3), 'Expected stop on stream close'
            assert saw_keepalive, 'Expected session keepalive'
            stdout, stderr = process.communicate(timeout=8)
            print(stdout.decode())
            if process.returncode:
                raise AssertionError(f'exit={process.returncode}: {stderr.decode()}')
            assert not list(Path(directory).iterdir()), 'Live USB must not write recording files'
            if repeat:
                label = 'Automatic' if automatic else 'Fixed-rate'
                print(f'{label} integration: signal mask, timing, keepalive, and {repeat} passed')
            print('USB transmission integration: invalid length rejected, one outstanding request, multiplexed 64-byte FD payload and status passed')
            print('Serial integration: both buses, FD, RTR, fragmentation, corruption, sequence wrap/gap, reboot, no disk recording passed')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    os.close(master)
    os.close(slave)


if __name__ == '__main__':
    main()
