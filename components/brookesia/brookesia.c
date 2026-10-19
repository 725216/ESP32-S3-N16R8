#include "brookesia.h"
#include "esp_log.h"
static const char *TAG = "brookesia";

void brookesia_set_agent(const char *name)
{
    ESP_LOGI(TAG,"Set agent: %s", name);
}

esp_err_t brookesia_init(brookesia_cfg_t *cfg)
{
    ESP_LOGI(TAG,"Brookesia init ok");
    return ESP_OK;
}

void brookesia_start_session(void)
{
    ESP_LOGI(TAG,"Start voice session");
}

void brookesia_stop_session(void)
{
    ESP_LOGI(TAG,"Stop voice session");
}
