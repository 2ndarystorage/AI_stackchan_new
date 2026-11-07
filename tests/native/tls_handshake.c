#include <stdio.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
static int verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
 char issuer[256]; (void)ctx;
 mbedtls_x509_dn_gets(issuer,sizeof(issuer),&crt->issuer);
 printf(" depth=%d flags=0x%08x issuer=%s\n",depth,(unsigned)*flags,issuer);
 return 0; /* Do not change the verification flags. */
}
int main(int argc,char **argv) {
 if(argc!=3) return 2;
 mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
 mbedtls_x509_crt ca; mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context rng;
 mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
 mbedtls_x509_crt_init(&ca); mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng);
 int ret=mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(unsigned char*)"esp32-tls",9);
 if(ret==0)ret=mbedtls_x509_crt_parse_file(&ca,argv[2]);
 if(ret==0)ret=mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
 mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_REQUIRED);
 mbedtls_ssl_conf_ca_chain(&conf,&ca,NULL);
 mbedtls_ssl_conf_rng(&conf,mbedtls_ctr_drbg_random,&rng);
 mbedtls_ssl_conf_verify(&conf,verify,NULL);
 if(ret==0)ret=mbedtls_ssl_setup(&ssl,&conf);
 if(ret==0)ret=mbedtls_ssl_set_hostname(&ssl,argv[1]);
 if(ret==0)ret=mbedtls_net_connect(&net,argv[1],"443",MBEDTLS_NET_PROTO_TCP);
 mbedtls_ssl_set_bio(&ssl,&net,mbedtls_net_send,mbedtls_net_recv,NULL);
 if(ret==0) do { ret=mbedtls_ssl_handshake(&ssl); } while(ret==MBEDTLS_ERR_SSL_WANT_READ||ret==MBEDTLS_ERR_SSL_WANT_WRITE);
 char error[200];mbedtls_strerror(ret,error,sizeof(error));
 printf("%s result=%d %s flags=0x%08x (handshake only)\n",argv[1],ret,error,(unsigned)mbedtls_ssl_get_verify_result(&ssl));
 mbedtls_ssl_close_notify(&ssl); mbedtls_net_free(&net); mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&conf);
 mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
 return ret?1:0;
}
