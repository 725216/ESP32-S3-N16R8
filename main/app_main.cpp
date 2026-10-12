#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "AI_CHAT";

//==================== 硬件引脚定义 ====================
#define KEY_PTT     GPIO_NUM_1
#define KEY_DOUBAO  GPIO_NUM_2
#define KEY_XIAOZHI GPIO_NUM_21
#define KEY_COZE    GPIO_NUM_47

// INMP441 麦克风 I2S0
#define I2S_MIC_PORT     I2S_NUM_0
#define I2S_MIC_BCLK     GPIO_NUM_5
#define I2S_MIC_WS       GPIO_NUM_4
#define I2S_MIC_DATA_IN  GPIO_NUM_6

// MAX98357A 功放 I2S1
#define I2S_SPK_PORT     I2S_NUM_1
#define I2S_SPK_BCLK     GPIO_NUM_8
#define I2S_SPK_WS       GPIO_NUM_7
#define I2S_SPK_DATA_OUT GPIO_NUM_9

#define SAMPLE_RATE 16000
#define SAMPLE_BITS 16
#define BLOCK_SAMPLES 512

//==================== AI类型 ====================
typedef enum {
    AI_NONE,
    AI_XIAOZHI,
    AI_COZE,
    AI_DOUBAO
} AiType;
static AiType g_current_ai = AI_DOUBAO;

// ============ 密钥，自行替换 ============
#define DOUBAO_TOKEN    "你的豆包token"
#define COZE_TOKEN      "你的coze token"
#define XIAOZHI_TOKEN   "你的小智token"

static bool wifi_connected = false;
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// GPIO按键初始化
static void gpio_keys_init(void)
{
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << KEY_PTT) | (1ULL << KEY_DOUBAO) | (1ULL << KEY_XIAOZHI) | (1ULL << KEY_COZE);
    gpio_config(&io_conf);
}

// I2S 麦克风初始化
static void i2s_mic_init(void)
{
    i2s_driver_install(I2S_MIC_PORT, &(i2s_config_t){
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = BLOCK_SAMPLES,
    }, 0, NULL);

    i2s_set_pin(I2S_MIC_PORT, &(i2s_pin_config_t){
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_WS,
        .data_in_num = I2S_MIC_DATA_IN,
        .data_out_num = I2S_PIN_NO_CHANGE
    });
}

// I2S 扬声器初始化
static void i2s_spk_init(void)
{
    i2s_driver_install(I2S_SPK_PORT, &(i2s_config_t){
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = BLOCK_SAMPLES,
    },0,NULL);

    i2s_set_pin(I2S_SPK_PORT, &(i2s_pin_config_t){
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_WS,
        .data_out_num = I2S_SPK_DATA_OUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    });
}

// WiFi初始化【修复：memset清零结构体】
static void wifi_init_sta(const char* ssid, const char* pwd)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strcpy((char*)wifi_cfg.sta.ssid, ssid);
    strcpy((char*)wifi_cfg.sta.password, pwd);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi connecting: %s", ssid);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retry");
        wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi ok, ip:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

// 读取I2S麦克风音频
static int i2s_read_mic(int16_t *buf, int samples)
{
    size_t bytes_read;
    i2s_read(I2S_MIC_PORT, buf, samples*2, &bytes_read, portMAX_DELAY);
    return bytes_read / 2;
}

// 播放音频到喇叭
static void i2s_play_audio(int16_t *buf, int samples)
{
    size_t wb;
    i2s_write(I2S_SPK_PORT, buf, samples*2, &wb, portMAX_DELAY);
}

// ========== AI业务任务（PTT录音+上传+接收TTS播放）==========
void ai_chat_task(void *pvParam)
{
    int16_t audio_buf[BLOCK_SAMPLES];
    bool ptt_prev = false;

    for(;;)
    {
        bool ptt_now = (gpio_get_level(KEY_PTT) == 0);

        // AI切换按键检测
        if(gpio_get_level(KEY_XIAOZHI)==0)
        {
            g_current_ai = AI_XIAOZHI;
            ESP_LOGI(TAG,"✅ 切换AI: 小智");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(KEY_COZE)==0)
        {
            g_current_ai = AI_COZE;
            ESP_LOGI(TAG,"✅ 切换AI: Coze扣子");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if(gpio_get_level(KEY_DOUBAO)==0)
        {
            g_current_ai = AI_DOUBAO;
            ESP_LOGI(TAG,"✅ 切换AI: 豆包");
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        if(ptt_now && !ptt_prev)
        {
            ESP_LOGI(TAG,"🎤 PTT按下，开始录音");
            if(!wifi_connected)
            {
                ESP_LOGE(TAG,"WiFi未连接，无法对话");
            }
            else
            {
                // 这里：循环读取I2S音频，流式发送websocket ASR
                // =====================
                // 【核心逻辑占位】
                // 豆包/Coze WebSocket 流式语音上传，接收返回音频TTS，送入i2s_play_audio播放
                // =====================
            }
        }
        if(!ptt_now && ptt_prev)
        {
            ESP_LOGI(TAG,"🔴 PTT松开，结束录音，等待AI回复");
        }
        ptt_prev = ptt_now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void)
{
    gpio_keys_init();
    i2s_mic_init();
    i2s_spk_init();

    // ========= 修改这里为你的WiFi信息 =========
    const char* WIFI_SSID = "你的wifi名称";
    const char* WIFI_PASS = "你的wifi密码";
    wifi_init_sta(WIFI_SSID, WIFI_PASS);

    xTaskCreate(ai_chat_task, "ai_chat_task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "==== ESP32S3 AI Chat Ready ====");
    ESP_LOGI(TAG, "KEY1=PTT | KEY2=豆包 | KEY21=小智 | KEY47=Coze");
}
