#!/usr/bin/env python3
"""Offline multi-frame DRH stress check; no radio or credentials required."""
import importlib.util
from pathlib import Path
import struct
import subprocess

spec = importlib.util.spec_from_file_location('reencode', Path(__file__).with_name('reencode-real-replay.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

encoder = Path(__file__).resolve().parent.parent/'build/drcd/drcd_reencode_replay'
worker = subprocess.Popen([str(encoder)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          env=r.encoder_settings())
start=b'\0\0\0\1'
annex=bytearray(start+bytes.fromhex('67640020ac2b406c1ef368')+start+bytes.fromhex('68ee060ce8'))
minimum=1<<30
maximum=0
number=0
try:
    for index in range(300):
        # Long flat runs exercise tiny CABAC outputs. Moving edges/checkerboard
        # and luma/chroma changes exercise intra, inter, carry and buffer growth.
        if index < 60:
            y=bytes([16 if index<30 else 235])*(864*480)
        else:
            row=bytes(32 if ((x+index*7)//16)%2 else 220 for x in range(864))
            y=b''.join(row if ((line+index)//16)%2 else row[::-1] for line in range(480))
        raw=y+bytes([96+(index%64)])*(864*480//4)+bytes([160-(index%64)])*(864*480//4)
        idr=index in (0,120,299)
        if idr: number=0
        worker.stdin.write(bytes([idr])+raw); worker.stdin.flush()
        chunks=[]
        for chunk in range(5):
            size,=struct.unpack('<I',r.exact(worker.stdout,4))
            assert 0<size<4*1024*1024
            minimum=min(minimum,size); maximum=max(maximum,size)
            chunks.append(r.exact(worker.stdout,size))
        header=0x25b804ff if idr else 0x21e003ff|((number&255)<<13)
        annex.extend(start+r.reconstruct.escape(header.to_bytes(4,'big')+b''.join(chunks)))
        number+=1
    worker.stdin.close()
    assert worker.wait()==0
    subprocess.run(['ffmpeg','-v','error','-xerror','-err_detect','explode','-f','h264',
                    '-i','pipe:0','-f','null','-'],input=annex,check=True)
    print(f'300 frames / 1500 nonempty chunks / forced IDRs and P chains decoded; chunk bytes={minimum}..{maximum}')
finally:
    if worker.poll() is None: worker.terminate()
    worker.wait()
    worker.stdout.close()
