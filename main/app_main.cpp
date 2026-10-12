#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include <cstring>

static const char *TAG = "AI_CHAT";

// ========== 硬件引脚定义 ==========
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

// AP热点配置（手机连接这个热点）
#define AP_SSID "ESP32S3_CHATBOT"
#define AP_PASS "12345678"

// 全局标记
bool g_wifi_connected = false;
httpd_handle_t web_server = NULL;

// ========== Web网页配网页面 ==========
static const char *html_page = R"HTML(
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>ESP32 WiFi配网</title></head>
<body>
<h2>填入2.4G WiFi信息</h2>
<form action="/setwifi" method="post">
SSID:<br><input name="ssid"><br>
密码:<br><input name="password"><br>
<button type="submit">保存并重启</button>
</form>
</body>
</html>
)HTML";

// 网页根路径
static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 接收WiFi表单提交
static esp_err_t setwifi_handler(httpd_req_t *req)
{
    char buf[128];
    size_t recv_len = httpd_req_get_url_query_len(req)+1;
    if(recv_len > sizeof(buf)) recv_len = sizeof(buf);
    httpd_req_get_url_query_str(req, buf, recv_len);

    char ssid[32]={0}, pass[64]={0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "password", pass, sizeof(pass));

    // 存入NVS
    nvs_handle_t nvs;
    nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "password", pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    httpd_resp_sendstr(req, "配置成功！设备即将重启...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t uri_root = {.uri="/", .method=HTTP_GET, .handler=get_root_handler};
static const httpd_uri_t uri_setwifi = {.uri="/setwifi", .method=HTTP_POST, .handler=setwifi_handler};

// 启动web服务器
void start_web_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&web_server, &config);
    httpd_register_uri_handler(web_server, &uri_root);
    httpd_register_uri_handler(web_server, &uri_setwifi);
    ESP_LOGI(TAG,"Web配网页面已启动，连接热点 %s",AP_SSID);
}

// 加载NVS里保存的WiFi
bool load_wifi_from_nvs(char *ssid, char *pwd)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if(err != ESP_OK) return false;
    size_t len=32;
    nvs_get_str(nvs, "ssid", ssid, &len);
    len=64;
    nvs_get_str(nvs, "password", pwd, &len);
    nvs_close(nvs);
    return (ssid[0] != 0);
}

// WiFi事件回调
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(!g_wifi_connected) {
            ESP_LOGW(TAG,"STA联网失败，切换到AP配网模式");
            esp_wifi_stop();
            // 开启AP热点
            wifi_config_t ap_cfg;
            memset(&ap_cfg,0,sizeof(ap_cfg));
            strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
            strcpy((char*)ap_cfg.ap.password, AP_PASS);
            ap_cfg.ap.max_connection = 1;
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
            esp_wifi_set_mode(WIFI_MODE_AP);
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            esp_wifi_start();
            start_web_server();
        }
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅WiFi联网成功！IP:" IPSTR, IP2STR(&event->ip_info.ip));
        if(web_server){
            httpd_stop(web_server);
            web_server=NULL;
        }
    }
}

void wifi_ap_sta_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    char ssid[32]={0}, pwd[64]={0};
    if(load_wifi_from_nvs(ssid,pwd))
    {
        ESP_LOGI(TAG,"NVS读取到WiFi，尝试连接：%s",ssid);
        wifi_config_t sta_cfg;
        memset(&sta_cfg,0,sizeof(sta_cfg));
        strcpy((char*)sta_cfg.sta.ssid, ssid);
        strcpy((char*)sta_cfg.sta.password, pwd);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_start();
    }else{
        ESP_LOGI(TAG,"NVS无WiFi配置，直接启动AP配网热点");
        wifi_config_t ap_cfg;
        memset(&ap_cfg,0,sizeof(ap_cfg));
        strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
        strcpy((char*)ap_cfg.ap.password, AP_PASS);
        ap_cfg.ap.max_connection = 1;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_start();
        start_web_server();
    }
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
    ESP_LOGI(TAG, "==== ESP32S3 AI ChatBot 启动 ====");
    wifi_ap_sta_init();
    i2s_mic_init();
    i2s_spk_init();
    key_init();
    ESP_LOGI(TAG, "I2S + KEY Init OK，GPIO1按键按住录音");

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
            ESP_LOGD(TAG, "采集到 %d PCM样本", (int)samples);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
