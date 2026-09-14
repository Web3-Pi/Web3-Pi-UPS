#!/usr/bin/env python3
"""Exercise the shipped init/register/publication suffix with a concurrent reader.

The SDK allocation/registration/task creation are fault-injection stubs. This
checks lifetime ownership, not ESP32 allocation pressure or hardware timing.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "firmware-ESP32-LTE-M/main/mqtt.c").read_text()
start = source.index("    esp_mqtt_client_handle_t client = atomic_load_explicit")
guard_end = source.index('    esp_log_level_set(', start)
guard = source[start:guard_end]
start = source.index("    client = esp_mqtt_client_init(&cfg);")
end = source.index("\n}\n", start)
suffix = source[start:end]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_EVENT_ANY_ID 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define mqtt_event_handler NULL
typedef int esp_err_t;
typedef int esp_mqtt_client_config_t;
typedef struct Client { bool registered; } *esp_mqtt_client_handle_t;
static _Atomic(esp_mqtt_client_handle_t) s_client;
static bool fail_init, fail_registration, fail_start;
static int allocations, registrations, starts, destroys;
static void *producer(void *unused) {
    (void)unused;
    /* Registration has not returned: no producer may acquire this object. */
    assert(atomic_load_explicit(&s_client, memory_order_acquire) == NULL);
    return NULL;
}
static esp_mqtt_client_handle_t esp_mqtt_client_init(const void *cfg) {
    (void)cfg;
    if (fail_init) return NULL;
    esp_mqtt_client_handle_t c = calloc(1, sizeof(*c));
    assert(c); allocations++; return c;
}
static int esp_mqtt_client_register_event(esp_mqtt_client_handle_t c,
                                         int id, void *handler, void *arg) {
    (void)id; (void)handler; (void)arg;
    pthread_t t; assert(!pthread_create(&t, NULL, producer, NULL));
    assert(!pthread_join(t, NULL));
    registrations++;
    if (fail_registration) return ESP_FAIL;
    c->registered = true; return ESP_OK;
}
static void esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
    assert(atomic_load(&s_client) != c); destroys++; free(c);
}
static int esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
    assert(c && c->registered && atomic_load(&s_client) == c);
    starts++; return fail_start ? ESP_FAIL : ESP_OK;
}
static int test_start(void) {
    esp_mqtt_client_config_t cfg = 0;
''' + guard + suffix + r'''
}
int main(void) {
    fail_init = true;
    assert(test_start() != ESP_OK && !atomic_load(&s_client));
    fail_init = false; fail_registration = true;
    for (int i = 0; i < 100; ++i) {
        assert(test_start() != ESP_OK && !atomic_load(&s_client));
        assert(allocations == destroys);
    }
    fail_registration = false; fail_start = true;
    assert(test_start() != ESP_OK && atomic_load(&s_client));
    int saved_allocations = allocations, saved_registrations = registrations;
    esp_mqtt_client_handle_t c = atomic_load(&s_client);
    assert(test_start() != ESP_OK && atomic_load(&s_client) == c);
    fail_start = false;
    assert(test_start() == ESP_OK && atomic_load(&s_client) == c);
    assert(allocations == saved_allocations && registrations == saved_registrations);
    assert(starts == 3 && allocations == destroys + 1);
    atomic_store(&s_client, NULL); free(c); /* Test teardown after all users joined. */
    puts("PASS: private registration, 100 failed-registration interleavings, allocation failure, start failure and same-client retry");
}
'''
with tempfile.TemporaryDirectory(prefix="mqtt-setup-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(code)
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    flags = ["-fsanitize=" + sanitizers] if sanitizers else []
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-pthread", *flags,
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"})
