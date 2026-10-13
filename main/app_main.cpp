#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#define GPIO_BTN_TALK     GPIO_NUM_1   //对话按键
#define GPIO_BTN_MODE1    GPIO_NUM_2   //小智（预留，暂不启用）
#define GPIO_BTN_MODE2    GPIO_NUM_3   //Coze
#define GPIO_BTN_MODE3    GPIO_NUM_4   //豆包

typedef enum {
    AI_XIAOZHI,
    AI_COZE,
    AI_DOUBAO
} AiAgentType;

static AiAgentType current_agent = AI_COZE;
static const char *TAG = "AI_CHAT";

//按键初始化
static void gpio_btn_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_BTN_TALK)|(1ULL << GPIO_BTN_MODE1)|(1ULL << GPIO_BTN_MODE2)|(1ULL << GPIO_BTN_MODE3),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
}

//WiFi AP无密码热点网页配网
static void wifi_ap_provision_start(void)
{
    //这里后续补AP配网代码
}

static void btn_task(void *arg)
{
    for(;;){
        if(gpio_get_level(GPIO_BTN_MODE1) == 0){
            ESP_LOGI(TAG,"切换至 小智（暂未启用）");
            current_agent = AI_XIAOZHI;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(GPIO_BTN_MODE2) == 0){
            ESP_LOGI(TAG,"切换至 Coze扣子");
            current_agent = AI_COZE;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(GPIO_BTN_MODE3) == 0){
            ESP_LOGI(TAG,"切换至 豆包");
            current_agent = AI_DOUBAO;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(GPIO_BTN_TALK) == 0){
            ESP_LOGI(TAG,"开始对话");
            switch(current_agent){
                case AI_XIAOZHI:
                    ESP_LOGW(TAG,"小智组件未编译，跳过");
                    break;
                case AI_COZE:
                    // start_pcm_upload_coze();
                    break;
                case AI_DOUBAO:
                    // start_pcm_upload_doubao();
                    break;
                default:break;
            }
            while(gpio_get_level(GPIO_BTN_TALK)==0) vTaskDelay(10);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    gpio_btn_init();

    //WiFi检测，无WiFi自动开启无密码AP配网
    wifi_ap_provision_start();

    xTaskCreate(btn_task,"btn_task",4096,NULL,5,NULL);
}
