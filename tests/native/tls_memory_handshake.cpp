// TLS handshake only. Never sends HTTP, synthesis requests, or API credentials.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mbedtls/platform.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include "TlsTestHeap.h"
using namespace tls_test;
int main(int argc,char**argv) {
  if(argc<3) { std::fprintf(stderr,"Usage: tls_memory_handshake HOST CA_PEM [INTERNAL_BUDGET] [mixed|internal]\n"); return 2; }
  heap.limit=argc>3?std::strtoul(argv[3],nullptr,10):46400;
  heap.mixed=argc<=4 || std::strcmp(argv[4],"internal")!=0;
  mbedtls_platform_set_calloc_free(testCalloc,testFree);
  mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config cfg;
  mbedtls_x509_crt ca; mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context rng;
  mbedtls_net_init(&net);mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&cfg);
  mbedtls_x509_crt_init(&ca);mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);
  int result=mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(const unsigned char*)"esp32-tls",9);
  if(!result)result=mbedtls_x509_crt_parse_file(&ca,argv[2]);
  if(!result)result=mbedtls_ssl_config_defaults(&cfg,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&cfg,MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&cfg,&ca,nullptr);
  mbedtls_ssl_conf_rng(&cfg,mbedtls_ctr_drbg_random,&rng);
  mbedtls_ssl_conf_min_version(&cfg,3,3);mbedtls_ssl_conf_max_version(&cfg,3,3);
  if(!result)result=mbedtls_ssl_setup(&ssl,&cfg);
  if(!result)result=mbedtls_ssl_set_hostname(&ssl,argv[1]);
  if(!result)result=mbedtls_net_connect(&net,argv[1],"443",MBEDTLS_NET_PROTO_TCP);
  mbedtls_ssl_set_bio(&ssl,&net,mbedtls_net_send,mbedtls_net_recv,nullptr);
  if(!result) do { result=mbedtls_ssl_handshake(&ssl); } while(result==MBEDTLS_ERR_SSL_WANT_READ || result==MBEDTLS_ERR_SSL_WANT_WRITE);
  char error[180];mbedtls_strerror(result,error,sizeof(error));
  std::printf("host=%s budget=%zu mixed=%d result=%d flags=0x%08x alloc_failures=%zu external_allocations=%zu %s\n",
      argv[1],heap.limit,heap.mixed,result,(unsigned)mbedtls_ssl_get_verify_result(&ssl),heap.failures,heap.externalCalls,error);
  mbedtls_ssl_close_notify(&ssl);mbedtls_net_free(&net);mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&cfg);
  mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
  assert(heap.internalBytes==0 && heap.externalBytes==0);
  return result?1:0;
}
