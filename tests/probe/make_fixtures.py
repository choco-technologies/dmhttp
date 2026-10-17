#!/usr/bin/env python3
"""Create disposable hardware TLS fixtures. Keep the output under ignored build/."""
import argparse
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory',type=Path)
args=parser.parse_args()
root=args.directory
root.mkdir(parents=True,exist_ok=True)
if any(root.iterdir()):parser.error('output directory must be empty')

def openssl(*args):
    subprocess.run(['openssl',*map(str,args)],check=True,capture_output=True)

for stem in ('ca','other'):
    openssl('req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256',
        '-nodes','-keyout',root/(stem+'.key'),'-out',root/(stem+'.pem'),
        '-days','365','-subj','/CN=dmtls disposable '+stem,
        '-addext','basicConstraints=critical,CA:TRUE',
        '-addext','keyUsage=critical,keyCertSign,cRLSign')
openssl('req','-new','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256',
    '-nodes','-keyout',root/'server.key','-out',root/'server.csr','-subj','/CN=tls.test')
(root/'extensions.cnf').write_text('basicConstraints=critical,CA:FALSE\n'
    'keyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:tls.test\n')
openssl('x509','-req','-in',root/'server.csr','-CA',root/'ca.pem','-CAkey',root/'ca.key',
    '-CAcreateserial','-out',root/'server.pem','-days','2','-extfile',root/'extensions.cnf')
print('Created disposable fixtures. Only ca.pem and other.pem belong on the board.')
