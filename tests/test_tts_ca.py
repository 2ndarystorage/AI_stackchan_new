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
        self.pems = re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', match.group(1), re.S)

    def test_official_self_signed_fingerprint(self):
        fingerprints = {hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest() for pem in self.pems}
        self.assertEqual(fingerprints, {
            '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6',
            'e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f'})

    def test_trust_anchor_is_valid_and_self_signed(self):
        with tempfile.TemporaryDirectory() as tmp:
            cert = pathlib.Path(tmp)/'root.pem'
            for pem in self.pems:
                cert.write_text(pem)
                subprocess.run(['openssl','verify','-check_ss_sig','-CAfile',str(cert),str(cert)],
                               check=True,capture_output=True)
                result = subprocess.check_output(['openssl','x509','-in',str(cert),'-noout','-subject','-issuer'],text=True)
                fields = dict(line.split('=',1) for line in result.strip().splitlines())
                self.assertEqual(fields['subject'], fields['issuer'])

    def test_google_bundle_contains_r4(self):
        text = (SRC / 'rootCAgoogle.h').read_text()
        pems = re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', text, re.S)
        self.assertEqual(len(pems), 2)
        fingerprints = {hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest() for pem in pems}
        self.assertIn('349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d', fingerprints)
        self.pems = pems
        self.test_trust_anchor_is_valid_and_self_signed()

    def test_tts_validation_remains_enabled(self):
        source = (SRC / 'WebVoiceVoxTTS.cpp').read_text()
        self.assertIn('tts_client->setCACert(root_ca);', source)
        self.assertNotIn('setInsecure(', source)

if __name__ == '__main__': unittest.main()
