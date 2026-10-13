#include <cstdio>
#include <cstring>
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_server.h"

// ===================== 硬件引脚定义 =====================
#define MIC_BCLK_PIN    GPIO_NUM_8
#define MIC_LRCK_PIN    GPIO_NUM_9
#define MIC_DATA_IN_PIN GPIO_NUM_7

#define SPEAKER_BCLK_PIN GPIO_NUM_10
#define SPEAKER_LRCK_PIN GPIO_NUM_11
#define SPEAKER_DATA_OUT_PIN GPIO_NUM_12

#define KEY_TALK        GPIO_NUM_1   // 对话按键 GPIO1
#define KEY_SWITCH_1    GPIO_NUM_2   // 切换 豆包
#define KEY_SWITCH_2    GPIO_NUM_3   // 切换 Coze
#define KEY_SWITCH_3    GPIO_NUM_4   // 切换 小智

// ===================== AI 模式枚举 =====================
enum AIMode {
    AI_DOUBAO,
    AI_COZE,
    AI_XIAOZHI
};

static AIMode g_ai_mode = AI_DOUBAO;
static bool g_wifi_connected = false;
static QueueHandle_t gpio_evt_queue = nullptr;
static const char *TAG = "AI_CHAT_S3";

// GitHub Actions注入密钥
#ifdef COZE_TOKEN
const char* coze_token = COZE_TOKEN;
#else
const char* coze_token = "";
#endif

#ifdef DOUBAO_APIKEY
const char* doubao_apikey = DOUBAO_APIKEY;
#else
const char* doubao_apikey = "";
#endif

#ifdef XIAOZHI_TOKEN
const char* xiaozhi_token = XIAOZHI_TOKEN;
#else
const char* xiaozhi_token = "";
#endif

// ===================== GPIO中断回调(按键) =====================
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, nullptr);
}

static void key_task(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(20)); //消抖
            if(gpio_get_level((gpio_num_t)io_num) == 0)
            {
                ESP_LOGI(TAG, "按下按键: GPIO %d", io_num);
                switch(io_num)
                {
                    case KEY_TALK:
                        ESP_LOGI(TAG, "开始对话");
                        if(!g_wifi_connected) {
                            ESP_LOGW(TAG, "无WiFi，进入AP配网模式");
                            // 启动热点配网
                            wifi_start_ap();
                        } else {
                            // 采集PCM并上传API，TTS播放
                            audio_start_capture_and_upload();
                        }
                        break;
                    case KEY_SWITCH_1:
                        g_ai_mode = AI_DOUBAO;
                        ESP_LOGI(TAG, "切换AI: 豆包");
                        break;
                    case KEY_SWITCH_2:
                        g_ai_mode = AI_COZE;
                        ESP_LOGI(TAG, "切换AI: Coze");
                        break;
                    case KEY_SWITCH_3:
                        g_ai_mode = AI_XIAOZHI;
                        ESP_LOGI(TAG, "切换AI: 小智");
                        break;
                    default: break;
                }
            }
        }
    }
}

// ===================== WiFi事件回调 =====================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi断开");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi已连接,ip:" IPSTR, IP2STR(&event->ip_info.ip));
        g_wifi_connected = true;
    }
}

// STA连接WiFi
void wifi_init_sta(const char* ssid, const char* pwd)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr);

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pwd);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// AP热点（无密码，网页配网）
void wifi_start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "ESP32S3-AI",
            .ssid_len = strlen("ESP32S3-AI"),
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN //无密码热点
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP热点开启，名称ESP32S3-AI，无密码");

    // 启动配网网页服务器
    httpd_handle_t server = nullptr;
    start_webserver(&server);
}

// ===================== I2S音频采集与播放 =====================
void audio_start_capture_and_upload(void)
{
    ESP_LOGI(TAG, "I2S麦克风采集PCM，上传云端API，TTS返回播放");
    // 此处：I2S读取INMP441 PCM
    // HTTP POST上传PCM到对应AI语音API（豆包/coze/小智，根据g_ai_mode选择）
    // 接收TTS音频流，I2S输出到MAX98357A
}

// ===================== Web配网网页服务（扫描WiFi+提交ssid/pass） =====================
httpd_handle_t start_webserver(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Web配网服务启动");
        //注册路由：扫描WiFi页面，提交WiFi表单
        //httpd_register_uri_handler(*server, &wifi_scan_handler);
        //httpd_register_uri_handler(*server, &wifi_save_handler);
    }
    return *server;
}

// ===================== 初始化按键GPIO =====================
void gpio_key_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    io_conf.pin_bit_mask = (1ULL << KEY_TALK) | (1ULL << KEY_SWITCH_1) | (1ULL << KEY_SWITCH_2) | (1ULL << KEY_SWITCH_3);
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(key_task, "key_task", 2048, nullptr, 10, nullptr);

    gpio_isr_handler_add(KEY_TALK, gpio_isr_handler, (void*)KEY_TALK);
    gpio_isr_handler_add(KEY_SWITCH_1, gpio_isr_handler, (void*)KEY_SWITCH_1);
    gpio_isr_handler_add(KEY_SWITCH_2, gpio_isr_handler, (void*)KEY_SWITCH_2);
    gpio_isr_handler_add(KEY_SWITCH_3, gpio_isr_handler, (void*)KEY_SWITCH_3);
    gpio_install_isr_service(0);
}

// ===================== 程序入口 【重点extern "C"】 =====================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32S3 AI语音助手启动");

    //NVS初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //初始化按键
    gpio_key_init();

    // 尝试读取NVS保存的WiFi信息并连接STA
    // 这里可增加NVS读取SSID/PASSWORD逻辑
    // wifi_init_sta(saved_ssid, saved_pwd);
}
