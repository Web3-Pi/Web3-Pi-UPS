#pragma once
#include "esp_err.h"

/* Attach the reviewed CA bundle and reject compromised intermediate keys. */
esp_err_t wups_crt_bundle_attach(void *conf);
