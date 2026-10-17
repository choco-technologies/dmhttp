"""Compare MCU Ethernet/dmtcp downloads with Python's independent HTTP client.

The board must have a running UTC service and a public/fixture CA file. No TLS
verification is bypassed. The server must be reachable from both PC and board.
"""
import argparse
import datetime
import http.client
from pathlib import Path
import re
import shlex
import socket
import ssl
import sys
import time
from urllib.parse import urlsplit

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--boot-dir',type=Path,required=True)
parser.add_argument('--url',action='append',required=True)
parser.add_argument('--server-ip',help='Override DNS for both reference and board')
parser.add_argument('--ca-board',required=True)
parser.add_argument('--ca-local',type=Path,help='Fixture CA; otherwise use the PC system trust store')
parser.add_argument('--module',default='httpprobe')
parser.add_argument('--repeat',type=int,default=1)
parser.add_argument('--regressions',action='store_true')
parser.add_argument('--port',default='/dev/ttyACM0')
args=parser.parse_args()
sys.path.insert(0,str(args.boot_dir/'scripts'))
from ymodem_send import Link
link=Link(args.port,921600)
ansi=re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')

def command(text,timeout=75):
    marker=b'HTTP_TCP_TEST_END_812';link.write(text.encode()+b'\necho '+marker+b'\n');out=b'';end=time.monotonic()+timeout
    while time.monotonic()<end:
        out+=link.drain(.1)
        if marker in out.splitlines():return ansi.sub(b'',out).decode(errors='replace')
    raise TimeoutError(text)

def reference(url,address):
    parts=urlsplit(url)
    if parts.scheme not in ('http','https') or not parts.hostname:raise ValueError('expected HTTP(S) URL')
    port=parts.port or (443 if parts.scheme=='https' else 80)
    raw=socket.create_connection((address,port),10)
    if parts.scheme=='https':
        ctx=ssl.create_default_context(cafile=str(args.ca_local) if args.ca_local else None)
        raw=ctx.wrap_socket(raw,server_hostname=parts.hostname)
    with raw:
        connection=http.client.HTTPConnection(parts.hostname,port,timeout=10)
        connection.sock=raw
        connection.request('GET',(parts.path or '/')+('?' + parts.query if parts.query else ''),headers={'Connection':'close','Accept-Encoding':'identity'})
        response=connection.getresponse();data=response.read();status=response.status;connection.close()
    value=2166136261
    for byte in data:value=((value^byte)*16777619)&0xffffffff
    return status,len(data),value

try:
    print(command('rtc set '+datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')),flush=True)
    passed=0
    for url in args.url:
        address=args.server_ip or socket.gethostbyname(urlsplit(url).hostname)
        expected=reference(url,address)
        for run in range(args.repeat):
            started=time.monotonic()
            out=command(' '.join(shlex.quote(part) for part in (args.module,url,address,args.ca_board)))
            print(out,flush=True)
            match=re.search(r'HTTP_RESULT result=(-?\d+) status=(\d+) bytes=(\d+) hash=(\d+)',out)
            assert match, 'missing result'
            result,status,length,checksum=map(int,match.groups())
            assert result==4 and (status,length,checksum)==expected,(url,result,status,length,checksum,expected)
            passed+=1;print(f'PASS {urlsplit(url).scheme} #{run+1} elapsed={time.monotonic()-started:.2f}s',flush=True)
    if args.regressions:
        for module,count in [('test_dmhttp','3/3'),('test_dmutc','6/6'),('test_dmrtc','12/12'),('test_dmrng','6/6')]:
            out=command(module);print(out,flush=True);assert count+' passed' in out
    print(f'HARDWARE_HTTP_TCP {passed}/{len(args.url)*args.repeat} PASS',flush=True)
finally:
    try:
        print(command('rtc set '+datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')),flush=True)
        print(command('service status dmutc@rtc'),flush=True)
    finally:link.close()
