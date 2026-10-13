#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <cstdlib>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs.h"

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
static httpd_handle_t g_web_server = nullptr;
static SemaphoreHandle_t wifi_scan_sem = nullptr;
static wifi_ap_record_t scan_list[16];
static uint16_t scan_count = 0;
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

// ========== 函数前置声明 ==========
httpd_handle_t start_webserver(httpd_handle_t *server);
void stop_webserver(httpd_handle_t server);
void wifi_start_ap(void);
void wifi_init_sta(const char* ssid, const char* pwd);
void audio_start_capture_and_upload(void);
esp_err_t nvs_save_wifi(const char* ssid, const char* pass);
esp_err_t nvs_load_wifi(char* out_ssid, char* out_pwd, size_t buf_len);
void start_wifi_scan(void);

// ===================== NVS读写WiFi账号密码 =====================
esp_err_t nvs_save_wifi(const char* ssid, const char* pass)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
    if(err != ESP_OK) return err;

    nvs_set_str(nvs_handle, "ssid", ssid);
    nvs_set_str(nvs_handle, "pass", pass);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi配置已存入NVS");
    return ESP_OK;
}

esp_err_t nvs_load_wifi(char* out_ssid, char* out_pwd, size_t buf_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle);
    if(err != ESP_OK) return err;

    size_t len = buf_len;
    err = nvs_get_str(nvs_handle, "ssid", out_ssid, &len);
    if(err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    len = buf_len;
    err = nvs_get_str(nvs_handle, "pass", out_pwd, &len);
    nvs_close(nvs_handle);
    return err;
}

// ===================== WiFi扫描回调 =====================
static void wifi_scan_done_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_id == WIFI_EVENT_SCAN_DONE)
    {
        esp_wifi_scan_get_ap_records(&scan_count, scan_list);
        ESP_LOGI(TAG, "WiFi扫描完成，找到 %d 个AP", scan_count);
        xSemaphoreGive(wifi_scan_sem);
    }
}

void start_wifi_scan(void)
{
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_wifi_scan_start(&scan_cfg, false);
}

// ===================== HTTP网页接口 =====================
// 主页配网页面
static esp_err_t web_root_handler(httpd_req_t *req)
{
    char html[4096];
    char opt_buf[512] = {0};
    opt_buf[0] = 0;

    xSemaphoreTake(wifi_scan_sem, portMAX_DELAY);
    for(int i=0; i<scan_count; i++)
    {
        char ssid[33];
        memcpy(ssid, scan_list[i].ssid, 32);
        ssid[32]=0;
        // 无密码标记
        const char* lock = (scan_list[i].authmode == WIFI_AUTH_OPEN) ? "🔓" : "🔒";
        sprintf(opt_buf + strlen(opt_buf), "<option value=\"%s\">%s %s (RSSI:%d)</option>", ssid, lock, ssid, scan_list[i].rssi);
    }
    xSemaphoreGive(wifi_scan_sem);

    const char* page_template = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>ESP32S3 AI 配网</title>
<style>
body{font-family:Arial;padding:20px;max-width:450px;margin:auto;}
.box{border:1px solid #ccc;padding:20px;border-radius:10px;}
select,input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;font-size:16px;}
button{background:#007bff;color:#fff;border:none;padding:12px;width:100%;font-size:18px;border-radius:6px;cursor:pointer;}
</style>
</head>
<body>
<div class="box">
<h2>ESP32S3 AI 配网</h2>
<form action="/savewifi" method="POST">
<label>选择WiFi</label>
<select name="ssid">%s</select>
<label>WiFi密码（无密码留空）</label>
<input type="text" name="password" placeholder="无密码WiFi不需要填写">
<button type="submit">保存并重启设备</button>
</form>
</div>
</body>
</html>
)HTML";
    snprintf(html, sizeof(html), page_template, opt_buf);
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 保存WiFi提交接口
static esp_err_t web_savewifi_handler(httpd_req_t *req)
{
    char buf[512];
    size_t recv_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if(recv_len > sizeof(buf)) recv_len = sizeof(buf)-1;
    httpd_req_recv(req, buf, recv_len);
    buf[recv_len] = 0;

    char ssid[33] = {0};
    char pass[64] = {0};
    sscanf(buf, "ssid=%32[^&]&password=%63s", ssid, pass);

    nvs_save_wifi(ssid, pass);
    ESP_LOGI(TAG, "保存WiFi SSID:%s", ssid);

    const char* ok_page = "<html><body><h2>保存成功！设备正在重启...</h2></body></html>";
    httpd_resp_send(req, ok_page, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

httpd_uri_t uri_root = {.uri="/", .method=HTTP_GET, .handler=web_root_handler, .user_ctx=nullptr};
httpd_uri_t uri_save = {.uri="/savewifi", .method=HTTP_POST, .handler=web_savewifi_handler, .user_ctx=nullptr};

httpd_handle_t start_webserver(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Web配网服务启动");
        httpd_register_uri_handler(*server, &uri_root);
        httpd_register_uri_handler(*server, &uri_save);
        start_wifi_scan();
    }
    return *server;
}

void stop_webserver(httpd_handle_t server)
{
    if(server) httpd_stop(server);
}

// ===================== WiFi事件回调 =====================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi断开，重连");
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
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_scan_done_handler, nullptr, nullptr);

    wifi_config_t wifi_config;
    memset(&wifi_config,0,sizeof(wifi_config_t));
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pwd);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA模式，尝试连接: %s", ssid);
}

// AP热点（无密码，网页配网）
void wifi_start_ap(void)
{
    if(g_web_server != nullptr) return;
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_scan_done_handler, nullptr, nullptr);

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(wifi_config_t));

    strcpy((char *)ap_cfg.ap.ssid, "ESP32S3-AI");
    ap_cfg.ap.ssid_len = strlen("ESP32S3-AI");
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.dtim_period = 1;
    ap_cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_NONE;
    ap_cfg.ap.ftm_responder = false;
    ap_cfg.ap.pmf_cfg.capable = false;
    ap_cfg.ap.pmf_cfg.required = false;
    ap_cfg.ap.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP热点开启，名称ESP32S3-AI，无密码。访问 192.168.4.1");

    g_web_server = start_webserver(&g_web_server);
}

// ===================== I2S音频采集占位 =====================
void audio_start_capture_and_upload(void)
{
    ESP_LOGI(TAG, "I2S麦克风采集PCM，上传云端API，TTS返回播放");
}

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
                ESP_LOGI(TAG, "按下按键: GPIO %" PRIu32, io_num);
                switch(io_num)
                {
                    case KEY_TALK:
                        ESP_LOGI(TAG, "对话按键按下");
                        if(!g_wifi_connected) {
                            ESP_LOGW(TAG, "无WiFi，进入AP配网模式");
                            wifi_start_ap();
                        } else {
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

// ===================== 初始化按键GPIO =====================
void gpio_key_init(void)
{
    gpio_config_t io_conf;
    memset(&io_conf,0,sizeof(gpio_config_t));
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    io_conf.pin_bit_mask = (1ULL << KEY_TALK) | (1ULL << KEY_SWITCH_1) | (1ULL << KEY_SWITCH_2) | (1ULL << KEY_SWITCH_3);
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(key_task, "key_task", 4096, nullptr, 10, nullptr);

    gpio_isr_handler_add(KEY_TALK, gpio_isr_handler, (void*)KEY_TALK);
    gpio_isr_handler_add(KEY_SWITCH_1, gpio_isr_handler, (void*)KEY_SWITCH_1);
    gpio_isr_handler_add(KEY_SWITCH_2, gpio_isr_handler, (void*)KEY_SWITCH_2);
    gpio_isr_handler_add(KEY_SWITCH_3, gpio_isr_handler, (void*)KEY_SWITCH_3);
    gpio_install_isr_service(0);
}

// ===================== 程序入口 =====================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32S3 AI语音助手启动");
    wifi_scan_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(wifi_scan_sem);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_key_init();

    char saved_ssid[33] = {0};
    char saved_pwd[64] = {0};
    ret = nvs_load_wifi(saved_ssid, saved_pwd, sizeof(saved_ssid));
    if(ret == ESP_OK && strlen(saved_ssid) > 0)
    {
        ESP_LOGI(TAG, "读取NVS保存WiFi: %s", saved_ssid);
        wifi_init_sta(saved_ssid, saved_pwd);
    }
    else
    {
        ESP_LOGI(TAG, "NVS无WiFi配置，直接开启AP配网");
        wifi_start_ap();
    }
}
