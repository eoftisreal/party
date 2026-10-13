#!/usr/bin/env python3
"""Generate deterministic synthetic source media; this is a real encoded movie, not a streaming mock."""
import argparse
import subprocess
parser = argparse.ArgumentParser()
parser.add_argument('output')
parser.add_argument('--size', default='1920x1080')
parser.add_argument('--seconds', type=int, default=40)
args = parser.parse_args()
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                f'testsrc2=size={args.size}:rate=30', '-f', 'lavfi', '-i',
                'sine=frequency=440:sample_rate=48000', '-t', str(args.seconds),
                '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-y', args.output], check=True)
