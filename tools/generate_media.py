#!/usr/bin/env python3
"""Create a real encoded test movie; generated inputs are never a streaming mock."""
import argparse
import array
import math
import pathlib
import subprocess
import sys
import tempfile
import wave

parser = argparse.ArgumentParser()
parser.add_argument('output')
parser.add_argument('--size', default='1920x1080')
parser.add_argument('--seconds', type=int, default=40)
parser.add_argument('--raw', action='store_true', help='Use a portable raw-video/WAV input instead of lavfi')
args = parser.parse_args()
width, height = map(int, args.size.split('x'))
if width < 16 or height < 16 or width > 4096 or height > 4096 or width % 2 or height % 2 or not 1 <= args.seconds <= 300:
    parser.error('Use even dimensions 16..4096 and duration 1..300 seconds')
output = str(pathlib.Path(args.output).resolve())
probe = subprocess.run(['ffmpeg', '-hide_banner', '-demuxers'], text=True, capture_output=True, check=True)
lavfi = any('lavfi' in line.split() for line in (probe.stdout + probe.stderr).splitlines())
if lavfi and not args.raw:
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                    f'testsrc2=size={args.size}:rate=30', '-f', 'lavfi', '-i',
                    'sine=frequency=440:sample_rate=48000', '-t', str(args.seconds),
                    '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-y', output], check=True)
else:
    # vcpkg's minimal FFmpeg CLI can lack the lavfi input device. Feed actual
    # generated YUV frames and PCM audio into its encoders without avdevice.
    # Only the resulting host-side test movie persists; intermediates are temporary.
    with tempfile.TemporaryDirectory(prefix='party-media-') as temporary:
        audio = pathlib.Path(temporary) / 'tone.wav'
        samples = array.array('h', (int(10000 * math.sin(2 * math.pi * 440 * i / 48000))
                                   for i in range(args.seconds * 48000)))
        if sys.byteorder != 'little':
            samples.byteswap()
        with wave.open(str(audio), 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(48000)
            wav.writeframes(samples.tobytes())
        background = b''.join(bytes(40 + 180 * ((x // 32 + y // 32) % 2) for x in range(width))
                              for y in range(height))
        frame = bytearray(background + bytes([96]) * (width * height // 4) + bytes([160]) * (width * height // 4))
        side = min(64, width // 4, height // 4)
        previous = None
        with tempfile.TemporaryFile(mode='w+') as errors:
            process = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                '-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', args.size,
                '-framerate', '30', '-i', 'pipe:0', '-i', str(audio), '-t', str(args.seconds),
                '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
                '-c:a', 'aac', '-y', output], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                for i in range(args.seconds * 30):
                    if previous:
                        px, py = previous
                        for y in range(py, py + side):
                            start = y * width + px
                            frame[start:start + side] = background[start:start + side]
                    x, y = (i * 13) % (width - side + 1), (i * 7) % (height - side + 1)
                    row = bytes([235 if (i // 30) % 2 else 16]) * side
                    for line in range(y, y + side):
                        start = line * width + x
                        frame[start:start + side] = row
                    previous = (x, y)
                    process.stdin.write(frame)
            except BrokenPipeError:
                pass  # Report the encoder's actual error below.
            finally:
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
            code = process.wait()
            if code:
                errors.seek(0)
                raise RuntimeError('FFmpeg raw-media generation failed: ' + errors.read())
