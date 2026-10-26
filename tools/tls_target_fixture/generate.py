#!/usr/bin/env python3
"""Disposable on-target fixtures. No production private key is read or used."""
from datetime import datetime, timezone
from pathlib import Path
import json
import struct
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization as ser
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

HERE = Path(__file__).resolve().parent
FW = HERE.parents[1] / 'firmware-ESP32-LTE-M'
def date(y): return datetime(y, 1, 1, tzinfo=timezone.utc)
def name(s): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, s)])
def key(): return ec.generate_private_key(ec.SECP256R1())
def cert(subject, public, issuer, signer, ca=False, start=2026, end=2030):
    b = (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(name(issuer))
         .public_key(public).serial_number(x509.random_serial_number())
         .not_valid_before(date(start)).not_valid_after(date(end))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
         .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
              key_encipherment=False, data_encipherment=False, key_agreement=False,
              key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False), critical=True))
    if not ca:
        b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName('fixture.test')]), critical=False)
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    return b.sign(signer, hashes.SHA256())

roots = {}
for label in ('A', 'B', 'U'):
    k = key(); roots[label] = (k, cert('Root '+label, k.public_key(), 'Root '+label, k, ca=True))
mid, leaf = key(), key()
intermediates = {label:cert('Intermediate', mid.public_key(), 'Root '+label, roots[label][0], ca=True) for label in roots}
valid = cert('fixture.test', leaf.public_key(), 'Intermediate', mid)
pem = lambda c: c.public_bytes(ser.Encoding.PEM)
arrays = {}
def add(n, data): arrays[n] = data; return n
add('server_key', leaf.private_bytes(ser.Encoding.PEM, ser.PrivateFormat.PKCS8, ser.NoEncryption()))
cases=[]
for label in ('A', 'B'):
    n=add('valid_'+label, pem(valid)+pem(intermediates[label])); cases.append((n,n,'fixture.test',0))
cases.append(('wrong_hostname','valid_A','wrong.test',4))
add('unknown_root',pem(valid)+pem(intermediates['U'])); cases.append(('unknown_root','unknown_root','fixture.test',8))
for n, start, end, flag in [('expired',2024,2025,1),('not_yet_valid',2027,2030,0x200)]:
    add(n,pem(cert('fixture.test',leaf.public_key(),'Intermediate',mid,start=start,end=end))+pem(intermediates['A']))
    cases.append((n,n,'fixture.test',flag))
add('expired_intermediate',pem(valid)+pem(cert('Intermediate',mid.public_key(),'Root A',roots['A'][0],ca=True,start=2024,end=2025)))
cases.append(('expired_intermediate','expired_intermediate','fixture.test',1))
raw=bytearray(valid.public_bytes(ser.Encoding.DER)); raw[-1]^=1
add('bad_signature',pem(x509.load_der_x509_certificate(bytes(raw)))+pem(intermediates['A']))
cases.append(('bad_signature','bad_signature','fixture.test',8))

entries=[]
for label in ('A','B'):
    c=roots[label][1]
    entries.append((c.subject.public_bytes(),c.public_key().public_bytes(ser.Encoding.DER,ser.PublicFormat.SubjectPublicKeyInfo)))
entries.sort()
records=[struct.pack('<HH',len(n),len(k))+n+k for n,k in entries]
offsets=[]; off=4*len(records)
for r in records: offsets.append(off); off+=len(r)
bundle=struct.pack('<'+'I'*len(offsets),*offsets)+b''.join(records)
add('test_bundle',bundle)
denied=[]
manifest=json.loads((FW/'certs/manifest.json').read_text())
for i,k in enumerate(manifest['distrusted_keys']):
    pub=ser.load_der_public_key(bytes.fromhex(k['spki_der_hex']))
    for label in ('A','B'):
        n=f'denied_{i}_{label}'
        add(n,pem(cert('fixture.test',pub,'Root '+label,roots[label][0])))
        denied.append(n)

lines=['/* GENERATED disposable certificate fixtures. Never use as production trust. */']
for n,data in arrays.items():
    if n!='test_bundle': data+=b'\0'
    lines.append('static const unsigned char '+n+'[] = {'+','.join(str(x) for x in data)+'};')
lines.append('static const test_t handshake_cases[] = {')
for n,arr,hostname,flag in cases:
    lines.append(f'{{"{n}",{arr},sizeof({arr}),"{hostname}",{flag}}},')
lines.append('};\nstatic const test_t denied_cases[] = {')
for n in denied: lines.append(f'{{"{n}",{n},sizeof({n}),"fixture.test",2}},')
lines.append('};')
(HERE/'main/fixtures.h').write_text('\n'.join(lines)+'\n')
(HERE/'certs').mkdir(exist_ok=True)
(HERE/'certs/roots.pem').write_bytes((FW/'certs/roots.pem').read_bytes())
(HERE/'partitions.csv').write_bytes((FW/'partitions.csv').read_bytes())
print(f'Generated {len(cases)} full handshake cases and {len(denied)} cross-issuer blocked-key certificates.')
