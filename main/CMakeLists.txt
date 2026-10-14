idf_component_register(
    SRCS "app.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif driver gpio i2s esp_http_client wifi_provisioning
)
