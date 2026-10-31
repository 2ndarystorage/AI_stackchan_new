"""Offline checks for the embedded TTS trust anchor (no API requests)."""
import hashlib
import pathlib
import re
import ssl
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / 'M5Unified_AI_StackChan/src'

class TtsCertificateTests(unittest.TestCase):
    def setUp(self):
        text = (SRC / 'WebVoiceVoxRootCA.h').read_text()
        match = re.search(r'R"CERT\(\n(.*?)\)CERT"', text, re.S)
        self.assertIsNotNone(match)
        self.pem = match.group(1)

    def test_official_self_signed_fingerprint(self):
        der = ssl.PEM_cert_to_DER_cert(self.pem)
        self.assertEqual(hashlib.sha256(der).hexdigest(),
                         '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6')

    def test_trust_anchor_is_valid_and_self_signed(self):
        with tempfile.TemporaryDirectory() as tmp:
            cert = pathlib.Path(tmp)/'root.pem'
            cert.write_text(self.pem)
            subprocess.run(['openssl','verify','-CAfile',str(cert),str(cert)],
                           check=True,capture_output=True)
            result = subprocess.check_output(['openssl','x509','-in',str(cert),'-noout','-subject','-issuer'],text=True)
            fields = dict(line.split('=',1) for line in result.strip().splitlines())
            self.assertEqual(fields['subject'], fields['issuer'])

    def test_tts_validation_remains_enabled(self):
        source = (SRC / 'WebVoiceVoxTTS.cpp').read_text()
        self.assertIn('tts_client->setCACert(root_ca);', source)
        self.assertNotIn('setInsecure(', source)

if __name__ == '__main__': unittest.main()
