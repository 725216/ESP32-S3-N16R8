#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_http_client.h"

// ========= 按键定义 =========
#define GPIO_KEY_TALK       1   // 对话按键
#define GPIO_KEY_XIAOZHI    2   // 切换小智
#define GPIO_KEY_COZE       3   // 切换Coze
#define GPIO_KEY_DOUBAO     4   // 切换豆包

// ========= I2S INMP441麦克风 =========
#define I2S_MIC_BCLK    8
#define I2S_MIC_WS      9
#define I2S_MIC_DIN     10
#define I2S_MIC_PORT    I2S_NUM_0

// AI类型
typedef enum {
    AI_XIAOZHI,
    AI_COZE,
    AI_DOUBAO
} ai_type_t;
static ai_type_t current_ai = AI_DOUBAO;

// 占位，CI编译时自动替换为secrets中的token
#define TOKEN_XIAOZHI  "TOKEN_XIAOZHI"
#define TOKEN_COZE     "TOKEN_COZE"
#define TOKEN_DOUBAO   "TOKEN_DOUBAO"

// AP热点 无密码
#define AP_SSID "ESP32-AI-Setup"
#define AP_PASSWORD ""

// GPIO按键初始化
static void keys_gpio_init(void)
{
    gpio_config_t io_cfg = {0};
    io_cfg.intr_type = GPIO_INTR_DISABLE;
    io_cfg.mode = GPIO_MODE_INPUT;
    io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_cfg.pin_bit_mask = (1ULL << GPIO_KEY_TALK) | (1ULL << GPIO_KEY_XIAOZHI) | (1ULL << GPIO_KEY_COZE) | (1ULL << GPIO_KEY_DOUBAO);
    gpio_config(&io_cfg);
}

// I2S麦克风初始化
static void i2s_mic_init(void)
{
    i2s_driver_uninstall(I2S_MIC_PORT);
    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_cfg = {
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DIN
    };
    i2s_driver_install(I2S_MIC_PORT, &i2s_cfg, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &pin_cfg);
}

// 启动无密码AP热点
static void wifi_ap_init(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_init_cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN //开放无密码
        }
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();
    printf("✅ AP热点开启 SSID: %s 无密码\n", AP_SSID);
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    keys_gpio_init();
    i2s_mic_init();
    wifi_ap_init();

    printf("==== ESP32 AI语音启动成功 ====\n");
    printf("当前AI: %d (0小智,1Coze,2豆包)\n", current_ai);

    while (1)
    {
        // 切换按键
        if (gpio_get_level(GPIO_KEY_XIAOZHI) == 0)
        {
            current_ai = AI_XIAOZHI;
            printf("👉 切换至【小智AI】\n");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (gpio_get_level(GPIO_KEY_COZE) == 0)
        {
            current_ai = AI_COZE;
            printf("👉 切换至【Coze】\n");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (gpio_get_level(GPIO_KEY_DOUBAO) == 0)
        {
            current_ai = AI_DOUBAO;
            printf("👉 切换至【豆包】\n");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        // 对话按键GPIO1
        if (gpio_get_level(GPIO_KEY_TALK) == 0)
        {
            printf("🎤 对话按键按下，开始采集PCM音频，准备上传API\n");
            // 此处预留：PCM读取、HTTP上传、接收TTS并播放
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
