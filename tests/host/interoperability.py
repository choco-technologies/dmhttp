"""Production HTTP + dmtls engines: fragmented framing and independent peers."""
import ctypes as C
import os
from pathlib import Path
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

lib=C.CDLL(sys.argv.pop(1))
IO=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t))
Open=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_char_p,C.c_uint16)
Close=C.CFUNCTYPE(None,C.c_void_p)
Entropy=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_size_t)
class Stream(C.Structure):_fields_=[('send',IO),('receive',IO),('user',C.c_void_p)]
class Transport(C.Structure):_fields_=[('open',Open),('close',Close),('stream',Stream)]
class Options(C.Structure):
    _fields_=[('transport',Transport),('tls_config',C.c_void_p),('method',C.c_int),('timeout_ms',C.c_uint32),('max_redirects',C.c_uint32),('max_body_bytes',C.c_uint64),('range_enabled',C.c_bool),('range_start',C.c_uint64)]
class Response(C.Structure):
    _fields_=[('status_code',C.c_uint),('redirects',C.c_uint),('content_length_known',C.c_bool),('content_length',C.c_uint64),('body_bytes',C.c_uint64),('content_range',C.c_char*128),('content_type',C.c_char*128),('error',C.c_int),('tls_error',C.c_int),('verification_flags',C.c_uint32)]
class Platform(C.Structure):_fields_=[('entropy',Entropy),('user',C.c_void_p),('heap_limit',C.c_size_t),('max_connections',C.c_uint32)]
for name,args in {'create':[C.c_char_p,C.POINTER(Options),C.POINTER(C.c_void_p)],'step':[C.c_void_p],
 'read':[C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)],'response':[C.c_void_p,C.POINTER(Response)],'cancel':[C.c_void_p]}.items():
    f=getattr(lib,'dmhttp_'+name);f.argtypes=args;f.restype=C.c_int
lib.dmhttp_destroy.argtypes=[C.c_void_p]
lib.dmtls_initialize.argtypes=[C.POINTER(Platform)]
lib.dmtls_config_create.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(C.c_void_p)]
lib.dmtls_config_destroy.argtypes=[C.c_void_p]
lib.test_live_allocations.restype=C.c_size_t
lib.test_ticks_advance.argtypes=[C.c_uint32]
assert lib.dmtls_test_module_init(None)==0
@Entropy
def entropy(user,out,n):C.memmove(out,os.urandom(n),n);return 0
assert lib.dmtls_initialize(C.byref(Platform(entropy,None,4*1024*1024,4)))==0

class Script:
    def __init__(self,*replies,fragment=1,wait=True):
        self.replies=replies;self.fragment=fragment;self.wait=wait;self.calls=0;self.closes=0;self.hosts=[];self.requests=[];self.data=b''
        self.callbacks=(Open(self.open),Close(self.close),IO(self.send),IO(self.recv))
    def open(self,user,host,port):
        self.hosts.append((host,port));self.data=self.replies[min(len(self.hosts)-1,len(self.replies)-1)];self.requests.append(bytearray());return 0
    def close(self,user):self.closes+=1
    def send(self,user,data,n,out):
        out[0]=0;self.calls+=1
        if self.wait and self.calls%3==0:return 2
        count=min(n,13);self.requests[-1].extend(C.string_at(data,count));out[0]=count;return 0
    def recv(self,user,data,n,out):
        out[0]=0;self.calls+=1
        if self.wait and self.calls%3==0:return 1
        count=min(n,len(self.data),self.fragment);C.memmove(data,self.data[:count],count);self.data=self.data[count:];out[0]=count;return 0
    def transport(self):return Transport(self.callbacks[0],self.callbacks[1],Stream(self.callbacks[2],self.callbacks[3],None))
    def wait_ready(self,rc):pass

class Network(Script):
    def __init__(self,context,reply):
        super().__init__(fragment=71);self.context=context;self.reply=reply;self.errors=[];self.notifications=0
    def open(self,user,host,port):
        self.hosts.append((host,port));self.requests.append(bytearray());self.socket,server=socket.socketpair();self.socket.setblocking(False)
        def peer():
            try:
                with server:
                    server.settimeout(5)
                    s=self.context.wrap_socket(server,server_side=True) if self.context else server
                    with s:
                        request=b''
                        while b'\r\n\r\n' not in request:request+=s.recv(4096)
                        self.requests[-1].extend(request)
                        for at in range(0,len(self.reply),29):s.sendall(self.reply[at:at+29])
                        if self.context:
                            try:s.unwrap();self.notifications+=1
                            except (ssl.SSLError,TimeoutError,ConnectionError):pass
            except (ssl.SSLError,ConnectionError,OSError) as e:self.errors.append(e)
        self.thread=threading.Thread(target=peer,daemon=True);self.thread.start();return 0
    def close(self,user):
        self.closes+=1;self.socket.close();self.thread.join(6)
    def send(self,user,data,n,out):
        out[0]=0
        try:out[0]=self.socket.send(C.string_at(data,min(n,97)));return 0
        except BlockingIOError:return 2
        except OSError:return -8
    def recv(self,user,data,n,out):
        out[0]=0
        try:chunk=self.socket.recv(min(n,71));C.memmove(data,chunk,len(chunk));out[0]=len(chunk);return 0
        except BlockingIOError:return 1
        except OSError:return -8
    def wait_ready(self,rc):select.select([self.socket] if rc==1 else [],[self.socket] if rc==2 else [],[],.01)

class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
        cls.configs=[]
        for name in ('server','other'):
            subprocess.run(['openssl','req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes','-days','2','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(cls.root/(name+'.key')),'-out',str(cls.root/(name+'.pem'))],check=True,capture_output=True)
            ca=(cls.root/(name+'.pem')).read_bytes();config=C.c_void_p();assert lib.dmtls_config_create(ca,len(ca),C.byref(config))==0;cls.configs.append(config)
    @classmethod
    def tearDownClass(cls):
        for config in cls.configs:assert lib.dmtls_config_destroy(config)==0
        assert lib.dmtls_shutdown()==0
        assert lib.dmod_deinit()==0
        assert lib.test_live_allocations()==0
        cls.temp.cleanup()
    def run_http(self,adapter,url=b'http://example.test/a/b',**overrides):
        options=Options();options.transport=adapter.transport();options.timeout_ms=15000;options.max_redirects=3
        for k,v in overrides.items():setattr(options,k,v)
        h=C.c_void_p();rc=lib.dmhttp_create(url,C.byref(options),C.byref(h));body=bytearray();response=Response()
        try:
            if rc!=0:return rc,body,response
            end=time.monotonic()+20
            while True:
                rc=lib.dmhttp_step(h)
                if rc<=0 or rc==4:break
                adapter.wait_ready(rc)
                if time.monotonic()>end:raise TimeoutError('headers')
            if rc==0:
                while True:
                    out=C.create_string_buffer(113);n=C.c_size_t();rc=lib.dmhttp_read(h,out,len(out),C.byref(n));body.extend(out.raw[:n.value])
                    if rc<0 or rc==4:break
                    adapter.wait_ready(rc)
                    if time.monotonic()>end:raise TimeoutError('body')
            lib.dmhttp_response(h,C.byref(response));return rc,body,response
        finally:lib.dmhttp_destroy(h)
    def test_fixed_length_and_request(self):
        s=Script(b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: audio/mpeg\r\n\r\nhello')
        rc,body,r=self.run_http(s);self.assertEqual((rc,body,r.status_code),(4,b'hello',200));self.assertEqual(r.content_type,b'audio/mpeg');self.assertEqual(s.closes,1)
        self.assertIn(b'GET /a/b HTTP/1.1\r\nHost: example.test\r\n',s.requests[0]);self.assertIn(b'Accept-Encoding: identity',s.requests[0])
    def test_chunked_extensions_trailers(self):
        s=Script(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: cHuNkEd\r\n\r\n2;foo="a\\\"b"\r\nhe\r\n3\r\nllo\r\n0\r\nX-Checksum: yes\r\n\r\n')
        rc,body,r=self.run_http(s);self.assertEqual((rc,body),(4,b'hello'))
    def test_close_delimited(self):self.assertEqual(self.run_http(Script(b'HTTP/1.0 200 OK\r\n\r\nhello'))[:2],(4,b'hello'))
    def test_empty_head_and_304(self):
        for status,extra,method in [(200,b'Content-Length: 9000\r\n',1),(204,b'',0),(304,b'Content-Length: 12\r\n',0)]:
            with self.subTest(status=status):self.assertEqual(self.run_http(Script(b'HTTP/1.1 '+str(status).encode()+b' OK\r\n'+extra+b'\r\n'),method=method)[:2],(4,b''))
    def test_informational_response(self):self.assertEqual(self.run_http(Script(b'HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'))[:2],(4,b'ok'))
    def test_http_error_status_is_available(self):
        rc,body,r=self.run_http(Script(b'HTTP/1.1 404 Missing\r\nContent-Length: 2\r\n\r\nno'));self.assertEqual((rc,body,r.status_code),(4,b'no',404))
    def test_relative_redirect(self):
        s=Script(b'HTTP/1.1 302 Found\r\nLocation: ../c?q=1#fragment\r\n\r\n',b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok')
        rc,body,r=self.run_http(s);self.assertEqual((rc,body,r.redirects,s.closes),(4,b'ok',1,2));self.assertIn(b'GET /c?q=1 HTTP/1.1',s.requests[1])
    def test_redirect_references(self):
        for ref,target,host in [(b'/x/../y',b'/y',b'example.test'),(b'?q=2',b'/a/b?q=2',b'example.test'),(b'//other.test:90/a/../z',b'/z',b'other.test'),(b'http://other.test/a/../z',b'/z',b'other.test')]:
            with self.subTest(ref=ref):
                s=Script(b'HTTP/1.1 307 Temporary\r\nLocation: '+ref+b'\r\n\r\n',b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n');self.assertEqual(self.run_http(s)[0],4);self.assertIn(b'GET '+target+b' HTTP',s.requests[1]);self.assertEqual(s.hosts[1][0],host)
    def test_redirect_limit(self):self.assertEqual(self.run_http(Script(b'HTTP/1.1 302 Loop\r\nLocation: /a\r\n\r\n'),max_redirects=2)[0],-12)
    def test_informational_cannot_reset_redirect_limit(self):
        s=Script(b'HTTP/1.1 103 Hint\r\n\r\nHTTP/1.1 302 Loop\r\nLocation: /a\r\n\r\n')
        self.assertEqual(self.run_http(s,max_redirects=2)[0],-12)
    def test_redirect_disabled(self):
        rc,_,r=self.run_http(Script(b'HTTP/1.1 302 Here\r\nLocation: /a\r\nContent-Length: 0\r\n\r\n'),max_redirects=0);self.assertEqual((rc,r.status_code),(4,302))
    def test_range(self):
        s=Script(b'HTTP/1.1 206 Partial\r\nContent-Length: 2\r\nContent-Range: bytes 5-6/7\r\n\r\nok');rc,_,r=self.run_http(s,range_enabled=True,range_start=5);self.assertEqual(rc,4);self.assertEqual(r.content_range,b'bytes 5-6/7');self.assertIn(b'Range: bytes=5-',s.requests[0])
    def test_large_stream(self):
        data=bytes(range(256))*8192;s=Script(b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(data)).encode()+b'\r\n\r\n'+data,fragment=997,wait=False)
        rc,body,r=self.run_http(s);self.assertEqual(rc,4);self.assertEqual(body,data);self.assertEqual(r.body_bytes,len(data))
    def test_framing_errors(self):
        cases=[(b'Content-Length: 2\r\nContent-Length: 3',b'abc',-7),(b'Content-Length: -1',b'',-7),(b'Content-Length: 18446744073709551616',b'',-7),
          (b'Transfer-Encoding: chunked\r\nContent-Length: 3',b'abc',-7),(b'Transfer-Encoding: gzip, chunked',b'',-7),
          (b'Content-Encoding: gzip',b'',-7),(b'Bad : value',b'',-7),(b' folded: x',b'',-7),
          (b'Content-Length: 5',b'abc',-9),(b'Transfer-Encoding: chunked',b'3\r\nab',-9),
          (b'Transfer-Encoding: chunked',b'1\r\nxXX',-7),(b'Transfer-Encoding: chunked',b'0\r\nContent-Length: 0\r\n\r\n',-7),
          (b'Transfer-Encoding: chunked',b'Z\r\n',-7),(b'Transfer-Encoding: chunked',b'1;=bad\r\n',-7),
          (b'Transfer-Encoding: chunked',b'fffffffffffffffff\r\n',-8), (b'X: '+b'x'*2050,b'',-8),
          (b'Transfer-Encoding: chunked',b'0\r\n',-9)]
        for fields,body,error in cases:
            with self.subTest(fields=fields[:80],body=body[:30]):self.assertEqual(self.run_http(Script(b'HTTP/1.1 200 OK\r\n'+fields+b'\r\n\r\n'+body))[0],error)
    def test_status_and_line_errors(self):
        for data in [b'HTTP/2 200 OK\r\n\r\n',b'HTTP/1.1 200 OK\n\n',b'HTTP/1.1 101 Switch\r\n\r\n',b'HTTP/1.1 204 None\r\nContent-Length: 1\r\n\r\nx',b'HTTP/1.1 200 OK\r\nX: a\x00b\r\n\r\n']:
            with self.subTest(data=data):self.assertEqual(self.run_http(Script(data))[0],-7)
    def test_body_limits(self):
        for data in [b'Content-Length: 5\r\n\r\nhello',b'Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n',b'\r\nhello']:
            with self.subTest(data=data):self.assertEqual(self.run_http(Script(b'HTTP/1.1 200 OK\r\n'+data),max_body_bytes=3)[0],-8)
    def test_invalid_urls(self):
        for url in [b'ftp://a/b',b'http://user@a/',b'http://a:0/',b'http://a:99999/',b'http://[::1]/',b'http://a/x\r\ny',b'http://a/a b',b'http://a/%xx',b'http://a/\\b']:
            with self.subTest(url=url):self.assertEqual(self.run_http(Script(b''),url=url)[0],-4)
    def test_pending_open_cancel_and_transport_contract(self):
        class Pending(Script):
            def open(self,user,host,port):return 1
        s=Pending(b'');o=Options();o.transport=s.transport();o.timeout_ms=1000;h=C.c_void_p()
        self.assertEqual(lib.dmhttp_create(b'http://a/',C.byref(o),C.byref(h)),0)
        self.assertEqual(lib.dmhttp_step(h),1);self.assertEqual(lib.dmhttp_cancel(h),-11)
        lib.dmhttp_destroy(h);self.assertEqual(s.closes,1)
        class Bad(Script):
            def send(self,user,data,n,out):out[0]=1;return 2
        self.assertEqual(self.run_http(Bad(b''))[0],-5)
    def test_header_budget(self):
        fields=b'X-Test: '+b'x'*100+b'\r\n'
        self.assertEqual(self.run_http(Script(b'HTTP/1.1 200 OK\r\n'+fields*80+b'\r\n',fragment=997,wait=False))[0],-8)
    def test_case_insensitive_scheme(self):
        self.assertEqual(self.run_http(Script(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n'),url=b'HTTP://EXAMPLE.TEST/')[0],4)
    def test_cancel_and_timeout(self):
        s=Script(b'');o=Options();o.transport=s.transport();o.timeout_ms=100;h=C.c_void_p()
        for timeout in (False,True):
            self.assertEqual(lib.dmhttp_create(b'http://a/',C.byref(o),C.byref(h)),0);self.assertEqual(lib.dmhttp_step(h),3)
            if timeout:lib.test_ticks_advance(101);self.assertEqual(lib.dmhttp_step(h),-10)
            else:self.assertEqual(lib.dmhttp_cancel(h),-11);self.assertEqual(lib.dmhttp_step(h),-11)
            lib.dmhttp_destroy(h)
        self.assertEqual(s.closes,2)
    def test_module_lifecycle_busy(self):
        s=Script(b'');o=Options();o.transport=s.transport();o.timeout_ms=1000;h=C.c_void_p()
        self.assertEqual(lib.dmhttp_create(b'http://a/',C.byref(o),C.byref(h)),0)
        self.assertLess(lib.dmod_deinit(),0);lib.dmhttp_destroy(h);self.assertEqual(lib.dmod_deinit(),0)
    def context(self,version):
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.minimum_version=ctx.maximum_version=version;ctx.load_cert_chain(self.root/'server.pem',self.root/'server.key');return ctx
    def test_http_socket(self):self.assertEqual(self.run_http(Network(None,b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello'))[:2],(4,b'hello'))
    def test_https_tls12_tls13(self):
        for version in (ssl.TLSVersion.TLSv1_2,ssl.TLSVersion.TLSv1_3):
            with self.subTest(version=version):
                s=Network(self.context(version),b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n')
                self.assertEqual(self.run_http(s,url=b'https://localhost/',tls_config=self.configs[0])[:2],(4,b'hello'));self.assertEqual(s.notifications,len(s.hosts))
    def test_https_redirect(self):
        class Redirect(Network):
            def open(self,user,host,port):
                self.reply=(b'HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n' if not self.hosts else b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello')
                return super().open(user,host,port)
        s=Redirect(self.context(ssl.TLSVersion.TLSv1_3),b'')
        self.assertEqual(self.run_http(s,url=b'https://localhost/',tls_config=self.configs[0])[:2],(4,b'hello'));self.assertEqual(s.notifications,len(s.hosts))
    def test_https_close_delimited(self):
        self.assertEqual(self.run_http(Network(self.context(ssl.TLSVersion.TLSv1_3),b'HTTP/1.1 200 OK\r\n\r\nhello'),url=b'https://localhost/',tls_config=self.configs[0])[:2],(4,b'hello'))
    def test_https_untrusted_and_hostname(self):
        for host,config,flag in [(b'localhost',self.configs[1],1),(b'wrong.test',self.configs[0],2)]:
            with self.subTest(host=host):
                rc,_,r=self.run_http(Network(self.context(ssl.TLSVersion.TLSv1_3),b''),url=b'https://'+host+b'/',tls_config=config)
                self.assertEqual((rc,r.tls_error),(-6,-6));self.assertTrue(r.verification_flags&flag)
    def test_https_downgrade(self):
        s=Network(self.context(ssl.TLSVersion.TLSv1_3),b'HTTP/1.1 302 Found\r\nLocation: http://localhost/plain\r\n\r\n')
        self.assertEqual(self.run_http(s,url=b'https://localhost/',tls_config=self.configs[0])[0],-12)
    def test_https_missing_tls_config(self):self.assertEqual(self.run_http(Script(b''),url=b'https://localhost/')[0],-6)

if __name__=='__main__':unittest.main(verbosity=2)
