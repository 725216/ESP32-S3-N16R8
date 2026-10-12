#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "AI_CHAT";

// ========== 硬件引脚定义 ==========
// 按键：按住说话
#define KEY_GPIO GPIO_NUM_1
// INMP441 麦克风 I2S
#define I2S_MIC_BCLK   GPIO_NUM_41
#define I2S_MIC_LRCK   GPIO_NUM_42
#define I2S_MIC_DIN    GPIO_NUM_40
// MAX98357A 功放 I2S
#define I2S_SPK_BCLK   GPIO_NUM_7
#define I2S_SPK_LRCK   GPIO_NUM_8
#define I2S_SPK_DOUT   GPIO_NUM_9

#define I2S_PORT_MIC I2S_NUM_0
#define I2S_PORT_SPK I2S_NUM_1
#define SAMPLE_RATE 16000
#define SAMPLE_BITS I2S_BITS_PER_SAMPLE_16BIT
#define RECORD_BUF_SAMPLES 1024

static int16_t mic_buffer[RECORD_BUF_SAMPLES];

// WiFi配置，请修改为你自己的WiFi
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASS     "你的WiFi密码"

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi断开，尝试重连");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi连接成功，IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi初始化完成");
}

// I2S麦克风初始化
void i2s_mic_init()
{
    i2s_config_t i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_cfg.sample_rate = SAMPLE_RATE;
    i2s_cfg.bits_per_sample = SAMPLE_BITS;
    i2s_cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count = 4;
    i2s_cfg.dma_buf_len = 512;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT_MIC, &i2s_cfg, 0, NULL));

    i2s_pin_config_t pin_cfg;
    memset(&pin_cfg, 0, sizeof(pin_cfg));
    pin_cfg.bck_io_num = I2S_MIC_BCLK;
    pin_cfg.ws_io_num = I2S_MIC_LRCK;
    pin_cfg.data_out_num = I2S_PIN_NO_CHANGE;
    pin_cfg.data_in_num = I2S_MIC_DIN;
    pin_cfg.mck_io_num = I2S_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT_MIC, &pin_cfg));
}

// I2S扬声器MAX98357初始化
void i2s_spk_init()
{
    i2s_config_t i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_cfg.sample_rate = SAMPLE_RATE;
    i2s_cfg.bits_per_sample = SAMPLE_BITS;
    i2s_cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count = 4;
    i2s_cfg.dma_buf_len = 512;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT_SPK, &i2s_cfg,0,NULL));

    i2s_pin_config_t pin_cfg;
    memset(&pin_cfg, 0, sizeof(pin_cfg));
    pin_cfg.bck_io_num = I2S_SPK_BCLK;
    pin_cfg.ws_io_num = I2S_SPK_LRCK;
    pin_cfg.data_out_num = I2S_SPK_DOUT;
    pin_cfg.data_in_num = I2S_PIN_NO_CHANGE;
    pin_cfg.mck_io_num = I2S_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT_SPK, &pin_cfg));
}

// 按键初始化
void key_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << KEY_GPIO;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
}

// 读取I2S麦克风PCM数据
size_t read_mic_pcm(int16_t *buf, size_t sample_count)
{
    size_t bytes_read;
    i2s_read(I2S_PORT_MIC, buf, sample_count*sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(100));
    return bytes_read / sizeof(int16_t);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==== ESP32S3 AI ChatBot Start ====");
    wifi_init();
    i2s_mic_init();
    i2s_spk_init();
    key_init();
    ESP_LOGI(TAG, "I2S + KEY Init OK, 按住GPIO1按键说话");

    bool is_recording = false;

    while(1)
    {
        int level = gpio_get_level(KEY_GPIO);
        if(level == 0 && !is_recording)
        {
            ESP_LOGI(TAG,"按键按下，开始录音...");
            is_recording = true;
        }
        else if(level == 1 && is_recording)
        {
            ESP_LOGI(TAG,"松开按键，停止录音");
            is_recording = false;
        }

        if(is_recording)
        {
            size_t samples = read_mic_pcm(mic_buffer, RECORD_BUF_SAMPLES);
            ESP_LOGD(TAG, "采集到 %d 个PCM样本", (int)samples);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
