idf_component_register(
    SRCS
        "activity_led.c"
        "bridge_main.c"
        "bridge_auth.c"
        "bridge_credentials.c"
        "bridge_io.c"
        "bridge_metrics.c"
        "bridge_server.c"
        "bridge_session.c"
        "expansion_protocol.c"
        "expansion_state_machine.c"
        "expansion_transport.c"
        "pairing_service.c"
        "rpc_proxy.c"
        "usb_console.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_uart esp_event esp_netif esp_timer esp_tinyusb esp_wifi lwip mbedtls nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
