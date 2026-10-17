"""STM32 HTTP + TLS/RNG/UTC engines against PC OpenSSL over UART byte queues.

This diagnostic does not claim to exercise Ethernet/dmtcp. Use httpprobe for
that path when the PC server is reachable from the board's network.
"""
import argparse
import datetime
from pathlib import Path
import re
import select
import ssl
import sys
import time
import os

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--boot-dir',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--port',default='/dev/ttyACM0')
p.add_argument('--case',action='append',default=[])
a=p.parse_args()
sys.path.insert(0,str(a.boot_dir/'scripts'))
from ymodem_send import Link
link=Link(a.port,921600)
completed=[]
ansi=re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')

def command(text):
    marker=b'HTTPWIRE_CMD_DONE_219';link.write(text.encode()+b'\necho '+marker+b'\n');out=b'';end=time.monotonic()+110
    while time.monotonic()<end:
        out+=link.drain(.1)
        if marker in out.splitlines():return ansi.sub(b'',out).decode(errors='replace')
    raise TimeoutError(text)

def fnv(data):
    value=2166136261
    for byte in data:value=((value^byte)*16777619)&0xffffffff
    return value

def probe(label,secure=False,chunked=False,redirect=False,hostname='tls.test',ca='/ca.pem',expected=4,flag=0,version=ssl.TLSVersion.TLSv1_3,downgrade=False):
    if a.case and label not in a.case:return
    body=b'board HTTP stream\x00\xff'*31
    reply=b'HTTP/1.1 200 OK\r\n'
    if chunked:reply+=b'Transfer-Encoding: chunked\r\n\r\n'+f'{len(body):x}'.encode()+b';test=yes\r\n'+body+b'\r\n0\r\nX-Test: yes\r\n\r\n'
    else:reply+=b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.minimum_version=ctx.maximum_version=version;ctx.load_cert_chain(a.fixtures/'server.pem',a.fixtures/'server.key')
    incoming=outgoing=peer=None;handshake=False;request=b'';pending=bytearray();opens=0;answered=False;buffer=b'';server_error=None
    def progress():
        nonlocal handshake,request,answered,server_error
        if secure:
            if server_error:return
            try:
                if not handshake:peer.do_handshake();handshake=True
                while not answered:
                    data=peer.read(4096)
                    if not data:break
                    request+=data
            except (ssl.SSLWantReadError,ssl.SSLWantWriteError):pass
            except ssl.SSLError as e:server_error=str(e)
        if b'\r\n\r\n' in request and not answered:
            if (redirect and opens==1) or downgrade:
                target=b'http://tls.test/plain' if downgrade else b'/next'
                message=b'HTTP/1.1 302 Found\r\nLocation: '+target+b'\r\nContent-Length: 0\r\n\r\n'
            else:message=reply
            if secure:peer.write(message)
            else:pending.extend(message)
            answered=True
        if secure:
            while outgoing.pending:pending.extend(outgoing.read())
    url=('https' if secure else 'http')+'://'+hostname+'/fixture'
    print('PROBE '+label,flush=True);link.write(f'httpwireprobe {url} {ca}\n'.encode());end=time.monotonic()+110
    while time.monotonic()<end:
        ready,_,_=select.select([link.rfd],[],[],.2)
        if not ready:continue
        buffer+=os.read(link.rfd,8192)
        while b'\n' in buffer:
            raw,buffer=buffer.split(b'\n',1);line=ansi.sub(b'',raw).replace(b'\r',b'').decode(errors='replace')
            if line.startswith('HTTPWIRE_OPEN '):
                print(f'OPEN {label} #{opens+1}',flush=True)
                opens+=1;request=b'';pending.clear();handshake=answered=False;server_error=None
                if secure:
                    incoming=ssl.MemoryBIO();outgoing=ssl.MemoryBIO();peer=ctx.wrap_bio(incoming,outgoing,server_side=True)
            elif line.startswith('HTTPWIRE_OUT '):
                data=bytes.fromhex(line[13:])
                if secure:incoming.write(data)
                else:request+=data
                progress()
            elif line=='HTTPWIRE_INPUT':
                progress();chunk=pending[:64];del pending[:64];link.write(b'I '+chunk.hex().encode()+b'\n')
            elif line.startswith('HTTPWIRE_DONE '):
                print(line,flush=True);values=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(-?\d+)',line))
                assert values['result']==expected,(label,values,server_error)
                if flag:assert values['verify']&flag,(label,values)
                if expected==4:
                    assert values['status']==200 and values['bytes']==len(body) and values['hash']==fnv(body),(label,values)
                    if redirect:assert values['redirects']==1
                link.drain(.2);completed.append(label);print('PASS '+label,flush=True);return
            elif 'ERROR' in line or 'fault' in line.lower():print(line,flush=True)
    raise TimeoutError(label+' '+repr(buffer[-200:]))

try:
    print(command('echo HTTPWIRE_READY'),flush=True)
    print(command('rtc set '+datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')),flush=True)
    result=command('test_dmhttp');print(result,flush=True);assert '3/3 passed' in result
    probe('HTTP-length')
    probe('HTTP-chunked',chunked=True)
    probe('HTTP-relative-redirect',redirect=True)
    probe('HTTPS-TLS12',secure=True,version=ssl.TLSVersion.TLSv1_2)
    probe('HTTPS-TLS13-chunked',secure=True,chunked=True)
    probe('HTTPS-relative-redirect',secure=True,redirect=True)
    probe('HTTPS-hostname-rejection',secure=True,hostname='wrong.test',expected=-6,flag=2)
    probe('HTTPS-CA-rejection',secure=True,ca='/other.pem',expected=-6,flag=1)
    probe('HTTPS-downgrade-rejection',secure=True,downgrade=True,expected=-12)
    print(f'HARDWARE_HTTP_WIRE {len(completed)}/{len(a.case) if a.case else 9} PASS',flush=True)
finally:
    try:
        print(command('rtc set '+datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')),flush=True)
        print(command('service status dmutc@rtc'),flush=True)
    finally:link.close()
