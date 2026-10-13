#!/usr/bin/env python3
"""Run the real peer test in an empty directory and report resources/files it created."""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile
import time
try:
    import resource
except ImportError:
    resource = None
parser = argparse.ArgumentParser()
parser.add_argument('executable')
parser.add_argument('movie')
parser.add_argument('--impair', action='store_true')
parser.add_argument('--window', action='store_true')
parser.add_argument('--hardware', action='store_true')
args = parser.parse_args()
exe, movie = pathlib.Path(args.executable).resolve(), pathlib.Path(args.movie).resolve()
env = dict(os.environ, SDL_AUDIODRIVER='dummy')
if args.impair:
    env['PARTY_TEST_IMPAIR'] = '1'
if args.window:
    env.update(PARTY_TEST_WINDOW='1', SDL_VIDEODRIVER='dummy')
if args.hardware:
    env['PARTY_TEST_HARDWARE'] = '1'
start = time.monotonic()
with tempfile.TemporaryDirectory(prefix='party-viewer-test-') as workdir:
    result = subprocess.run([str(exe), str(movie)], cwd=workdir, env=env, text=True, capture_output=True)
    print(result.stdout)
    if result.returncode:
        print(result.stderr)
    files = list(pathlib.Path(workdir).rglob('*'))
    report = {'wallSeconds': round(time.monotonic()-start, 3), 'exitCode': result.returncode,
              'viewerWorkingDirectoryFiles': [str(p.relative_to(workdir)) for p in files]}
    if resource:
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        report.update(userCpuSeconds=round(usage.ru_utime, 3), systemCpuSeconds=round(usage.ru_stime, 3),
                      peakRssKiB=usage.ru_maxrss)
    print(json.dumps(report, indent=2))
    if files:
        raise SystemExit('Unexpected viewer working-directory writes')
    raise SystemExit(result.returncode)
