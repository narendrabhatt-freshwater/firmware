#!/usr/bin/env python3
"""Measure Channel Card protocol-v3 continuous receive throughput (requires pyserial).

Uses the diagnostic sink, leaves samples/voices unchanged, and verifies one
ordered reply after the timed stream. This is not a playback qualification.
"""
import argparse
import datetime
import json
import statistics
import struct
import time

import serial


def frame(kind, payload=b''):
    return struct.pack('<BBBH', kind, 0, 0, len(payload)) + payload


def read_exact(port, size):
    result = bytearray()
    while len(result) < size:
        data = port.read(size - len(result))
        if not data:
            raise RuntimeError(f'Timeout: received {len(result)}/{size} bytes')
        result.extend(data)
    return bytes(result)


def reply(port, request):
    kind, target, req, size = struct.unpack('<BBBH', read_exact(port, 5))
    if (kind, target, req) != (6, 0, request) or size > 128:
        raise RuntimeError(f'Unexpected reply: {(kind, target, req, size)}')
    data = read_exact(port, size)
    if not data or data[0]:
        raise RuntimeError(f'Device rejected request: {data!r}')
    return data


def connect(port):
    port.dtr = False
    time.sleep(0.1)
    port.reset_input_buffer()
    port.dtr = True
    time.sleep(0.1)
    port.write(frame(1, bytes([3])))
    caps = reply(port, 1)
    if len(caps) != 14 or caps[1] != 3 or struct.unpack_from('<H', caps, 8)[0] != 1024:
        raise RuntimeError(f'Incompatible capabilities: {caps.hex()}')
    return caps.hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Channel Card CDC serial port')
    parser.add_argument('--mib', type=int, default=4, help='payload MiB per run')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--batch-blocks', type=int, nargs='+', default=[8, 32, 128])
    parser.add_argument('--output', required=True, help='JSON measurement log')
    args = parser.parse_args()
    if not 1 <= args.mib <= 256 or args.repeats < 1 or any(n < 1 for n in args.batch_blocks):
        parser.error('mib must be 1..256; repeats and batch sizes must be positive')
    from serial.tools import list_ports
    device = next((p for p in list_ports.comports() if p.device == args.port), None)
    if device is None or (device.vid, device.pid) != (0xCAFE, 0x4032):
        parser.error('port is not a connected Channel Card (cafe:4032)')
    payload = bytes(range(256)) * 4
    block = frame(7, payload)
    total_blocks = args.mib * 1024
    if any(total_blocks % n for n in args.batch_blocks):
        parser.error("each batch size must divide the total block count")
    expected_hash = 2166136261
    for value in payload * total_blocks:
        expected_hash = ((expected_hash ^ value) * 16777619) & 0xffffffff
    results = {
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'port': args.port,
        'serial_number': device.serial_number,
        'protocol': 3,
        'direction': 'host to card',
        'method': 'Continuous 1024-byte PROBE messages, no intermediate replies; one final ordered barrier verifies receipt, block count and FNV-1a hash. Timing includes final reply.',
        'load': 'Existing board state; no notes started, no RS485 polling added. Diagnostic sink, not BODY/voice playback.',
        'runs': [],
    }
    output_path = args.output
    with serial.Serial(results['port'], 115200, timeout=3, write_timeout=5, exclusive=True) as port:
        try:
            for batch in args.batch_blocks:
                packet = block * batch
                for repeat in range(1, args.repeats + 1):
                    results['capabilities_hex'] = connect(port)
                    start = time.perf_counter()
                    for _ in range(0, total_blocks, batch):
                        if port.write(packet) != len(packet):
                            raise RuntimeError('Incomplete host write')
                    if port.write(frame(7)) != 5:
                        raise RuntimeError('Incomplete barrier write')
                    stats = reply(port, 7)
                    elapsed = time.perf_counter() - start
                    if len(stats) != 13:
                        raise RuntimeError(f'Invalid probe statistics: {stats.hex()}')
                    got_bytes, got_blocks, got_hash = struct.unpack_from('<III', stats, 1)
                    if (got_bytes, got_blocks, got_hash) != (total_blocks * 1024, total_blocks, expected_hash):
                        raise RuntimeError(f'Integrity failure: {stats.hex()}')
                    row = {
                        'write_batch_blocks': batch, 'repeat': repeat,
                        'payload_bytes': got_bytes, 'elapsed_s': elapsed,
                        'payload_kB_s': got_bytes / elapsed / 1000,
                        'protocol_out_kB_s': (total_blocks * len(block) + 5) / elapsed / 1000,
                        'payload_Mbps': got_bytes * 8 / elapsed / 1e6,
                        'fnv1a': f'{got_hash:08x}', 'integrity_pass': True,
                    }
                    results['runs'].append(row)
                    with open(output_path, 'w') as output:
                        json.dump(results, output, indent=2)
                    print(json.dumps(row), flush=True)
        finally:
            port.dtr = False
    for batch in args.batch_blocks:
        values = [r['payload_kB_s'] for r in results['runs'] if r['write_batch_blocks'] == batch]
        print(f'{batch} blocks/write: median {statistics.median(values):.1f} kB/s; range {min(values):.1f}-{max(values):.1f}', flush=True)
    print(f'Results: {output_path}', flush=True)


if __name__ == '__main__':
    main()
