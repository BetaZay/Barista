"""Decode both entropy paths and compare all visible reconstructed pixels."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    probe, ffmpeg = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="barista-cabac-") as directory:
        root = pathlib.Path(directory)
        cavlc = root / "cavlc.h264"
        cabac = root / "cabac.h264"
        subprocess.run([probe, str(cavlc), str(cabac)], check=True)
        decoded = []
        for source in (cavlc, cabac):
            output = source.with_suffix(".yuv")
            subprocess.run([
                ffmpeg, "-v", "error", "-xerror", "-i", str(source),
                "-vf", "crop=854:480:0:0", "-pix_fmt", "yuv420p",
                "-f", "rawvideo", str(output),
            ], check=True)
            decoded.append(output.read_bytes())
        expected = 300 * 854 * 480 * 3 // 2
        if any(len(data) != expected for data in decoded):
            raise RuntimeError("decoder did not produce exactly 300 complete frames")
        if decoded[0] != decoded[1]:
            offset = next(i for i, (a, b) in enumerate(zip(*decoded)) if a != b)
            raise RuntimeError(f"CABAC reconstruction differs at decoded byte {offset}")
        print("300 moving IDR/P frames match exactly, including frame-number wrap and consecutive IDRs")


if __name__ == "__main__":
    main()
