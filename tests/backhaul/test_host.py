import importlib.util
import os
from pathlib import Path
import socket
import ssl
import subprocess
import time
import unittest

spec=importlib.util.spec_from_file_location('pair',Path(__file__).parents[2]/'tools/backhaul/czc_pair.py')
pair=importlib.util.module_from_spec(spec); spec.loader.exec_module(pair)

class Frames(unittest.TestCase):
    def test_fragmented_and_coalesced(self):
        for size in range(251):
            frame=pair.packet(0x61,0xc3,bytes(range(size)))
            parser=pair.Parser(); result=[]
            for b in frame: result+=parser.feed(bytes([b]))
            self.assertEqual(result,[(0x61,0xc3,bytes(range(size)))])
            self.assertEqual(pair.Parser().feed(frame*4),result*4)

    def test_malformed(self):
        for raw in (b'\xff',b'\xfe\xfb',b'\xfe\x00\x61\x00\x00'):
            with self.assertRaises(ValueError): pair.Parser().feed(raw)
        with self.assertRaises(ValueError): pair.packet(0x21,1,b'X'*251)

    def test_psk_size(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'psk'; path.write_text('00'*31)
            with self.assertRaises(ValueError): pair.key_read(path)
            path.write_text('01'*32); self.assertEqual(pair.key_read(path),b'\1'*32)

class Authentication(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=subprocess.Popen([os.environ.get('CZC_TLS_PEER','.backhaul-tests/tls-peer'),'27443'])
        time.sleep(0.2)
    @classmethod
    def tearDownClass(cls):
        cls.server.terminate(); cls.server.wait(timeout=5)
    def tearDown(self): time.sleep(0.08)
    def test_ipv4_and_ipv6_encrypted_stream(self):
        for host in ('127.0.0.1','::1'):
            with pair.connect(host,bytes(range(32)),port=27443) as conn:
                self.assertEqual(conn.version(),'TLSv1.3')
                self.assertEqual(conn.cipher()[0],'TLS_AES_128_GCM_SHA256')
                for size in (1,250,260,4096,16384):
                    data=bytes(i%251 for i in range(size)); conn.sendall(data)
                    out=b''
                    while len(out)<len(data): out+=conn.recv(len(data)-len(out))
                    self.assertEqual(data,out)
            time.sleep(0.08)
    def test_wrong_key(self):
        with self.assertRaises(ssl.SSLError): pair.connect('::1',b'\xff'*32,port=27443)
    def test_wrong_identity(self):
        with self.assertRaises(ssl.SSLError): pair.connect('127.0.0.1',bytes(range(32)),'czc-peer-v1',27443)
    def test_hardware_acceptance_denial_evidence(self):
        worker=dict(pair.__dict__)
        exec((Path(__file__).parents[2]/'tools/backhaul/accept_hardware_radio.py').read_text(),worker)
        for key,identity in ((b'\xff'*32,'czc-admin-v1'),(bytes(range(32)),'czc-peer-v2')):
            with pair.connect('127.0.0.1',bytes(range(32)),port=27443): pass
            time.sleep(.08)
            worker['denied_handshake']('127.0.0.1',key,identity,27443)
            time.sleep(.08)
            with pair.connect('127.0.0.1',bytes(range(32)),port=27443): pass
            time.sleep(.08)
    def test_no_certificate_fallback(self):
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
        ctx.minimum_version=ctx.maximum_version=ssl.TLSVersion.TLSv1_3
        with socket.create_connection(('127.0.0.1',27443),3) as raw:
            with self.assertRaises(ssl.SSLError): ctx.wrap_socket(raw)
    def test_no_tls12_downgrade(self):
        ctx=pair.tls_context(bytes(range(32)))
        ctx.minimum_version=ctx.maximum_version=ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers('PSK-AES128-GCM-SHA256')
        with socket.create_connection(('127.0.0.1',27443),3) as raw:
            with self.assertRaises(ssl.SSLError): ctx.wrap_socket(raw)
    def test_handshake_deadline(self):
        with socket.create_connection(('::1',27443),3) as raw:
            raw.settimeout(5); self.assertEqual(raw.recv(1),b'')
    def test_bulk_stream_and_reconnect(self):
        for iteration in range(2):
            with pair.connect('::1',bytes(range(32)),port=27443) as client:
                data=bytes(i%251 for i in range(100000)); client.sendall(data)
                out=b''
                while len(out)<len(data):
                    part=client.recv(4096)
                    self.assertTrue(part); out+=part
                self.assertEqual(out,data)
            time.sleep(0.15)

if __name__=='__main__': unittest.main(verbosity=1)
