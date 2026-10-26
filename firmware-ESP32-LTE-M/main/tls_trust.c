#include "tls_trust.h"
#include "tls_distrust.h"
#include "esp_crt_bundle.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* ESP-IDF exports this callback, but it is not a public header API.
 * cmake/wups_tls_trust.cmake pins its source and requires review on SDK
 * changes. Keep the SDK signature/hostname/time checks after the denylist.
 */
extern int esp_crt_verify_callback(void *ctx, mbedtls_x509_crt *crt,
                                   int depth, uint32_t *flags);

static int wups_verify_certificate(void *ctx, mbedtls_x509_crt *crt,
                                   int depth, uint32_t *flags)
{
    if (crt == NULL || flags == NULL) return MBEDTLS_ERR_X509_FATAL_ERROR;
    /* pk_raw is the complete DER SubjectPublicKeyInfo, not a certificate
     * fingerprint: another issuer/cross-sign cannot bypass this check.
     * A nonzero return prevents a later callback from clearing the flags.
     */
    if (wups_tls_spki_distrusted(crt->pk_raw.p, crt->pk_raw.len)) {
        *flags |= MBEDTLS_X509_BADCERT_REVOKED;
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    return esp_crt_verify_callback(ctx, crt, depth, flags);
}

esp_err_t wups_crt_bundle_attach(void *conf)
{
    if (conf == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err != ESP_OK) return err;
    mbedtls_ssl_conf_verify((mbedtls_ssl_config *)conf,
                           wups_verify_certificate, NULL);
    return ESP_OK;
}
