#!/usr/bin/env python3
"""Offline A/B: decode a real replay and replace only its video with drcd encoding."""
import argparse
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('reconstruct', Path(__file__).with_name('reconstruct-media-capture.py'))
reconstruct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reconstruct)


def exact(stream, count):
    data = bytearray()
    while len(data) < count:
        part = stream.read(count-len(data))
        if not part:
            raise ValueError('Truncated replay/decoder/encoder output')
        data.extend(part)
    return bytes(data)


def read_replay(path):
    with open(path, 'rb') as stream:
        header = exact(stream, 20)
        if header[:8] != b'DRCREP01': raise ValueError('Invalid replay magic')
        count, _, _ = struct.unpack('<III', header[8:])
        if not 0 < count <= 100000: raise ValueError('Invalid record count')
        records = []
        for _ in range(count):
            t, kind, size = struct.unpack('<IBH', exact(stream, 7))
            if kind > 2 or not 0 < size <= 2063: raise ValueError('Invalid record')
            records.append((t, kind, exact(stream, size)))
        if stream.read(1): raise ValueError('Trailing data')
    return header, records


def split_frames(records):
    frames = []
    chunks = None
    for record in records:
        t, kind, p = record
        if kind: continue
        if len(p) < 17: raise ValueError('Short video packet')
        if p[2] & 64:
            if chunks is not None: raise ValueError('Incomplete frame')
            chunks = [[]]
        if chunks is None: raise ValueError('Missing frame start')
        chunks[-1].append(record)
        if p[2] & 16:
            if len(chunks) != 5 or not p[2] & 32: raise ValueError('Invalid chunk count')
            frames.append(chunks)
            chunks = None
        elif p[2] & 32:
            chunks.append([])
    if chunks is not None or not frames: raise ValueError('Incomplete clip')
    if 128 not in frames[0][0][0][2][8:16]: raise ValueError('Clip needs initial IDR')
    return frames


def packetize(chunk, original, chunk_index, sequence):
    # Retain original packet time slots wherever possible. Different compressed
    # sizes may need more slots; interpolate within the same chunk time window.
    count = min(len(chunk), max(len(original), (len(chunk)+1693)//1694))
    result = []
    for i in range(count):
        position = i*(len(original)-1)/(count-1) if count > 1 else 0
        low = int(position)
        high = min(low+1, len(original)-1)
        t = round(original[low][0] + (original[high][0]-original[low][0])*(position-low))
        part = chunk[i*len(chunk)//count:(i+1)*len(chunk)//count]
        p = bytearray(original[0][2][:16])
        p[0] = (p[0]&252) | ((sequence>>8)&3); p[1] = sequence&255
        p[2] = (p[2]&0x88) | (0x40 if chunk_index==0 and i==0 else 0) | \
               (0x20 if i==count-1 else 0) | (0x10 if chunk_index==4 and i==count-1 else 0) | (len(part)>>8)
        p[3] = len(part)&255
        result.append((t, 0, bytes(p)+part))
        sequence = (sequence+1)&1023
    return result, sequence


def encoder_settings(intra_refresh=True):
    return {**os.environ, 'DRCD_VIDEO_QP':'32', 'DRCD_FAST_ENCODE':'0',
            'DRCD_INTRA_REFRESH':'1' if intra_refresh else '0'}


def convert(source, destination, encoder, intra_refresh=True):
    header, records = read_replay(source)
    frames = split_frames(records)
    output = [r for r in records if r[1] != 0]
    changed_slots = 0
    with tempfile.TemporaryFile() as annex:
        start = b'\0\0\0\1'
        annex.write(start+bytes.fromhex('67640020ac2b406c1ef368')+start+bytes.fromhex('68ee060ce8'))
        number = 0
        for chunks in frames:
            idr = 128 in chunks[0][0][2][8:16]
            if idr: number = 0
            slice_header = 0x25b804ff if idr else 0x21e003ff | ((number&255)<<13)
            payload = b''.join(r[2][16:] for c in chunks for r in c)
            annex.write(start+reconstruct.escape(slice_header.to_bytes(4,'big')+payload))
            number += 1
        annex.seek(0)
        # Disable SPS cropping: the transport encoder needs all 864 columns,
        # including the ten normally hidden columns. Do not stretch 854 to 864.
        decoder = subprocess.Popen(['ffmpeg','-nostdin','-v','error','-apply_cropping','0',
            '-f','h264','-i','pipe:0','-pix_fmt','yuv420p','-f','rawvideo','pipe:1'], stdin=annex, stdout=subprocess.PIPE)
        settings = encoder_settings(intra_refresh)
        worker = subprocess.Popen([str(encoder)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=settings)
        try:
            sequence = 0
            for chunks in frames:
                raw = exact(decoder.stdout, 864*480*3//2)
                idr = 128 in chunks[0][0][2][8:16]
                worker.stdin.write(bytes([idr])+raw); worker.stdin.flush()
                for i, original in enumerate(chunks):
                    size, = struct.unpack('<I', exact(worker.stdout, 4))
                    if not 0 < size <= 4*1024*1024: raise ValueError('Invalid encoded chunk size')
                    encoded = exact(worker.stdout, size)
                    packets, sequence = packetize(encoded, original, i, sequence)
                    changed_slots += len(packets) != len(original)
                    output.extend(packets)
            worker.stdin.close()
            if decoder.stdout.read(1): raise ValueError('Decoded frame count/dimensions differ')
            if decoder.wait() or worker.wait(): raise ValueError('Decoder/encoder failed')
        finally:
            for process in (decoder, worker):
                if process.poll() is None: process.terminate()
                process.wait()
            decoder.stdout.close(); worker.stdout.close()
    output.sort(key=lambda r: r[0])
    # Exclusive output: never overwrite the known-good source or previous trial.
    with open(destination, 'xb') as stream:
        stream.write(header[:8]+struct.pack('<I', len(output))+header[12:])
        for t, kind, p in output:
            stream.write(struct.pack('<IBH', t, kind, len(p))+p)
    return {'frames':len(frames), 'video_packets':sum(r[1]==0 for r in output),
            'chunks_with_changed_packet_count':changed_slots, 'total_chunks':len(frames)*5,
            'audio_and_format':'unchanged', 'encoder':'QP32, slow', 'intra_refresh':intra_refresh,
            'automatic_keyframes':'suppressed offline when intra-refresh is off; captured frame types retained'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source'); parser.add_argument('output')
    parser.add_argument('--encoder', type=Path, default=Path(__file__).resolve().parent.parent/'build/app/drcd_reencode_replay')
    parser.add_argument('--no-intra-refresh', action='store_true',
                        help='Disable only cyclic intra-refresh; retain captured IDR/P decisions')
    args = parser.parse_args()
    print(convert(args.source, args.output, args.encoder, not args.no_intra_refresh))
