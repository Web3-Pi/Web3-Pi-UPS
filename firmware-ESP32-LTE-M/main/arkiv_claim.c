#include "arkiv_claim.h"
#include "arkiv_cfg.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "identity.h"
#include "cmdauth_arkiv.h"
#include "arkiv_writer.h"
#include "arkiv_rpc.h"
#include "wups_link.h"

#include "bip39.h"
#include "arkiv_crypto/keccak256.h"
#include "arkiv_crypto/secp256k1.h"

#define TAG "arkiv_claim"

/* ui.trust_prompt modes (common/protocol.h doc). */
#define TRUST_MODE_FINGERPRINT  0
#define TRUST_MODE_CLAIM_CODE   1

#define TRUST_CONFIRM_SECS      5        /* §10.1 both-button hold */
/* The human must read the OLED, eyeball it against the panel, then hold
 * for 5 s. Keep this well above the RP2040's own UX timeout. */
#define TRUST_WAIT_MS           180000
/* Re-push the claim-code screen periodically: the RP2040 may (re)boot
 * independently of the ESP32, so a one-shot display could be missed. */
#define CLAIM_CODE_REFRESH_MS   60000

#define PUB_LEN   64
#define ADDR_LEN  20
#define SIG_LEN   64

/* --- hex ------------------------------------------------------------- */

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode `hex` (optionally 0x-prefixed) into exactly out_len bytes.
 * Returns out_len on success, -1 on malformed input / size mismatch. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    if (strlen(hex) != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hexnib(hex[2 * i]), lo = hexnib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)out_len;
}

/* Owner pubkey: 64 B X||Y. Accept the canonical 64-byte form and, leniently,
 * a 65-byte 0x04-prefixed uncompressed key (strip the prefix). */
static bool decode_owner_pub(const char *hex, uint8_t pub[PUB_LEN])
{
    if (hex_decode(hex, pub, PUB_LEN) == PUB_LEN) return true;
    uint8_t tmp[65];
    if (hex_decode(hex, tmp, sizeof(tmp)) == (int)sizeof(tmp) &&
        tmp[0] == 0x04) {
        memcpy(pub, tmp + 1, PUB_LEN);
        return true;
    }
    return false;
}

/* --- cJSON attribute helpers (mirror arkiv_rpc.c) -------------------- */

static const char *str_attr(const cJSON *arr, const char *key)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "key");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value");
        if (cJSON_IsString(k) && cJSON_IsString(v) &&
            strcmp(k->valuestring, key) == 0) {
            return v->valuestring;
        }
    }
    return NULL;
}

static uint64_t num_attr(const cJSON *arr, const char *key)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "key");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value");
        if (!cJSON_IsString(k) || strcmp(k->valuestring, key) != 0) continue;
        if (cJSON_IsNumber(v)) return (uint64_t)v->valuedouble;
        if (cJSON_IsString(v) && v->valuestring)
            return strtoull(v->valuestring, NULL, 0);
    }
    return 0;
}

/* --- derivations (§10.1 / §10.4) — MUST match WS-4 byte-for-byte ----- */

/* boot_nonce: per-boot / per-UNCLAIMED-entry random, RAM only (lost on
 * reboot → fresh claim-code, exactly §10.4's "regenerated each UNCLAIMED
 * entry"). Rotated on TTL expiry. */
static uint8_t  s_boot_nonce[ARKIV_BOOT_NONCE_LEN];
static uint8_t  s_local_cc[ARKIV_CC_BYTES];      /* canonical claim-code   */
static char     s_cc_words[96];                  /* 4 BIP39 words for OLED */

static void claim_code_regen(void)
{
    esp_fill_random(s_boot_nonce, sizeof(s_boot_nonce));

    /* CC digest = keccak(device_pub[64] || boot_nonce[16]). */
    uint8_t cc[32];
    arkiv_keccak256_ctx kx;
    arkiv_keccak256_init(&kx);
    arkiv_keccak256_update(&kx, cmdauth_arkiv_device_pub(), PUB_LEN);
    arkiv_keccak256_update(&kx, s_boot_nonce, sizeof(s_boot_nonce));
    arkiv_keccak256_finish(&kx, cc);

    bip39_pack_top_bits(cc, sizeof(cc), ARKIV_CC_BITS,
                        s_local_cc, sizeof(s_local_cc));
    bip39_words_from_bits(cc, sizeof(cc), ARKIV_CC_WORDS,
                          s_cc_words, sizeof(s_cc_words));
}

/* Owner fingerprint (§10.1): keccak(owner_addr[20] || device_id_ascii).
 * Top 44 bits → 4 BIP39 words; next 16 bits → 4-hex visual checksum.
 * Bit math (must match WS-4): word bits = digest bits [0,44); checksum =
 * bits [44,60) = (d[5] low nibble)<<12 | d[6]<<4 | (d[7] high nibble). */
static void owner_fingerprint(const uint8_t owner_addr[ADDR_LEN],
                              const char *iccid, char *out, size_t out_sz)
{
    uint8_t d[32];
    arkiv_keccak256_ctx kx;
    arkiv_keccak256_init(&kx);
    arkiv_keccak256_update(&kx, owner_addr, ADDR_LEN);
    arkiv_keccak256_update(&kx, (const uint8_t *)iccid, strlen(iccid));
    arkiv_keccak256_finish(&kx, d);

    char words[64];
    bip39_words_from_bits(d, sizeof(d), ARKIV_FP_WORDS, words, sizeof(words));
    uint16_t chk = (uint16_t)(((d[5] & 0x0F) << 12) |
                              ((uint16_t)d[6] << 4) |
                              (d[7] >> 4));
    snprintf(out, out_sz, "%s chk:%04x", words, chk);
}

/* §10.4 binding digest the owner signs:
 *   keccak( "w3pups-claim\0" || device_id_ascii || owner_pub[64]
 *         || claim_code[6] || epoch_LE32 || enc_pub[64] ).
 * enc_pub (ADR-0013) is bound so a hostile gateway cannot swap the owner's
 * encryption key and read telemetry. MUST match WS-4 (claim.ts) byte-for-byte. */
static void claim_binding_digest(const char *iccid,
                                 const uint8_t owner_pub[PUB_LEN],
                                 const uint8_t cc[ARKIV_CC_BYTES],
                                 uint32_t epoch,
                                 const uint8_t enc_pub[PUB_LEN],
                                 uint8_t out[32])
{
    uint8_t ep[4] = { (uint8_t)(epoch & 0xFF), (uint8_t)((epoch >> 8) & 0xFF),
                      (uint8_t)((epoch >> 16) & 0xFF),
                      (uint8_t)((epoch >> 24) & 0xFF) };
    arkiv_keccak256_ctx kx;
    arkiv_keccak256_init(&kx);
    /* tag + its implicit NUL (13 bytes) */
    arkiv_keccak256_update(&kx, (const uint8_t *)ARKIV_CLAIM_BIND_TAG,
                           strlen(ARKIV_CLAIM_BIND_TAG) + 1);
    arkiv_keccak256_update(&kx, (const uint8_t *)iccid, strlen(iccid));
    arkiv_keccak256_update(&kx, owner_pub, PUB_LEN);
    arkiv_keccak256_update(&kx, cc, ARKIV_CC_BYTES);
    arkiv_keccak256_update(&kx, ep, sizeof(ep));
    arkiv_keccak256_update(&kx, enc_pub, PUB_LEN);
    arkiv_keccak256_finish(&kx, out);
}

static void pub_to_addr(const uint8_t pub[PUB_LEN], uint8_t addr[ADDR_LEN])
{
    uint8_t h[32];
    arkiv_keccak256(pub, PUB_LEN, h);
    memcpy(addr, h + 12, ADDR_LEN);
}

/* EIP-191 personal_sign wrap of a 32-byte digest. Browser wallets
 * (MetaMask/wagmi, WS-4) cannot raw-sign an arbitrary hash; the owner
 * signs the binding digest via personal_sign, so the device must verify
 * against keccak256("\x19Ethereum Signed Message:\n32" || digest) — the
 * exact preimage viem's signMessage({raw}) hashes for a 32-byte message. */
static void eip191_wrap(const uint8_t digest[32], uint8_t out[32])
{
    static const char PFX[] = "\x19" "Ethereum Signed Message:\n32";
    arkiv_keccak256_ctx kx;
    arkiv_keccak256_init(&kx);
    arkiv_keccak256_update(&kx, (const uint8_t *)PFX, sizeof(PFX) - 1);
    arkiv_keccak256_update(&kx, digest, 32);
    arkiv_keccak256_finish(&kx, out);
}

/* --- claim verification + trust gate --------------------------------- */

/* Verify one candidate entity and, if it checks out, run the OLED gate.
 * Returns true iff the owner was bound (→ ARKIV_CLAIMED). Fail-closed:
 * any check miss returns false (keep polling). */
static bool try_candidate(const cJSON *e, const char *iccid)
{
    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(e, "stringAttributes");
    const cJSON *na = cJSON_GetObjectItemCaseSensitive(e, "numericAttributes");
    const cJSON *writer = cJSON_GetObjectItemCaseSensitive(e, "creator");
    if (!cJSON_IsString(writer))
        writer = cJSON_GetObjectItemCaseSensitive(e, "owner");

    const char *did      = str_attr(sa, ARKIV_ATTR_DEVICE_ID);
    const char *opub_hex = str_attr(sa, ARKIV_ATTR_OWNER_PUB);
    const char *epub_hex = str_attr(sa, ARKIV_ATTR_ENC_PUB);
    const char *cc_hex   = str_attr(sa, ARKIV_ATTR_CLAIM_CODE);
    const char *sig_hex  = str_attr(sa, ARKIV_ATTR_SIG);
    if (!did || !opub_hex || !epub_hex || !cc_hex || !sig_hex ||
        !cJSON_IsString(writer))
        return false;

    /* 1. device_id is ours (the query already filters, defence in depth). */
    if (strcmp(did, iccid) != 0) return false;

    /* 2. claim-code must equal the one only someone who physically saw the
     *    OLED could have transcribed (§10.4 front-running defence). */
    uint8_t cc_b[ARKIV_CC_BYTES];
    if (hex_decode(cc_hex, cc_b, sizeof(cc_b)) != (int)sizeof(cc_b)) return false;
    if (memcmp(cc_b, s_local_cc, sizeof(cc_b)) != 0) {
        ESP_LOGW(TAG, "claim-code mismatch — ignoring (stale/front-run?)");
        return false;
    }

    uint8_t owner_pub[PUB_LEN], enc_pub[PUB_LEN], sig[SIG_LEN], writer_b[ADDR_LEN];
    if (!decode_owner_pub(opub_hex, owner_pub)) return false;
    if (!decode_owner_pub(epub_hex, enc_pub)) return false;
    if (hex_decode(sig_hex, sig, SIG_LEN) != SIG_LEN) return false;
    if (hex_decode(writer->valuestring, writer_b, ADDR_LEN) != ADDR_LEN)
        return false;

    /* Reject an off-curve / infinity owner or encryption key before trusting
     * it (ADR-0013): a bad enc_pub would otherwise poison every K_dir. */
    if (arkiv_secp256k1_valid_pubkey(owner_pub) != 1 ||
        arkiv_secp256k1_valid_pubkey(enc_pub) != 1) {
        ESP_LOGW(TAG, "owner_pub/enc_pub not a valid secp256k1 point — ignoring");
        return false;
    }

    uint32_t epoch = (uint32_t)num_attr(na, ARKIV_ATTR_EPOCH);

    /* 3. The owner key itself must have signed the binding for THIS device
     *    (so a lying RPC that forged `writer` still can't bind anyone). The
     *    digest binds enc_pub too, so the encryption key can't be swapped. */
    uint8_t digest[32], signed_digest[32];
    claim_binding_digest(iccid, owner_pub, s_local_cc, epoch, enc_pub, digest);
    eip191_wrap(digest, signed_digest);  /* owner used personal_sign (WS-4) */
    if (arkiv_secp256k1_verify(owner_pub, signed_digest, sig) != 1) {
        ESP_LOGW(TAG, "owner claim signature INVALID — ignoring");
        return false;
    }

    /* 4. Defence in depth: on-chain writer must be addr(owner_pub). */
    uint8_t owner_addr[ADDR_LEN];
    pub_to_addr(owner_pub, owner_addr);
    if (memcmp(writer_b, owner_addr, ADDR_LEN) != 0) {
        ESP_LOGW(TAG, "writer != addr(owner_pub) — ignoring");
        return false;
    }

    /* 5. §10.1 physical trust anchor: show the fingerprint, wait for the
     *    human 2-button confirm. The RP2040 only renders + reports. */
    char fp[80];
    owner_fingerprint(owner_addr, iccid, fp, sizeof(fp));
    uint32_t nonce = esp_random();
    ESP_LOGI(TAG, "claim candidate verified — OLED fingerprint: %s", fp);
    wups_link_trust_prompt(TRUST_MODE_FINGERPRINT, TRUST_CONFIRM_SECS,
                           nonce, fp);

    uint8_t result = 1;
    esp_err_t w = wups_link_trust_wait(nonce, TRUST_WAIT_MS, &result);
    if (w != ESP_OK || result != 0) {
        ESP_LOGW(TAG, "trust gate not confirmed (w=%s result=%u) — staying "
                 "UNCLAIMED", esp_err_to_name(w), result);
        return false;
    }

    /* 6. Confirmed → bind. cmdauth_arkiv_bind_owner persists owner_pub +
     *    enc_pub + epoch, resets the per-owner counter, sets ARKIV_CLAIMED. */
    esp_err_t b = cmdauth_arkiv_bind_owner(owner_pub, enc_pub, epoch);
    if (b != ESP_OK) {
        ESP_LOGE(TAG, "bind_owner failed: %s — staying UNCLAIMED",
                 esp_err_to_name(b));
        return false;
    }
    /* Pre-derive the payload-seal K_dir now, on this 8 KB claim task, so the
     * first ack/event from the 4 KB wups_rx task only runs GCM (ADR-0013). */
    if (!arkiv_writer_seal_prewarm()) {
        ESP_LOGW(TAG, "seal prewarm failed — keys will derive lazily on first emit");
    }
    ESP_LOGI(TAG, "owner bound via OLED trust anchor (epoch=%u) — "
             "ARKIV_CLAIMED", (unsigned)epoch);
    return true;
}

static bool poll_once(const char *iccid)
{
    static char resp[ARKIV_RPC_RESP_CAP];

    char filter[160];
    int fn = snprintf(filter, sizeof(filter),
        "%s = \\\"%s\\\" && %s = \\\"%s\\\"",
        ARKIV_ATTR_TYPE, ARKIV_CLAIM_ENTITY_TYPE,
        ARKIV_ATTR_DEVICE_ID, iccid);
    if (fn <= 0 || fn >= (int)sizeof(filter)) return false;
    if (arkiv_rpc_query(filter, resp, sizeof(resp)) != ESP_OK) return false;

    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG, "claim rpc json parse failed"); return false; }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *data = result
        ? cJSON_GetObjectItemCaseSensitive(result, "data") : NULL;

    bool bound = false;
    if (cJSON_IsArray(data)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, data) {
            if (try_candidate(e, iccid)) { bound = true; break; }
        }
    }
    cJSON_Delete(root);
    return bound;
}

/* --- task ------------------------------------------------------------ */

static void claim_task(void *arg)
{
    (void)arg;
    bool    cc_shown   = false;
    int64_t cc_gen_ts  = 0;   /* last claim-code (re)generation, ms */
    int64_t cc_disp_ts = 0;   /* last OLED claim-code (re)send,    ms */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ARKIV_CLAIM_POLL_MS));

        if (!cmdauth_arkiv_ready()) { cc_shown = false; continue; }
        if (cmdauth_arkiv_claim_state() != ARKIV_UNCLAIMED) {
            /* Bound (or downgraded) elsewhere — nothing to do anymore. */
            cc_shown = false;
            continue;
        }
        const char *iccid = identity_iccid();
        if (!iccid || iccid[0] == '\0') continue;

        int64_t now = esp_timer_get_time() / 1000; /* ms */

        /* Rotate the claim-code on first pass or TTL expiry (§10.4 /
         * §10.9-2); resend the (unchanged) screen every refresh window
         * so a possibly-rebooted OLED still has it. Display-only prompt
         * (confirm_secs = 0 → no result expected). */
        bool rotate = !cc_shown || (now - cc_gen_ts) > ARKIV_CLAIM_CODE_TTL_MS;
        if (rotate) {
            claim_code_regen();
            cc_gen_ts = now;
            cc_shown  = true;
            ESP_LOGI(TAG, "claim-code (OLED): %s", s_cc_words);
        }
        if (rotate || (now - cc_disp_ts) > CLAIM_CODE_REFRESH_MS) {
            char screen[128];
            snprintf(screen, sizeof(screen), "ID:%s\n%s", iccid, s_cc_words);
            wups_link_trust_prompt(TRUST_MODE_CLAIM_CODE, 0,
                                   esp_random(), screen);
            cc_disp_ts = now;
        }

        if (poll_once(iccid)) {
            ESP_LOGI(TAG, "owner-binding complete — claim task idle");
            /* Loop continues but self-gates out (state != UNCLAIMED);
             * the w3pups-cmd poll task takes over from here. */
        }
    }
}

void arkiv_claim_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* 8 KB: TLS + cJSON over the response + secp256k1 verify. */
    xTaskCreate(claim_task, "arkiv_claim", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "Arkiv claim task started (poll=%dms)",
             ARKIV_CLAIM_POLL_MS);
}
