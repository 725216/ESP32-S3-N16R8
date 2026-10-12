#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_netif.h"

static const char *TAG = "CHATBOT";

// ============ NVS 存储KEY ============
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

// ============ AP配网热点配置 ============
#define AP_SSID "ESP32-CHAT-SETUP"
#define AP_PASSWORD ""
#define AP_IP_ADDR "192.168.4.1"

// ============ 4个按键GPIO定义 ============
#define KEY_SPEAK     GPIO_NUM_1   // 说话按键
#define KEY_DOUBAO    GPIO_NUM_4   // 切换豆包
#define KEY_COZE      GPIO_NUM_21  // 切换Coze
#define KEY_XIAOZHI   GPIO_NUM_47  // 切换小智

// 当前AI代理枚举
typedef enum {
    AI_DOUBAO,
    AI_COZE,
    AI_XIAOZHI
} ai_agent_t;
ai_agent_t g_current_ai = AI_DOUBAO;

// ============ I2S引脚定义（全部用GPIO_NUM_xx 修复类型错误） ============
// I2S0: MAX98357A 功放输出
#define I2S_TX_BCLK  GPIO_NUM_42
#define I2S_TX_LRCK  GPIO_NUM_41
#define I2S_TX_DOUT  GPIO_NUM_40

// I2S1: INMP441 麦克风输入
#define I2S_RX_BCLK  GPIO_NUM_38
#define I2S_RX_LRCK  GPIO_NUM_39
#define I2S_RX_DIN   GPIO_NUM_48

// 全局句柄
i2s_chan_handle_t tx_handle = NULL;
i2s_chan_handle_t rx_handle = NULL;

// 全局状态
httpd_handle_t web_server = NULL;
bool g_sta_connected = false;
bool g_ap_mode = false;
uint32_t g_wifi_retry_count = 0;
#define WIFI_MAX_RETRY 8

// ===================== WEB网页HTML页面 =====================
static const char* html_page =
"<html><meta charset='UTF-8'>"
"<head><title>ESP32语音机器人-WiFi设置</title></head>"
"<body style='font-size:22px;margin:20px;'>"
"<h2>🤖 ESP32 Chatbot WiFi配网</h2>"
"<form method='post' action='/save'>"
"<div>WiFi名称(SSID):</div>"
"<input type='text' name='ssid' maxlength='32' style='width:90%;padding:8px;font-size:20px;'/><br><br>"
"<div>WiFi密码(无密码留空):</div>"
"<input type='text' name='pass' maxlength='64' style='width:90%;padding:8px;font-size:20px;'/><br><br>"
"<input type='submit' value='保存并重启' style='padding:10px 20px;font-size:22px;'/>"
"</form>"
"</body></html>";

// HTTP根页面回调
esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 保存WiFi配置POST接口
esp_err_t handle_save(httpd_req_t *req)
{
    char buf[128];
    size_t recv_len = httpd_req_get_url_query_len(req)+1;
    if(recv_len > sizeof(buf)) recv_len = sizeof(buf);
    httpd_req_get_url_query_str(req, buf, recv_len);

    char ssid[33]={0}, pass[65]={0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));

    nvs_handle_t nvs_handle;
    nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs_handle, NVS_KEY_PASS, pass);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG,"✅ 保存WiFi: SSID=%s", ssid);

    httpd_resp_sendstr(req, "<meta charset='utf-8'><h1>保存成功，设备正在重启连接WiFi...</h1>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// 启动网页服务器
void start_web_server(void)
{
    if(web_server != NULL) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_start(&web_server, &config);

    httpd_uri_t uri_root = {.uri="/", .method=HTTP_GET, .handler=handle_root};
    httpd_uri_t uri_save = {.uri="/save", .method=HTTP_POST, .handler=handle_save};
    httpd_register_uri_handler(web_server, &uri_root);
    httpd_register_uri_handler(web_server, &uri_save);
    ESP_LOGI(TAG,"🌐 Web配网页面已启动，访问 http://%s", AP_IP_ADDR);
}

// AP模式初始化（配网热点）
void wifi_ap_init(void)
{
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASSWORD,
            .ssid_len = 0,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .max_connection = 4,
            .beacon_interval = 100
        }
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    g_ap_mode = true;
    ESP_LOGI(TAG,"🔥 AP配网热点开启：%s，WiFi找不到，可网页重新配网", AP_SSID);
    start_web_server();
}

// 从NVS读取保存的WiFi
esp_err_t read_wifi_nvs(char *ssid, char *pass)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if(ret != ESP_OK) return ret;

    size_t len =32;
    ret = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &len);
    len =64;
    nvs_get_str(nvs_handle, NVS_KEY_PASS, pass, &len);
    nvs_close(nvs_handle);
    return ret;
}

// WiFi事件回调
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG,"开始连接WiFi");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi断开，原因码:%d，重试次数:%" PRIu32, disconn->reason, g_wifi_retry_count);
        g_sta_connected = false;
        g_wifi_retry_count ++;

        if(g_wifi_retry_count >= WIFI_MAX_RETRY)
        {
            ESP_LOGE(TAG,"❌ WiFi多次连接失败，切换到AP配网模式");
            wifi_ap_init();
            return;
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ WiFi连接成功，IP:" IPSTR, IP2STR(&event->ip_info.ip));
        g_sta_connected = true;
        g_wifi_retry_count = 0;
    }
}

// STA模式连接WiFi
void wifi_sta_init(const char* ssid, const char* pass)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = ""
        },
    };
    strncpy((char*)wifi_config.sta.ssid, ssid, 32);
    strncpy((char*)wifi_config.sta.password, pass, 64);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// I2S 输出 MAX98357A TTS功放
void i2s_tx_init(void)
{
    // 第一步：通道基础配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_TX);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // 第二步：标准模式参数
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = I2S_TX_BCLK,
            .ws = I2S_TX_LRCK,
            .dout = I2S_TX_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S TX(功放)初始化成功");
}

// I2S 输入 INMP441麦克风
void i2s_rx_init(void)
{
    // 第一步：通道基础配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_RX);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // 第二步：标准模式参数
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = I2S_RX_BCLK,
            .ws = I2S_RX_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_RX_DIN,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S RX(麦克风)初始化成功");
}

// 按键初始化
void key_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_SPEAK) | (1ULL << KEY_DOUBAO) | (1ULL << KEY_COZE) | (1ULL << KEY_XIAOZHI);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "4按键初始化完成");
    ESP_LOGI(TAG, "✅ 默认选中：豆包");
}

// 打印当前AI名字
void print_current_ai(void)
{
    switch(g_current_ai)
    {
        case AI_DOUBAO: ESP_LOGI(TAG, "👉 当前AI：豆包"); break;
        case AI_COZE: ESP_LOGI(TAG, "👉 当前AI：Coze"); break;
        case AI_XIAOZHI: ESP_LOGI(TAG, "👉 当前AI：小智"); break;
        default: break;
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32S3 Multi-AI Chatbot Start");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid_buf[33]={0}, pass_buf[65]={0};
    esp_err_t ret = read_wifi_nvs(ssid_buf, pass_buf);
    if(ret == ESP_OK && strlen(ssid_buf)>0)
    {
        ESP_LOGI(TAG,"📖 读取已保存WiFi: %s", ssid_buf);
        wifi_sta_init(ssid_buf, pass_buf);
    }else{
        ESP_LOGI(TAG,"⚠️ 无保存WiFi，开启配网AP热点");
        wifi_ap_init();
    }

    i2s_tx_init();
    i2s_rx_init();
    key_init();

    bool key_spk_last = true;
    bool key_db_last = true;
    bool key_cz_last = true;
    bool key_xz_last = true;

    while(1)
    {
        bool key_spk_now = gpio_get_level(KEY_SPEAK);
        bool key_db_now = gpio_get_level(KEY_DOUBAO);
        bool key_cz_now = gpio_get_level(KEY_COZE);
        bool key_xz_now = gpio_get_level(KEY_XIAOZHI);

        if(key_spk_now == 0 && key_spk_last == 1)
        {
            ESP_LOGI(TAG, "🎤 按下说话按键，开始录音");
        }
        if(key_spk_now == 1 && key_spk_last == 0)
        {
            ESP_LOGI(TAG, "🎤 松开说话按键，结束录音");
        }

        if(key_db_now == 0 && key_db_last == 1)
        {
            g_current_ai = AI_DOUBAO;
            print_current_ai();
        }
        if(key_cz_now == 0 && key_cz_last == 1)
        {
            g_current_ai = AI_COZE;
            print_current_ai();
        }
        if(key_xz_now == 0 && key_xz_last == 1)
        {
            g_current_ai = AI_XIAOZHI;
            print_current_ai();
        }

        key_spk_last = key_spk_now;
        key_db_last = key_db_now;
        key_cz_last = key_cz_now;
        key_xz_last = key_xz_now;

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
