#include "cmdauth_arkiv.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "arkiv_crypto/secp256k1.h"
#include "arkiv_crypto/keccak256.h"
#include "arkiv_cfg.h"

#define TAG "cmdauth_arkiv"

/* prov (read-only, provisioned) — shared namespace with Track 0/1. */
#define PROV_PARTITION  "prov"
#define PROV_NAMESPACE  "w3pups"
#define KEY_DEV_PRIV    "ak_dev_priv"   /* 32 B secp256k1 device private key */

/* writable runtime state (default `nvs`) — separate from WS-9's w3wsec. */
#define STATE_NAMESPACE "w3arkiv"
#define KEY_OWNER_PUB   "owner_pub"     /* 64 B X||Y — bound at OLED confirm */
#define KEY_KEY_EPOCH   "key_epoch"     /* u32 — revocation/ratchet (§4.5)   */
#define KEY_LAST_CTR    "last_ctr"      /* u64 — monotonic replay baseline   */
#define KEY_CUR_BLOCK   "cur_block"     /* u64 — Braga cursor (§4.4)         */
#define KEY_CLAIM_STATE "claim_state"   /* u8  — arkiv_claim_state_t         */

#define ADDR_LEN 20
#define PUB_LEN  64

static bool                s_ready;
static uint8_t             s_dev_addr[ADDR_LEN];
static uint8_t             s_dev_pub[PUB_LEN];
static uint8_t             s_owner_pub[PUB_LEN];
static uint8_t             s_owner_addr[ADDR_LEN]; /* derived from owner_pub */
static bool                s_have_owner;

/* Ethereum address from an uncompressed pubkey: keccak256(X||Y)[12:32]. */
static void pub_to_addr(const uint8_t pub[PUB_LEN], uint8_t addr[ADDR_LEN])
{
    uint8_t h[32];
    arkiv_keccak256(pub, PUB_LEN, h);
    memcpy(addr, h + 12, ADDR_LEN);
}
static uint32_t            s_key_epoch;
static uint64_t            s_last_ctr;
static uint64_t            s_cur_block;
static arkiv_claim_state_t s_claim_state;

static esp_err_t state_open(nvs_handle_t *h, nvs_open_mode_t mode)
{
    return nvs_open(STATE_NAMESPACE, mode, h);
}

esp_err_t cmdauth_arkiv_init(void)
{
    s_ready = false;
    s_have_owner = false;
    s_claim_state = ARKIV_UNCLAIMED;

    /* prov is brought up by identity_init(); this is idempotent. */
    esp_err_t err = nvs_flash_init_partition(PROV_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t ph;
    err = nvs_open_from_partition(PROV_PARTITION, PROV_NAMESPACE,
                                  NVS_READONLY, &ph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t dev_priv[ARKIV_SECP256K1_PRIVKEY_LEN];
    size_t plen = sizeof(dev_priv);
    err = nvs_get_blob(ph, KEY_DEV_PRIV, dev_priv, &plen);
    nvs_close(ph);
    if (err != ESP_OK || plen != ARKIV_SECP256K1_PRIVKEY_LEN) {
        ESP_LOGW(TAG, "%s absent (%s) — device not Arkiv-provisioned; "
                 "Paranoic mode unavailable", KEY_DEV_PRIV,
                 esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (arkiv_secp256k1_derive_address(dev_priv, s_dev_addr) != 0 ||
        arkiv_secp256k1_derive_pubkey(dev_priv, s_dev_pub) != 0) {
        memset(dev_priv, 0, sizeof(dev_priv));
        ESP_LOGE(TAG, "device key derivation failed");
        return ESP_FAIL;
    }
    memset(dev_priv, 0, sizeof(dev_priv)); /* don't leave the key on stack */

    /* Writable Arkiv state. Absent owner = UNCLAIMED (valid, not an error):
     * the owner is bound later via the physical OLED gate (§10.1). */
    nvs_handle_t sh;
    err = state_open(&sh, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state open failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t olen = sizeof(s_owner_pub);
    if (nvs_get_blob(sh, KEY_OWNER_PUB, s_owner_pub, &olen) == ESP_OK &&
        olen == PUB_LEN) {
        pub_to_addr(s_owner_pub, s_owner_addr);
        s_have_owner = true;
    }
    if (nvs_get_u32(sh, KEY_KEY_EPOCH, &s_key_epoch) != ESP_OK) s_key_epoch = 0;
    if (nvs_get_u64(sh, KEY_LAST_CTR, &s_last_ctr) != ESP_OK) s_last_ctr = 0;
    if (nvs_get_u64(sh, KEY_CUR_BLOCK, &s_cur_block) != ESP_OK) s_cur_block = 0;
    uint8_t cs = ARKIV_UNCLAIMED;
    nvs_get_u8(sh, KEY_CLAIM_STATE, &cs);
    nvs_close(sh);
    s_claim_state = (arkiv_claim_state_t)cs;

    s_ready = true;
    ESP_LOGI(TAG, "Arkiv ready (state=%d, owner=%s, epoch=%u, ctr=%llu, "
             "block=%llu)", (int)s_claim_state, s_have_owner ? "bound" : "none",
             (unsigned)s_key_epoch, (unsigned long long)s_last_ctr,
             (unsigned long long)s_cur_block);
    return ESP_OK;
}

bool cmdauth_arkiv_ready(void) { return s_ready; }
arkiv_claim_state_t cmdauth_arkiv_claim_state(void) { return s_claim_state; }
const uint8_t *cmdauth_arkiv_device_addr(void) { return s_dev_addr; }
const uint8_t *cmdauth_arkiv_device_pub(void) { return s_dev_pub; }
uint64_t cmdauth_arkiv_cursor_block(void) { return s_cur_block; }

static bool persist_progress(uint64_t ctr, uint64_t block)
{
    nvs_handle_t sh;
    if (state_open(&sh, NVS_READWRITE) != ESP_OK) return false;
    esp_err_t e = nvs_set_u64(sh, KEY_LAST_CTR, ctr);
    if (e == ESP_OK) e = nvs_set_u64(sh, KEY_CUR_BLOCK, block);
    if (e == ESP_OK) e = nvs_commit(sh);
    nvs_close(sh);
    return e == ESP_OK;
}

bool cmdauth_arkiv_check(const arkiv_cmd_t *cmd,
                         const uint8_t **frame, size_t *frame_len)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "not initialised — rejecting");
        return false;
    }
    if (s_claim_state != ARKIV_CLAIMED || !s_have_owner) {
        ESP_LOGW(TAG, "no owner bound (state=%d) — rejecting Arkiv command",
                 (int)s_claim_state);
        return false;
    }
    if (!cmd || !cmd->frame || cmd->frame_len == 0 || !cmd->sig) {
        ESP_LOGW(TAG, "malformed command struct");
        return false;
    }

    /* §4.3 provenance: the entity's on-chain writer MUST be the registered
     * owner. This is the Arkiv-native authority check. */
    if (memcmp(cmd->writer, s_owner_addr, ADDR_LEN) != 0) {
        ESP_LOGW(TAG, "writer != registered owner — dropping");
        return false;
    }
    /* Revocation/ratchet: only the current epoch is accepted. A higher epoch
     * needs an owner-signed bump first (cmdauth_arkiv_set_epoch); lower = a
     * rollback attempt. */
    if (cmd->epoch != s_key_epoch) {
        ESP_LOGW(TAG, "epoch %u != cur %u — dropping",
                 (unsigned)cmd->epoch, (unsigned)s_key_epoch);
        return false;
    }
    /* Freshness baseline: strictly monotonic counter (no clock needed). The
     * Braga block is the §4.4 clock/cursor, advanced on success. */
    if (cmd->counter <= s_last_ctr) {
        ESP_LOGW(TAG, "replay: counter %llu <= last %llu",
                 (unsigned long long)cmd->counter,
                 (unsigned long long)s_last_ctr);
        return false;
    }
    if (cmd->block < s_cur_block) {
        ESP_LOGW(TAG, "stale block %llu < cursor %llu — dropping",
                 (unsigned long long)cmd->block,
                 (unsigned long long)s_cur_block);
        return false;
    }

    /* Defense-in-depth beyond the Arkiv-reported writer: the command must
     * carry a valid OWNER signature over a digest that binds the frame to
     * (device_id, epoch, seq, command_id). The binding is what stops a
     * hostile gateway from re-publishing the same (frame, sig) with a
     * swapped `seq`/`epoch` attribute and causing double execution — the
     * device's `counter > last_ctr` check alone would otherwise let two
     * different seq values for the same owner-signed frame both pass.
     *
     * Browser wallets (MetaMask/wagmi) cannot raw-sign an arbitrary hash,
     * so the owner signs the binding digest via EIP-191 personal_sign and
     * the device verifies against keccak256("\x19Ethereum Signed Message:\n32"
     * || digest) — the exact preimage viem's signMessage({raw: digest})
     * hashes for a 32-byte payload. Contract spec lives in arkiv_cfg.h. */
    if (!cmd->device_id || !cmd->command_id) {
        ESP_LOGW(TAG, "cmd missing device_id/command_id — dropping");
        return false;
    }
    if (strlen(cmd->command_id) != ARKIV_COMMAND_ID_LEN) {
        ESP_LOGW(TAG, "cmd command_id len=%u (want %d) — dropping",
                 (unsigned)strlen(cmd->command_id), ARKIV_COMMAND_ID_LEN);
        return false;
    }

    uint8_t ep[4] = {
        (uint8_t)(cmd->epoch & 0xFF),
        (uint8_t)((cmd->epoch >> 8) & 0xFF),
        (uint8_t)((cmd->epoch >> 16) & 0xFF),
        (uint8_t)((cmd->epoch >> 24) & 0xFF),
    };
    uint8_t sq[8];
    for (int i = 0; i < 8; i++) sq[i] = (uint8_t)((cmd->counter >> (8 * i)) & 0xFF);

    uint8_t digest[32];
    arkiv_keccak256_ctx kx;
    arkiv_keccak256_init(&kx);
    /* tag + its implicit NUL — keccak update treats the byte string as
     * opaque, the NUL is part of the contract (see ARKIV_CMD_BIND_TAG). */
    arkiv_keccak256_update(&kx, (const uint8_t *)ARKIV_CMD_BIND_TAG,
                           strlen(ARKIV_CMD_BIND_TAG) + 1);
    arkiv_keccak256_update(&kx, (const uint8_t *)cmd->device_id,
                           strlen(cmd->device_id));
    arkiv_keccak256_update(&kx, ep, sizeof(ep));
    arkiv_keccak256_update(&kx, sq, sizeof(sq));
    arkiv_keccak256_update(&kx, (const uint8_t *)cmd->command_id,
                           ARKIV_COMMAND_ID_LEN);
    arkiv_keccak256_update(&kx, cmd->frame, cmd->frame_len);
    arkiv_keccak256_finish(&kx, digest);

    /* EIP-191 wrap (matches arkiv_claim.c eip191_wrap; not factored out yet
     * — rule of three, will lift to arkiv_crypto on the next consumer). */
    static const char PFX[] = "\x19" "Ethereum Signed Message:\n32";
    uint8_t signed_digest[32];
    arkiv_keccak256_init(&kx);
    arkiv_keccak256_update(&kx, (const uint8_t *)PFX, sizeof(PFX) - 1);
    arkiv_keccak256_update(&kx, digest, sizeof(digest));
    arkiv_keccak256_finish(&kx, signed_digest);

    if (arkiv_secp256k1_verify(s_owner_pub, signed_digest, cmd->sig) != 1) {
        ESP_LOGW(TAG, "owner signature INVALID — dropping");
        return false;
    }

    /* Accept: advance the replay baseline + block cursor, then hand the
     * bare inner WUPS frame to the RP2040 (Decision C — it sees exactly
     * what the Track 1 MQTT path delivers). */
    s_last_ctr  = cmd->counter;
    if (cmd->block > s_cur_block) s_cur_block = cmd->block;
    if (!persist_progress(s_last_ctr, s_cur_block)) {
        ESP_LOGW(TAG, "failed to persist progress (continuing)");
    }
    *frame     = cmd->frame;
    *frame_len = cmd->frame_len;
    ESP_LOGI(TAG, "Arkiv command verified (ctr=%llu, len=%u)",
             (unsigned long long)cmd->counter, (unsigned)cmd->frame_len);
    return true;
}

esp_err_t cmdauth_arkiv_bind_owner(const uint8_t owner_pub[64], uint32_t epoch)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t sh;
    esp_err_t err = state_open(&sh, NVS_READWRITE);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(sh, KEY_OWNER_PUB, owner_pub, PUB_LEN);
    if (err == ESP_OK) err = nvs_set_u32(sh, KEY_KEY_EPOCH, epoch);
    /* Fresh counter namespace per owner (§10.6) so a previous owner's old
     * signed commands can never collide post-resale. */
    if (err == ESP_OK) err = nvs_set_u64(sh, KEY_LAST_CTR, 0);
    if (err == ESP_OK) err = nvs_set_u8(sh, KEY_CLAIM_STATE, ARKIV_CLAIMED);
    if (err == ESP_OK) err = nvs_commit(sh);
    nvs_close(sh);
    if (err != ESP_OK) return err;

    memcpy(s_owner_pub, owner_pub, PUB_LEN);
    pub_to_addr(s_owner_pub, s_owner_addr);
    s_have_owner = true;
    s_key_epoch = epoch;
    s_last_ctr = 0;
    s_claim_state = ARKIV_CLAIMED;
    ESP_LOGI(TAG, "owner bound (epoch=%u) — ARKIV_CLAIMED", (unsigned)epoch);
    return ESP_OK;
}

esp_err_t cmdauth_arkiv_set_epoch(uint32_t epoch)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (epoch <= s_key_epoch) {
        ESP_LOGW(TAG, "epoch bump %u not > cur %u — ignored",
                 (unsigned)epoch, (unsigned)s_key_epoch);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t sh;
    esp_err_t err = state_open(&sh, NVS_READWRITE);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(sh, KEY_KEY_EPOCH, epoch);
    if (err == ESP_OK) err = nvs_commit(sh);
    nvs_close(sh);
    if (err != ESP_OK) return err;
    s_key_epoch = epoch;
    ESP_LOGI(TAG, "key epoch ratcheted → %u", (unsigned)epoch);
    return ESP_OK;
}

esp_err_t cmdauth_arkiv_clear(void)
{
    nvs_handle_t sh;
    esp_err_t err = state_open(&sh, NVS_READWRITE);
    if (err != ESP_OK) return err;
    nvs_erase_key(sh, KEY_OWNER_PUB);
    nvs_erase_key(sh, KEY_KEY_EPOCH);
    nvs_erase_key(sh, KEY_LAST_CTR);
    nvs_erase_key(sh, KEY_CUR_BLOCK);
    err = nvs_set_u8(sh, KEY_CLAIM_STATE, ARKIV_UNCLAIMED);
    if (err == ESP_OK) err = nvs_commit(sh);
    nvs_close(sh);
    memset(s_owner_pub, 0, PUB_LEN);
    memset(s_owner_addr, 0, ADDR_LEN);
    s_have_owner = false;
    s_key_epoch = 0;
    s_last_ctr = 0;
    s_cur_block = 0;
    s_claim_state = ARKIV_UNCLAIMED;
    ESP_LOGI(TAG, "Arkiv binding cleared — UNCLAIMED");
    return err;
}
