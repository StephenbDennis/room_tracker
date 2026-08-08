idf_component_register(
    SRCS
        "app_main.c"
        "ld2450.c"
        "fusion.c"
        "zones.c"
        "config.c"
        "actions.c"
        "ble_gatt.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        nvs_flash
        bt
    # IDF v6.0 split the monolithic `driver` component; UART and GPIO now come
    # from their own components and are only used from .c files here.
    PRIV_REQUIRES
        esp_driver_uart
        esp_driver_gpio
)
