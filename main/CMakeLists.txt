idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif driver gpio i2s esp_http_client wifi_provisioning
)
