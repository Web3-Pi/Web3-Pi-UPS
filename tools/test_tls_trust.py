#!/usr/bin/env python3
"""Exercise the actual trust wrapper; SDK callback/TLS transport are stubs.
No hardware or complete TLS handshake is claimed by this regression test.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware-ESP32-LTE-M/main"
manifest = json.loads((MAIN.parent / "certs/manifest.json").read_text())
with tempfile.TemporaryDirectory(prefix="wups-tls-trust-") as d:
    out = Path(d)
    (out / "mbedtls").mkdir()
    (out / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 258\n")
    (out / "mbedtls/x509_crt.h").write_text("""#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { unsigned char *p; size_t len; } mbedtls_x509_buf;
typedef struct { mbedtls_x509_buf pk_raw; } mbedtls_x509_crt;
#define MBEDTLS_ERR_X509_FATAL_ERROR -1
#define MBEDTLS_ERR_X509_CERT_VERIFY_FAILED -2
#define MBEDTLS_X509_BADCERT_REVOKED 2
""")
    (out / "mbedtls/ssl.h").write_text("""#pragma once
#include "x509_crt.h"
typedef int (*verify_fn)(void *, mbedtls_x509_crt *, int, uint32_t *);
typedef struct { verify_fn verify; void *ctx; } mbedtls_ssl_config;
void mbedtls_ssl_conf_verify(mbedtls_ssl_config *, verify_fn, void *);
""")
    (out / "esp_crt_bundle.h").write_text('#pragma once\n#include "esp_err.h"\nesp_err_t esp_crt_bundle_attach(void *conf);\n')
    keys = "\n".join("    {" + ",".join(str(b) for b in bytes.fromhex(k["spki_der_hex"])) + "}," for k in manifest["distrusted_keys"])
    c = """#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tls_trust.h"
#include "mbedtls/ssl.h"
static int delegate_calls, attach_result, delegate_result;
static unsigned char keys[8][120] = {
KEYS
};
void mbedtls_ssl_conf_verify(mbedtls_ssl_config *s, verify_fn f, void *ctx) { s->verify=f; s->ctx=ctx; }
int esp_crt_bundle_attach(void *conf) { (void)conf; return attach_result; }
int esp_crt_verify_callback(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx; (void)crt; (void)depth; (void)flags;
    delegate_calls++; return delegate_result;
}
int main(void) {
    mbedtls_ssl_config s = {0};
    assert(wups_crt_bundle_attach(NULL) == ESP_ERR_INVALID_ARG);
    attach_result = -99;
    assert(wups_crt_bundle_attach(&s) == -99 && s.verify == NULL);
    attach_result = 0;
    assert(wups_crt_bundle_attach(&s) == 0 && s.verify != NULL);
    for (int key=0; key<8; key++) {
        for (int depth=0; depth<5; depth++) {
            /* Same SPKI is rejected at every depth, regardless of issuer
             * or callback flags (including a previously valid chain). */
            for (uint32_t initial=0; initial<16; initial++) {
                uint32_t flags = initial;
                mbedtls_x509_crt crt = {.pk_raw={keys[key],120}};
                int before = delegate_calls;
                assert(s.verify(s.ctx,&crt,depth,&flags) != 0);
                assert(flags & MBEDTLS_X509_BADCERT_REVOKED);
                assert(delegate_calls == before);
            }
        }
    }
    unsigned char other[120]; memcpy(other, keys[0], 120); other[119] ^= 1;
    mbedtls_x509_crt crt = {.pk_raw={other,120}};
    uint32_t flags = 0;
    assert(s.verify(s.ctx,&crt,0,&flags) == 0 && delegate_calls == 1);
    /* An unrelated key still undergoes the original SDK checks. */
    delegate_result = -77; flags = 0x81;
    assert(s.verify(s.ctx,&crt,0,&flags) == -77 && flags == 0x81);
    assert(s.verify(s.ctx,NULL,0,&flags) != 0);
    assert(s.verify(s.ctx,&crt,0,NULL) != 0);
    puts("TLS wrapper PASS: 640 denied-key/depth/flags cases; SDK errors preserved; attach fails closed");
}
""".replace("KEYS", keys)
    (out / "test.c").write_text(c)
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I"+str(out), "-I"+str(MAIN), str(out / "test.c"), str(MAIN / "tls_trust.c"), "-o", str(out / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(out / "test")], check=True)
