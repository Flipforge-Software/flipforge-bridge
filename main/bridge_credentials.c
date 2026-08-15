#include "bridge_credentials.h"

#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#define NVS_AP_PASSWORD_KEY "ap_password"
#define NVS_PAIR_SECRET_KEY "pair_secret"

static const char credential_alphabet[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

static void generate_password(char output[BRIDGE_AP_PASSWORD_BYTES + 1U]) {
    uint8_t random[BRIDGE_AP_PASSWORD_BYTES];
    esp_fill_random(random, sizeof(random));
    for(size_t index = 0; index < BRIDGE_AP_PASSWORD_BYTES; ++index) {
        output[index] = credential_alphabet[random[index] % (sizeof(credential_alphabet) - 1U)];
    }
    output[BRIDGE_AP_PASSWORD_BYTES] = '\0';
    memset(random, 0, sizeof(random));
}

esp_err_t bridge_credentials_load_or_create(BridgeCredentials* credentials) {
    if(!credentials) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(BRIDGE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(error != ESP_OK) return error;

    size_t password_size = sizeof(credentials->ap_password);
    size_t secret_size = sizeof(credentials->pairing_secret);
    const esp_err_t password_error =
        nvs_get_str(handle, NVS_AP_PASSWORD_KEY, credentials->ap_password, &password_size);
    const esp_err_t secret_error =
        nvs_get_blob(handle, NVS_PAIR_SECRET_KEY, credentials->pairing_secret, &secret_size);

    if(password_error == ESP_OK && secret_error == ESP_OK &&
       password_size == sizeof(credentials->ap_password) &&
       secret_size == sizeof(credentials->pairing_secret)) {
        nvs_close(handle);
        return ESP_OK;
    }
    if((password_error != ESP_ERR_NVS_NOT_FOUND && password_error != ESP_OK) ||
       (secret_error != ESP_ERR_NVS_NOT_FOUND && secret_error != ESP_OK)) {
        nvs_close(handle);
        bridge_credentials_clear(credentials);
        return password_error != ESP_OK && password_error != ESP_ERR_NVS_NOT_FOUND ?
                   password_error : secret_error;
    }

    generate_password(credentials->ap_password);
    esp_fill_random(credentials->pairing_secret, sizeof(credentials->pairing_secret));
    error = nvs_set_str(handle, NVS_AP_PASSWORD_KEY, credentials->ap_password);
    if(error == ESP_OK) {
        error = nvs_set_blob(
            handle,
            NVS_PAIR_SECRET_KEY,
            credentials->pairing_secret,
            sizeof(credentials->pairing_secret));
    }
    if(error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if(error != ESP_OK) {
        bridge_credentials_clear(credentials);
        return error;
    }
    credentials->newly_created = true;
    return ESP_OK;
}

void bridge_credentials_clear(BridgeCredentials* credentials) {
    if(credentials) memset(credentials, 0, sizeof(*credentials));
}
