/* On-target real mbedTLS handshakes through bounded in-memory BIO queues.
 * Only this isolated, network-free app installs disposable test roots.
 * Production tls_trust.c is linked unchanged. No verification mocks. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "tls_trust.h"
typedef struct { const char *name; const unsigned char *pem; size_t len; const char *host; uint32_t flag; } test_t;
#include "fixtures.h"
static unsigned failures;
#define OK(call) do { int e=(call); if(e) { printf("TLS_FIXTURE FATAL %s rc=%d line=%d\n",#call,e,__LINE__); abort(); } } while(0)
typedef struct { unsigned char bytes[32768]; size_t used; } queue_t;
typedef struct { queue_t *rx,*tx; } bio_t;
static int send_bytes(void *p,const unsigned char *buf,size_t n) {
    bio_t *b=p; if(n>sizeof(b->tx->bytes)-b->tx->used) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(b->tx->bytes+b->tx->used,buf,n); b->tx->used+=n; return (int)n;
}
static int recv_bytes(void *p,unsigned char *buf,size_t n) {
    bio_t *b=p; if(!b->rx->used) return MBEDTLS_ERR_SSL_WANT_READ;
    if(n>b->rx->used) n=b->rx->used;
    memcpy(buf,b->rx->bytes,n);
    b->rx->used-=n; memmove(b->rx->bytes,b->rx->bytes+n,b->rx->used); return (int)n;
}
static void handshake(const test_t *t) {
    mbedtls_ssl_context client,server; mbedtls_ssl_config cc,sc;
    mbedtls_x509_crt chain; mbedtls_pk_context key;
    mbedtls_ssl_init(&client); mbedtls_ssl_init(&server);
    mbedtls_ssl_config_init(&cc); mbedtls_ssl_config_init(&sc);
    mbedtls_x509_crt_init(&chain); mbedtls_pk_init(&key);
    OK(mbedtls_x509_crt_parse(&chain,t->pem,t->len));
    OK(mbedtls_pk_parse_key(&key,server_key,sizeof(server_key),NULL,0));
    OK(mbedtls_ssl_config_defaults(&cc,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
    OK(mbedtls_ssl_config_defaults(&sc,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_authmode(&cc,MBEDTLS_SSL_VERIFY_REQUIRED);
    OK(wups_crt_bundle_attach(&cc)); OK(mbedtls_ssl_conf_own_cert(&sc,&chain,&key));
    OK(mbedtls_ssl_setup(&client,&cc)); OK(mbedtls_ssl_setup(&server,&sc));
    OK(mbedtls_ssl_set_hostname(&client,t->host));
    queue_t *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)); if(!a||!b) abort();
    bio_t cb={a,b},sb={b,a}; mbedtls_ssl_set_bio(&client,&cb,send_bytes,recv_bytes,NULL);
    mbedtls_ssl_set_bio(&server,&sb,send_bytes,recv_bytes,NULL);
    int cr=0,sr=0; int64_t until=esp_timer_get_time()+15000000;
    while(esp_timer_get_time()<until) {
        if(!mbedtls_ssl_is_handshake_over(&client)) cr=mbedtls_ssl_handshake(&client);
        if(cr && cr!=MBEDTLS_ERR_SSL_WANT_READ && cr!=MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if(!mbedtls_ssl_is_handshake_over(&server)) sr=mbedtls_ssl_handshake(&server);
        if(sr && sr!=MBEDTLS_ERR_SSL_WANT_READ && sr!=MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if(mbedtls_ssl_is_handshake_over(&client)&&mbedtls_ssl_is_handshake_over(&server)) break;
        vTaskDelay(1);
    }
    uint32_t flags=mbedtls_ssl_get_verify_result(&client);
    bool connected=mbedtls_ssl_is_handshake_over(&client)&&mbedtls_ssl_is_handshake_over(&server);
    /* mbedTLS 4 maps a nonzero verification-callback return to FATAL_ERROR
     * and an undefined all-ones verify result. SDK bundle rejects unknown
     * roots/bad signatures this way; date/name errors retain precise flags. */
    bool rejected=cr==MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && (flags&t->flag);
    if(t->flag==MBEDTLS_X509_BADCERT_NOT_TRUSTED &&
       cr==MBEDTLS_ERR_X509_FATAL_ERROR && flags==UINT32_MAX) rejected=true;
    bool pass=t->flag ? (!connected && rejected) : (connected && flags==0);
    if(!pass) failures++;
    printf("TLS_FIXTURE handshake=%s result=%s connected=%d client_rc=%d server_rc=%d flags=0x%08" PRIx32 "\n",t->name,pass?"PASS":"FAIL",connected,cr,sr,flags);
    mbedtls_ssl_free(&client); mbedtls_ssl_free(&server); mbedtls_ssl_config_free(&cc); mbedtls_ssl_config_free(&sc);
    mbedtls_x509_crt_free(&chain); mbedtls_pk_free(&key); free(a); free(b);
}
static int verify(const test_t *t,bool guarded,uint32_t *flags) {
    mbedtls_ssl_config conf; mbedtls_x509_crt cert;
    mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&cert);
    OK(mbedtls_x509_crt_parse(&cert,t->pem,t->len));
    OK(guarded?wups_crt_bundle_attach(&conf):esp_crt_bundle_attach(&conf));
    int rc=mbedtls_x509_crt_verify(&cert,conf.MBEDTLS_PRIVATE(ca_chain),NULL,t->host,flags,conf.MBEDTLS_PRIVATE(f_vrfy),conf.MBEDTLS_PRIVATE(p_vrfy));
    mbedtls_x509_crt_free(&cert); mbedtls_ssl_config_free(&conf); return rc;
}
static void run(void *unused) {
    (void)unused;
    struct timeval tv={.tv_sec=1793016000}; settimeofday(&tv,NULL);
    OK(psa_crypto_init()); OK(esp_crt_bundle_set(test_bundle,sizeof(test_bundle)));
#ifdef MBEDTLS_HAVE_TIME_DATE
    puts("TLS_FIXTURE date_validation=enabled");
#else
    puts("TLS_FIXTURE date_validation=DISABLED");
#endif
    for(size_t i=0;i<sizeof(handshake_cases)/sizeof(handshake_cases[0]);i++) handshake(&handshake_cases[i]);
    for(size_t i=0;i<sizeof(denied_cases)/sizeof(denied_cases[0]);i++) {
        uint32_t before=0,after=0;
        int base=verify(&denied_cases[i],false,&before),guard=verify(&denied_cases[i],true,&after);
        bool pass=base==0 && before==0 && guard!=0 && (after&MBEDTLS_X509_BADCERT_REVOKED);
        if(!pass) failures++;
        printf("TLS_FIXTURE blocked_key=%s result=%s baseline_rc=%d baseline_flags=0x%08" PRIx32 " guarded_rc=%d guarded_flags=0x%08" PRIx32 "\n",denied_cases[i].name,pass?"PASS":"FAIL",base,before,guard,after);
    }
    esp_crt_bundle_detach(NULL);
    printf("TLS_FIXTURE COMPLETE failures=%u handshakes=8 blocked_key_chains=16\n",failures);
    for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
void app_main(void) { xTaskCreate(run,"tls_acceptance",16384,NULL,5,NULL); }
