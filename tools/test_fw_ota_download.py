#!/usr/bin/env python3
"""Exercise actual OTA HTTP callbacks + download task with scripted SDK faults.

Only SDK/flash/task boundaries are stubbed. The real task and header policy
are compiled, including cleanup, retry, checkpoint and commit decisions.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware-ESP32-LTE-M/main'
source = (MAIN / 'fw_ota.c').read_text()
start = source.index('/* HTTP callbacks run synchronously')
end = source.index('/* --- RP2040 relay', start)
production = source[start:end]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include "fw_ota_http_policy.h"
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=2, ESP_ERR_INVALID_VERSION=3,
 ESP_ERR_OTA_VALIDATE_FAILED=4, ESP_ERR_HTTPS_OTA_IN_PROGRESS=5,
 ESP_ERR_HTTP_CONNECT=10, ESP_ERR_HTTP_CONNECTING, ESP_ERR_HTTP_WRITE_DATA,
 ESP_ERR_HTTP_FETCH_HEADER, ESP_ERR_HTTP_EAGAIN, ESP_ERR_HTTP_READ_TIMEOUT,
 ESP_ERR_HTTP_CONNECTION_CLOSED, ESP_ERR_HTTP_INCOMPLETE_DATA,
 ESP_ERR_MBEDTLS_CERT_PARTLY_OK, ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED,
 ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED,
 HTTP_EVENT_ON_STATUS_CODE, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ERROR,
 HTTP_EVENT_DISCONNECTED, HTTP_EVENT_ON_CONNECTED, HTTP_TRANSPORT_OVER_SSL };
#define FW_OTA_TIMEOUT_S 1200
#define FW_OTA_HTTP_TIMEOUT_MS 30000
#define pdMS_TO_TICKS(x) (x)
#define TAG "test"
typedef struct { const char *label; } esp_partition_t;
static const esp_partition_t part = {.label="ota_1"};
static const esp_partition_t *s_update_part = &part;
static char s_url[]="https://example.test/image?signature=not-logged";
static uint32_t s_image_len;
static uint8_t s_sha_expected[32];
typedef struct fake_client *esp_http_client_handle_t;
typedef struct {
 int event_id; esp_http_client_handle_t client; void *user_data;
 char *header_key, *header_value;
} esp_http_client_event_t;
typedef struct {
 const char *url; int timeout_ms; void (*crt_bundle_attach)(void);
 int buffer_size; bool keep_alive_enable;
 int keep_alive_idle, keep_alive_interval, keep_alive_count;
 esp_err_t (*event_handler)(esp_http_client_event_t*); void *user_data;
} esp_http_client_config_t;
typedef struct {
 const esp_http_client_config_t *http_config;
 bool ota_resumption; uint32_t ota_image_bytes_written;
 struct { const esp_partition_t *staging; } partition;
} esp_https_ota_config_t;
typedef struct fake_client *esp_https_ota_handle_t;
typedef struct { int err, written; int seconds; } step_t;
typedef struct {
 uint32_t offset; int status, begin_err, tls_flags, tls_error, tls_code, sock;
 int malformed, fallback, chunked, plaintext, complete, wrong_sha, finish_err, begin_seconds, verify_seconds;
 step_t steps[12];
} script_t;
static script_t scripts[8];
static int script_count, attempts, performs, aborts, finishes, verifies;
static int releases, rebooted, retries, deleted, clears;
static bool claimed;
static uint32_t flash_written;
static int64_t now;
static int delays[12], delay_count;
static jmp_buf task_exit;
struct fake_client {
 bool live; int index, step, written;
 esp_http_client_config_t cfg;
};
static struct fake_client client;
static script_t *current(void) { assert(client.live); return &scripts[client.index]; }
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { assert(c==&client); return current()->status; }
static int esp_http_client_get_errno(esp_http_client_handle_t c) { assert(c==&client); return current()->sock; }
static esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,int *code,int *flags) {
 assert(c==&client); *code=current()->tls_code; *flags=current()->tls_flags; return current()->tls_error;
}
static bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { assert(c==&client); return current()->chunked; }
static int esp_http_client_get_transport_type(esp_http_client_handle_t c) { assert(c==&client); return current()->plaintext ? 0 : HTTP_TRANSPORT_OVER_SSL; }
static int64_t esp_timer_get_time(void) { return now; }
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "error"; }
static void log_ignored(const char *fmt,...) { (void)fmt; }
#define ESP_LOGW(tag,...) log_ignored(__VA_ARGS__)
#define ESP_LOGI(tag,...) log_ignored(__VA_ARGS__)
#define ESP_LOGE(tag,...) log_ignored(__VA_ARGS__)
static void emit_status(const char *fmt,...) {
 assert(claimed); if(strstr(fmt,"retry")) retries++;
}
static void ota_ui_banner(const char *s) { assert(claimed); if(!s) clears++; }
static void release_in_progress(void) { assert(claimed && !client.live); claimed=false; releases++; }
static void vTaskDelay(int ms) { assert(claimed); assert(delay_count<12); delays[delay_count++]=ms; now+=(int64_t)ms*1000; }
static void esp_restart(void) { assert(claimed && !client.live && finishes==1 && verifies==1); rebooted++; longjmp(task_exit,1); }
static void vTaskDelete(void *p) { assert(!p && !claimed && !client.live); deleted++; longjmp(task_exit,1); }
static void esp_crt_bundle_attach(void) {}
static void event(int id,char *key,char *value) {
 esp_http_client_event_t e={.event_id=id,.client=&client,.user_data=client.cfg.user_data,.header_key=key,.header_value=value};
 assert(client.live && claimed); assert(client.cfg.event_handler(&e)==ESP_OK);
}
static void close_client(void) { assert(client.live); event(HTTP_EVENT_DISCONNECTED,NULL,NULL); client.live=false; }
static esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *cfg,esp_https_ota_handle_t *h) {
 assert(!client.live && claimed && attempts<script_count);
 script_t *s=&scripts[attempts];
 assert(cfg->partition.staging==&part && cfg->ota_image_bytes_written==s->offset);
 assert(cfg->ota_resumption==(s->offset!=0));
 assert(cfg->http_config->url==s_url && cfg->http_config->timeout_ms==30000);
 assert(cfg->http_config->keep_alive_enable && cfg->http_config->keep_alive_idle==60);
 assert(cfg->http_config->keep_alive_interval==10 && cfg->http_config->keep_alive_count==3);
 client=(struct fake_client){.live=true,.index=attempts++,.written=(int)s->offset,.cfg=*cfg->http_config};
 *h=NULL;
 event(HTTP_EVENT_ON_CONNECTED,NULL,NULL);
 if(s->fallback) {
  int saved=s->status; s->status=416; event(HTTP_EVENT_ON_STATUS_CODE,NULL,NULL);
  event(HTTP_EVENT_ON_HEADER,"Content-Range","bytes */4096");
  close_client(); client.live=true; s->status=saved;
  event(HTTP_EVENT_ON_CONNECTED,NULL,NULL);
 }
 event(HTTP_EVENT_ON_STATUS_CODE,NULL,NULL);
 if(s->begin_err) {
  event(HTTP_EVENT_ERROR,NULL,NULL);
  /* Actual IDF pre-connect failure frees without DISCONNECTED. */
  if(s->begin_err==ESP_ERR_HTTP_CONNECT) client.live=false; else close_client();
  return s->begin_err;
 }
 char length[32],range[80];
 snprintf(length,sizeof(length),"%u",s_image_len-s->offset);
 if(s->malformed==1) strcpy(length,"999999");
 event(HTTP_EVENT_ON_HEADER,"Content-Length",length);
 if(s->malformed==2) event(HTTP_EVENT_ON_HEADER,"Content-Length",length);
 if(s->offset && s->status==206) {
  snprintf(range,sizeof(range),"bytes %u-%u/%u",s->offset+(s->malformed==3),s_image_len-1,s_image_len);
  if(s->malformed!=4) event(HTTP_EVENT_ON_HEADER,"Content-Range",range);
 }
 if(s->malformed==5) event(HTTP_EVENT_ON_HEADER,"Content-Encoding","gzip");
 if(s->malformed==6) event(HTTP_EVENT_ON_HEADER,"Transfer-Encoding","chunked");
 now+=(int64_t)s->begin_seconds*1000000;
 *h=&client; return ESP_OK;
}
static esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h) {
 assert(h==&client && client.live && claimed); assert(client.step<12);
 script_t *s=current(); step_t step=s->steps[client.step++]; performs++;
 now+=(int64_t)step.seconds*1000000;
 if(step.err==ESP_OK || step.err==ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
  client.written=step.written;
  if(step.written>=0) flash_written=(uint32_t)step.written;
 }
 return step.err;
}
static int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h) { assert(h==&client); current(); return client.written; }
static bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h) { assert(h==&client); return current()->complete; }
static esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h) { assert(h==&client); aborts++; close_client(); return ESP_OK; }
static bool verify_written_image(const esp_partition_t *p,uint32_t n,const uint8_t *sha) {
 assert(claimed && p==&part && n==s_image_len && sha==s_sha_expected && flash_written==n);
 verifies++; now+=(int64_t)current()->verify_seconds*1000000; return !current()->wrong_sha;
}
static esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h) {
 assert(h==&client && verifies==1 && current()->complete); finishes++;
 int result=current()->finish_err; close_client(); return result;
}
'''
CASES = r'''
static int checks;
#define CHECK(x) do { checks++; assert(x); } while(0)
static void reset(void) {
 assert(!client.live); memset(scripts,0,sizeof(scripts));
 s_image_len=8192; attempts=performs=aborts=finishes=verifies=0;
 releases=rebooted=retries=deleted=clears=delay_count=0;
 flash_written=0; now=0; claimed=true; script_count=1;
 for(unsigned i=0;i<8;i++) {
  scripts[i].status=200; scripts[i].complete=1;
  scripts[i].steps[0]=(step_t){ESP_ERR_HTTPS_OTA_IN_PROGRESS,1024,1};
  scripts[i].steps[1]=(step_t){ESP_OK,8192,1};
 }
}
static void run(void) { if(!setjmp(task_exit)) { ota_task(NULL); assert(0); } }
static void success(void) { CHECK(rebooted==1 && finishes==1 && verifies==1 && releases==0 && deleted==0); }
static void failure(void) { CHECK(rebooted==0 && finishes==0 && releases==1 && deleted==1 && clears==1); }
int main(void) {
 reset(); run(); success(); CHECK(attempts==1 && aborts==0 && performs==2);
 /* Report failure at 75%: only the missing bytes are requested next. */
 reset(); script_count=2;
 scripts[0].steps[0]=(step_t){ESP_ERR_HTTPS_OTA_IN_PROGRESS,6144,1};
 scripts[0].steps[1]=(step_t){ESP_ERR_HTTP_CONNECTION_CLOSED,0,20};
 scripts[0].sock=113;
 scripts[1].offset=6144; scripts[1].status=206;
 scripts[1].steps[0]=(step_t){ESP_OK,8192,1};
 run(); success(); CHECK(attempts==2 && aborts==1 && retries==1 && delays[0]==5000);
 /* A second begin failure or first-perform failure preserves the offset. */
 for(int first_perform=0;first_perform<2;first_perform++) {
  reset(); script_count=3;
  scripts[0].steps[1]=(step_t){ESP_ERR_HTTP_CONNECTION_CLOSED,0,1};
  for(int i=1;i<3;i++) { scripts[i].offset=1024; scripts[i].status=206; }
  if(first_perform) scripts[1].steps[0]=(step_t){ESP_ERR_HTTP_CONNECTION_CLOSED,0,1};
  else scripts[1].begin_err=ESP_ERR_HTTP_CONNECT;
  run(); success(); CHECK(attempts==3 && retries==2 && delays[1]==15000);
 }
 /* Initial image-header timeout: abort and fresh begin, never same handle. */
 reset(); script_count=2; scripts[0].steps[0]=(step_t){ESP_ERR_HTTP_EAGAIN,0,30};
 run(); success(); CHECK(aborts==1 && attempts==2);
 /* Retry ceiling, cumulative backoff, no accidental verification/commit. */
 reset(); script_count=5;
 for(int i=0;i<5;i++) scripts[i].begin_err=ESP_ERR_HTTP_CONNECT;
 run(); failure(); CHECK(attempts==5 && aborts==0 && retries==4 && verifies==0);
 CHECK(delays[0]==5000 && delays[1]==15000 && delays[2]==30000 && delays[3]==60000);
 /* No progress gets a new connection after 90s. */
 reset(); script_count=2;
 for(int i=0;i<3;i++) scripts[0].steps[i]=(step_t){ESP_ERR_HTTPS_OTA_IN_PROGRESS,0,30};
 run(); success(); CHECK(attempts==2 && aborts==1);
 /* The common deadline spans attempts. */
 reset(); scripts[0].steps[0]=(step_t){ESP_ERR_HTTPS_OTA_IN_PROGRESS,1024,1200};
 run(); failure(); CHECK(attempts==1 && performs==1 && verifies==0);
 reset(); scripts[0].steps[1]=(step_t){ESP_OK,8192,1200};
 run(); failure(); CHECK(verifies==0 && finishes==0);
 reset(); scripts[0].begin_seconds=1200;
 run(); failure(); CHECK(performs==0 && verifies==0);
 reset(); scripts[0].verify_seconds=1200;
 run(); failure(); CHECK(verifies==1 && finishes==0);
 /* Reject every malformed/missing/duplicate range BEFORE any perform. */
 for(int bad=1;bad<=6;bad++) {
  reset(); script_count=2;
  scripts[0].steps[1]=(step_t){ESP_ERR_HTTP_CONNECTION_CLOSED,0,1};
  scripts[1].offset=1024; scripts[1].status=206; scripts[1].malformed=bad;
  run(); failure(); CHECK(attempts==2 && performs==2 && verifies==0 && aborts==2);
 }
 for(int bad=1;bad<=2;bad++) {
  reset(); scripts[0].chunked=(bad==1); scripts[0].plaintext=(bad==2);
  run(); failure(); CHECK(performs==0 && verifies==0);
 }
 /* The SDK's rejected-Range fallback cannot cause prefix + full-body mixing. */
 for(int fallback_status=200;fallback_status<=206;fallback_status+=6) {
  reset(); script_count=3;
  scripts[0].steps[1]=(step_t){ESP_ERR_HTTP_CONNECTION_CLOSED,0,1};
  scripts[1].offset=1024; scripts[1].status=fallback_status; scripts[1].fallback=1;
  run(); success(); CHECK(attempts==3 && aborts==2 && performs==4);
 }
 /* Same-size object changed: digest mismatch must not change the boot slot. */
 reset(); scripts[0].wrong_sha=1; run(); failure(); CHECK(verifies==1 && aborts==1 && attempts==1);
 /* Flash, image/chip and unknown errors are terminal despite errno noise. */
 int terminal[]={ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_INVALID_VERSION,ESP_ERR_OTA_VALIDATE_FAILED};
 for(unsigned i=0;i<sizeof(terminal)/sizeof(*terminal);i++) {
  reset(); scripts[0].steps[0]=(step_t){terminal[i],0,1}; scripts[0].sock=113;
  run(); failure(); CHECK(attempts==1 && verifies==0 && aborts==1);
 }
 /* Transient HTTP errors vs auth/URL errors; TLS verification is terminal. */
 int statuses[]={408,429,500,502,503,504,401,403,404,416};
 for(unsigned i=0;i<sizeof(statuses)/sizeof(*statuses);i++) {
  reset(); script_count=2; scripts[0].status=statuses[i]; scripts[0].begin_err=ESP_FAIL;
  run(); if(i<6) { success(); CHECK(attempts==2); } else { failure(); CHECK(attempts==1); }
 }
 reset(); scripts[0].begin_err=ESP_ERR_HTTP_CONNECT; scripts[0].tls_flags=4;
 run(); failure(); CHECK(attempts==1 && verifies==0);
 reset(); scripts[0].begin_err=ESP_ERR_HTTP_CONNECT; scripts[0].tls_error=ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED;
 run(); failure(); CHECK(attempts==1);
 /* Failed finish consumes its handle; never double-abort it. */
 reset(); scripts[0].finish_err=ESP_ERR_OTA_VALIDATE_FAILED; run();
 CHECK(finishes==1 && verifies==1 && aborts==0 && releases==1 && rebooted==0);
 /* Full bytes without a complete response cannot be resumed past EOF. */
 reset(); scripts[0].complete=0; run(); failure(); CHECK(attempts==1 && verifies==0);
 printf("OTA actual task: %d checks passed (transport/retry/resume/range/TLS/SHA/cleanup)\n",checks);
 return 0;
}
'''

if __name__ == '__main__':
    sanitizers = os.getenv('MQTT_TEST_SANITIZERS', 'address,undefined')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if sanitizers:
        flags += [f'-fsanitize={sanitizers}', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='wups-ota-task-') as td:
        c = Path(td) / 'test.c'; exe = Path(td) / 'test'
        c.write_text(PRELUDE + production + CASES)
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + flags + ['-I',str(MAIN),str(c),str(MAIN/'fw_ota_http_policy.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
