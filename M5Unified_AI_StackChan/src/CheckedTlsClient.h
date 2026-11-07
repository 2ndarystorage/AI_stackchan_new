#pragma once
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#include "TlsMemory.h"

// Arduino 2.0.9 normally clears the verification details in stop() on failure.
// Preserve them BEFORE teardown; never change trust, hostname or verification mode.
class CheckedTlsClient : public WiFiClientSecure {
 public:
  bool allocationFailureInHandshake() const { return allocationFailed_; }
  uint32_t verificationFlags() const { return verificationFlags_; }
  int connect(const char* host, uint16_t port) override {
    allocationFailed_ = false;
    verificationFlags_ = 0xFFFFFFFFu;
    IPAddress address;
    if (!WiFi.hostByName(host, address)) return 0;
    const auto before = tlsMemorySnapshot();
    const int result = start_ssl_client(sslclient, address, port, host, _timeout,
        _CA_cert, false, _cert, _private_key, nullptr, nullptr, false, _alpn_protos);
    const auto after = tlsMemorySnapshot();
    allocationFailed_ = after.failures != before.failures && after.lastFailedTask == tlsCurrentTaskTag();
    Serial.printf("[TLS] memory policy=%d external_allocs=%lu failures=%lu last_failed_bytes=%u this_task_failure=%d\n",
        after.installed, (unsigned long)(after.externalAllocations - before.externalAllocations),
        (unsigned long)(after.failures - before.failures), (unsigned)after.lastFailedBytes, allocationFailed_);
    _lastError = result;
    if (result < 0) {
      const uint32_t flags = mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx);
      verificationFlags_ = flags;
      Serial.printf("[TLS] verify_failed host=%s ip=%s error=%d flags=0x%08lx\n",
                    host, address.toString().c_str(), result, (unsigned long)flags);
      if (flags && flags != 0xFFFFFFFFu) {
        char detail[256] = {};
        mbedtls_x509_crt_verify_info(detail, sizeof(detail), "  ", flags);
        Serial.print(detail);
      }
      const auto* peer = mbedtls_ssl_get_peer_cert(&sslclient->ssl_ctx);
      if (peer) {
        char issuer[192] = {};
        mbedtls_x509_dn_gets(issuer, sizeof(issuer), &peer->issuer);
        Serial.printf("[TLS] peer_issuer=%s valid=%04d-%02d-%02d..%04d-%02d-%02d\n",
            issuer, peer->valid_from.year, peer->valid_from.mon, peer->valid_from.day,
            peer->valid_to.year, peer->valid_to.mon, peer->valid_to.day);
      }
      stop();
      return 0;
    }
    _connected = true;
    verificationFlags_ = 0;
    Serial.printf("[TLS] verified host=%s ip=%s\n", host, address.toString().c_str());
    return 1;
  }
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    _timeout = timeout;
    return connect(host, port);
  }
 private:
  bool allocationFailed_ = false;
  uint32_t verificationFlags_ = 0xFFFFFFFFu;
};
