#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "AI_CHAT";

// ========== 按键定义 ==========
#define KEY_PTT     GPIO_NUM_1
#define KEY_DOUBAO  GPIO_NUM_2
#define KEY_XIAOZHI GPIO_NUM_21
#define KEY_COZE    GPIO_NUM_47

typedef enum {
    AI_NONE,
    AI_XIAOZHI,
    AI_COZE,
    AI_DOUBAO
} AiType;

static AiType g_current_ai = AI_DOUBAO;

static void gpio_keys_init(void)
{
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << KEY_PTT) | (1ULL << KEY_DOUBAO) | (1ULL << KEY_XIAOZHI) | (1ULL << KEY_COZE);
    gpio_config(&io_conf);
}

// I2S配置
#define I2S_MIC_PORT     I2S_NUM_0
#define I2S_SPK_PORT     I2S_NUM_1

// INMP441
#define I2S_MIC_BCLK     GPIO_NUM_5
#define I2S_MIC_WS       GPIO_NUM_4
#define I2S_MIC_DATA_IN  GPIO_NUM_6

// MAX98357A
#define I2S_SPK_BCLK     GPIO_NUM_8
#define I2S_SPK_WS       GPIO_NUM_7
#define I2S_SPK_DATA_OUT GPIO_NUM_9

static void i2s_mic_init(void)
{
    i2s_driver_install(I2S_MIC_PORT, &(i2s_config_t){
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
    }, 0, NULL);

    i2s_set_pin(I2S_MIC_PORT, &(i2s_pin_config_t){
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_WS,
        .data_in_num = I2S_MIC_DATA_IN,
        .data_out_num = I2S_PIN_NO_CHANGE
    });
}

static void i2s_spk_init(void)
{
    i2s_driver_install(I2S_SPK_PORT, &(i2s_config_t){
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
    },0,NULL);

    i2s_set_pin(I2S_SPK_PORT, &(i2s_pin_config_t){
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_WS,
        .data_out_num = I2S_SPK_DATA_OUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    });
}

// WiFi初始化
static void wifi_init_sta(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = "你的WiFi名称",
            .password = "你的WiFi密码",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi Starting");
}

extern "C" void app_main(void)
{
    gpio_keys_init();
    i2s_mic_init();
    i2s_spk_init();
    wifi_init_sta();

    ESP_LOGI(TAG, "===== AI Chat Ready =====");
    ESP_LOGI(TAG, "KEY21=小智 | KEY47=Coze | KEY2=豆包 | KEY1=PTT说话");

    bool ptt_prev = false;
    while(1)
    {
        bool ptt_now = gpio_get_level(KEY_PTT) == 0;

        // 切换AI
        if(gpio_get_level(KEY_XIAOZHI)==0)
        {
            g_current_ai = AI_XIAOZHI;
            ESP_LOGI(TAG,"已切换：小智");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(KEY_COZE)==0)
        {
            g_current_ai = AI_COZE;
            ESP_LOGI(TAG,"已切换：Coze");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(KEY_DOUBAO)==0)
        {
            g_current_ai = AI_DOUBAO;
            ESP_LOGI(TAG,"已切换：豆包");
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        // PTT检测
        if(ptt_now && !ptt_prev)
        {
            ESP_LOGI(TAG,"PTT按下，开始录音");
        }
        if(!ptt_now && ptt_prev)
        {
            ESP_LOGI(TAG,"PTT松开，停止录音，发送识别");
        }
        ptt_prev = ptt_now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
