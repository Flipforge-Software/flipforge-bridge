#include "bridge_auth.h"

#ifdef BRIDGE_HOST_TEST
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include "mbedtls/md.h"
#endif

esp_err_t bridge_auth_hmac_sha256(
    const uint8_t secret[BRIDGE_PAIRING_SECRET_BYTES],
    const uint8_t* message,
    size_t message_size,
    uint8_t output[BRIDGE_HMAC_BYTES]) {
    if(!secret || !message || !output) return ESP_ERR_INVALID_ARG;
#ifdef BRIDGE_HOST_TEST
    unsigned int output_size = 0U;
    return HMAC(
               EVP_sha256(),
               secret,
               BRIDGE_PAIRING_SECRET_BYTES,
               message,
               message_size,
               output,
               &output_size) && output_size == BRIDGE_HMAC_BYTES ?
               ESP_OK : ESP_FAIL;
#else
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!info) return ESP_FAIL;
    return mbedtls_md_hmac(
               info,
               secret,
               BRIDGE_PAIRING_SECRET_BYTES,
               message,
               message_size,
               output) == 0 ?
               ESP_OK : ESP_FAIL;
#endif
}
