#!/usr/bin/env python3
"""Actual host/viewer executable test, separate processes, with local TLS fixture coordination.
This deliberately does not claim Appwrite Cloud, production JWT or WAN verification.
"""
import argparse
import json
import os
import pathlib
import queue
import secrets
import ssl
import urllib.request
import shutil
import subprocess
import tempfile
import threading
import time
parser=argparse.ArgumentParser()
parser.add_argument('executable');parser.add_argument('movie');parser.add_argument('--report')
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
exe=pathlib.Path(args.executable).resolve();movie=pathlib.Path(args.movie).resolve()
class App:
    def __init__(self,argv,env,folder):
        self.messages=queue.Queue();self.stats=[];self.other=[]
        self.process=subprocess.Popen(argv,env=env,cwd=folder,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
        self.errors=[]
        threading.Thread(target=self.read,args=(self.process.stdout,False),daemon=True).start()
        threading.Thread(target=self.read,args=(self.process.stderr,True),daemon=True).start()
    def read(self,pipe,error):
        for line in pipe:
            if error:self.errors.append(line.rstrip());continue
            try:
                value=json.loads(line)
                if 'player' in value or 'media' in value:self.stats.append(value)
            except ValueError:pass
            self.messages.put(line.rstrip())
    def command(self,text):
        self.process.stdin.write(text+'\n');self.process.stdin.flush()
    def wait(self,predicate,seconds=40):
        start=time.monotonic()
        while time.monotonic()-start<seconds:
            if predicate():return
            if self.process.poll() is not None:raise RuntimeError('Application exited early: '+ '\n'.join(self.errors[-6:]))
            time.sleep(.05)
        raise TimeoutError('Application condition did not complete: '+ '\n'.join(self.errors[-6:]))
    def line(self,prefix):
        lines=[];start=time.monotonic()
        while time.monotonic()-start<30:
            try:line=self.messages.get(timeout=.1)
            except queue.Empty:continue
            if line.startswith(prefix):return line[len(prefix):]
            lines.append(line)
        raise TimeoutError('No startup line: '+prefix)
    def latest(self,key,field,default=0):
        return self.stats[-1].get(key,{}).get(field,default) if self.stats else default
    def finish(self):
        if self.process.poll() is None:
            try:self.command('quit');self.process.wait(timeout=15)
            except (BrokenPipeError,subprocess.TimeoutExpired):self.process.kill();self.process.wait()
start=time.monotonic();report={'coordination':'local TLS fixture (not Appwrite Cloud)','network':'same-machine UDP/DTLS/SRTP','separateProcesses':True}
apps=[];server=None
with tempfile.TemporaryDirectory(prefix='party-processes-') as temporary:
    work=pathlib.Path(temporary);key=work/'key.pem';cert=work/'cert.pem'
    openssl=shutil.which('openssl') or str(pathlib.Path(os.environ.get('ProgramFiles','C:/Program Files'))/'Git/usr/bin/openssl.exe')
    subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),
        '-days','1','-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1,DNS:localhost'],check=True,capture_output=True)
    base=dict(os.environ,SDL_AUDIODRIVER='dummy',PARTY_CA_BUNDLE=str(cert),PARTY_ICE_SERVERS='[]',PARTY_RELAY_ONLY='0',PARTY_BIND_ADDRESS='127.0.0.1')
    # Never reuse a developer's account credentials in an offline fixture.
    for name in ['PARTY_JWT_FILE','PARTY_SESSION_SECRET','PARTY_APPWRITE_ENDPOINT','PARTY_PROJECT_ID','PARTY_DATABASE_ID']:base.pop(name,None)
    hostToken=secrets.token_urlsafe(32);viewerToken=secrets.token_urlsafe(32)
    fixtureEnv=dict(base,TEST_HOST_TOKEN=hostToken,TEST_VIEWER_TOKEN=viewerToken,TEST_KEY=str(key),TEST_CERT=str(cert))
    try:
        server=subprocess.Popen(['node',str(root/'backend/test/tls-server.js')],env=fixtureEnv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        port=json.loads(server.stdout.readline())['port'];base['PARTY_FUNCTION_URL']=f'https://127.0.0.1:{port}'
        hostDir=work/'host';hostDir.mkdir();viewerDir=work/'viewer';viewerDir.mkdir()
        host=App([str(exe),'host','--file',str(movie),'--software'],dict(base,PARTY_JWT=hostToken),hostDir);apps.append(host)
        room=host.line('Room: ');invite=host.line('Invite (share privately): ')
        def join():
            v=App([str(exe),'viewer','--room',room,'--invite',invite,'--headless'],dict(base,PARTY_JWT=viewerToken),viewerDir);apps.append(v);return v
        viewer=join()
        viewer.wait(lambda:viewer.latest('player','renderedVideoFrames')>25 and viewer.latest('player','decodedAudioPackets')>50)
        before=viewer.latest('player','renderedVideoFrames');host.command('pause');time.sleep(2.5);viewer.command('stats');time.sleep(.4)
        paused=viewer.latest('player','renderedVideoFrames');time.sleep(1);viewer.command('stats');time.sleep(.4)
        assert viewer.latest('player','renderedVideoFrames')==paused,'Rendering continued while paused'
        host.command('play');viewer.wait(lambda:viewer.latest('player','renderedVideoFrames')>paused+20)
        epochs=[]
        for seconds in [12,2,16,4]:
            previous=viewer.latest('player','epoch');host.command(f'seek {seconds}');viewer.command('stats')
            viewer.wait(lambda:viewer.latest('player','epoch')>previous)
            epochs.append(viewer.latest('player','epoch'))
        first=viewer.stats[-1];viewer.command('quit');viewer.process.wait(timeout=15)
        time.sleep(3);viewer=join()
        viewer.wait(lambda:viewer.latest('player','renderedVideoFrames')>25 and viewer.latest('player','decodedAudioPackets')>50)
        second=viewer.stats[-1];host.command('quit');host.process.wait(timeout=15);viewer.process.wait(timeout=15)
        assert viewer.process.returncode==0 and host.process.returncode==0,'Unclean application shutdown'
        assert not list(viewerDir.rglob('*')),'Viewer wrote persistent files'
        report.update(firstViewer=first,secondViewer=second,seekEpochs=epochs,viewerFiles=[],exitCodes=[a.process.returncode for a in apps])
        request=urllib.request.Request(f'https://127.0.0.1:{port}/test-metrics',headers={'Authorization':'Bearer '+hostToken})
        with urllib.request.urlopen(request,context=ssl.create_default_context(cafile=str(cert))) as response:counts=json.load(response)
        server.terminate();server.wait(timeout=5)
        assert counts.get('signalResponseDropped')==1 and counts.get('ackResponseDropped')==1,'Lost-response scenarios not exercised'
        report['fixtureCounters']=counts;report['passed']=True
    finally:
        for app in reversed(apps):app.finish()
        if server and server.poll() is None:server.terminate();server.wait(timeout=5)
report['wallSeconds']=round(time.monotonic()-start,3)
output=json.dumps(report,indent=2)
if args.report:pathlib.Path(args.report).write_text(output+'\n')
print(output)
