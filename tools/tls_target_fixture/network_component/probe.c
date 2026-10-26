/* Explicitly opt-in read-only acceptance task. Not part of release builds.
 * No backend-mode change, on-chain write, provision update or TLS override. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "modem.h"
#include "arkiv_rpc.h"
#include "arkiv_ws_token.h"
#include "tls_trust.h"
static atomic_bool ws_up,ws_result;
static char response[8192];
static size_t response_len;
static void ws_event(void *arg,esp_event_base_t base,int32_t id,void *event) {
    (void)arg; (void)base;
    if(id==WEBSOCKET_EVENT_CONNECTED) { atomic_store(&ws_up,true); return; }
    if(id!=WEBSOCKET_EVENT_DATA) return;
    esp_websocket_event_data_t *d=event;
    if(d->op_code!=1 && d->op_code!=0) return;
    if(d->payload_offset==0) response_len=0;
    if(d->data_len<0 || response_len+(size_t)d->data_len>=sizeof(response)) return;
    memcpy(response+response_len,d->data_ptr,d->data_len); response_len+=d->data_len;
    response[response_len]=0;
    if((size_t)d->payload_offset+d->data_len < (size_t)d->payload_len) return;
    cJSON *json=cJSON_Parse(response);
    if(json) {
        cJSON *rid=cJSON_GetObjectItemCaseSensitive(json,"id");
        cJSON *result=cJSON_GetObjectItemCaseSensitive(json,"result");
        if(cJSON_IsNumber(rid)&&rid->valueint==7143&&cJSON_IsString(result)&&result->valuestring[0]) atomic_store(&ws_result,true);
        cJSON_Delete(json);
    }
}
static void probe(void *unused) {
    (void)unused;
    int64_t end=esp_timer_get_time()+240000000;
    while((!modem_ppp_is_up() || time(NULL)<1767225600) && esp_timer_get_time()<end) vTaskDelay(pdMS_TO_TICKS(1000));
    if(!modem_ppp_is_up() || time(NULL)<1767225600) { puts("ACCEPTANCE COMPLETE FAIL reason=no_ppp_or_clock"); vTaskDelete(NULL); return; }
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_http_client_config_t http={.url="https://panel.web3pi.io/",.method=HTTP_METHOD_HEAD,
        .crt_bundle_attach=wups_crt_bundle_attach,.timeout_ms=30000,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&http);
    esp_err_t er=client?esp_http_client_perform(client):ESP_FAIL;
    int status=client?esp_http_client_get_status_code(client):-1;
    bool http_ok=er==ESP_OK&&status==200;
    printf("ACCEPTANCE https_panel=%s err=%d status=%d\n",http_ok?"PASS":"FAIL",er,status);
    if(client) esp_http_client_cleanup(client);
    uint64_t block=0; er=arkiv_eth_block_number(&block);
    bool rpc_ok=er==ESP_OK&&block>0;
    printf("ACCEPTANCE arkiv_rpc=%s err=%d block=%" PRIu64 "\n",rpc_ok?"PASS":"FAIL",er,block);
    esp_websocket_client_config_t wc={.uri="wss://arkiv.web3pi.io/ws/" ARKIV_WS_TOKEN,
        .crt_bundle_attach=wups_crt_bundle_attach,.disable_auto_reconnect=true,.network_timeout_ms=30000,
        .buffer_size=2048,.task_stack=6144};
    esp_websocket_client_handle_t ws=esp_websocket_client_init(&wc);
    if(ws) {
        esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,ws_event,NULL);
        er=esp_websocket_client_start(ws);
        end=esp_timer_get_time()+60000000;
        while(!atomic_load(&ws_up)&&esp_timer_get_time()<end) vTaskDelay(pdMS_TO_TICKS(100));
        if(atomic_load(&ws_up)) {
            const char *query="{\"jsonrpc\":\"2.0\",\"id\":7143,\"method\":\"eth_blockNumber\",\"params\":[]}";
            esp_websocket_client_send_text(ws,query,strlen(query),pdMS_TO_TICKS(10000));
            end=esp_timer_get_time()+30000000;
            while(!atomic_load(&ws_result)&&esp_timer_get_time()<end) vTaskDelay(pdMS_TO_TICKS(100));
        }
        printf("ACCEPTANCE arkiv_wss=%s connected=%d response=%d\n",atomic_load(&ws_result)?"PASS":"FAIL",atomic_load(&ws_up),atomic_load(&ws_result));
        esp_websocket_client_stop(ws); esp_websocket_client_destroy(ws);
    }
    printf("ACCEPTANCE COMPLETE %s\n",http_ok&&rpc_ok&&atomic_load(&ws_result)?"PASS":"FAIL");
    vTaskDelete(NULL);
}
void __real_app_main(void);
void __wrap_app_main(void) {
    if(xTaskCreate(probe,"tls_net_probe",8192,NULL,3,NULL)!=pdPASS) abort();
    __real_app_main();
}
