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
#include <cstdio>

static const char *TAG = "AI_CHAT";

// ========== 硬件引脚定义 ==========
#define KEY_TALK        GPIO_NUM_1
#define KEY_SWITCH_AI   GPIO_NUM_2

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

// AP热点【无密码开放热点】
#define AP_SSID "ESP32S3_CHATBOT"

// AI代理枚举
typedef enum {
    AI_XIAOZHI = 0,
    AI_COZE,
    AI_DOUBAO,
    AI_MAX
} AiAgentType;
const char* g_ai_name[] = {"小智", "Coze", "豆包"};
AiAgentType g_current_ai = AI_DOUBAO;

bool g_wifi_connected = false;
httpd_handle_t web_server = NULL;

// ========== 网页：WiFi扫描配网页面 ==========
static const char *html_page = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>ESP32 WiFi配网</title>
<script>
async function scanWifi(){
  const res = await fetch('/scan');
  const data = await res.json();
  const sel = document.getElementById('wifiList');
  sel.innerHTML = "";
  data.wifi.forEach(item=>{
    const opt = document.createElement('option');
    opt.value = item.ssid;
    opt.textContent = item.ssid + (item.is_open ? " 【无密码】" : "");
    opt.dataset.open = item.is_open;
    sel.appendChild(opt);
  })
}
function selectWifi(){
  const sel = document.getElementById('wifiList');
  const opt = sel.options[sel.selectedIndex];
  document.getElementById('ssid').value = opt.value;
  const isOpen = opt.dataset.open === "true";
  document.getElementById('pwdRow').style.display = isOpen ? "none" : "block";
}
window.onload = scanWifi;
</script>
</head>
<body>
<h2>附近2.4G WiFi列表</h2>
<select id="wifiList" size="8" onchange="selectWifi()">
  <option>正在扫描WiFi...</option>
</select>
<br>
<form action="/setwifi" method="post">
SSID:<br><input id="ssid" name="ssid" readonly><br>
<div id="pwdRow">密码:<br><input name="password"><br></div>
<button type="submit">保存并重启设备</button>
</form>
</body>
</html>
)HTML";

// 扫描WiFi接口
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    esp_wifi_scan_start(NULL, true);
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t ap_records[20];
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);

    char json_buf[1024];
    int pos = sprintf(json_buf, "{\"wifi\":[");
    for(int i=0; i<ap_num; i++)
    {
        wifi_ap_record_t *r = &ap_records[i];
        bool is_open = (r->authmode == WIFI_AUTH_OPEN);
        pos += sprintf(json_buf+pos, "{\"ssid\":\"%s\",\"is_open\":%s},", r->ssid, is_open?"true":"false");
    }
    if(ap_num>0) json_buf[pos-1] = ']';
    else strcpy(json_buf+pos, "]}");
    httpd_resp_sendstr(req, json_buf);
    return ESP_OK;
}

static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t setwifi_handler(httpd_req_t *req)
{
    char buf[128];
    size_t recv_len = httpd_req_get_url_query_len(req)+1;
    if(recv_len > sizeof(buf)) recv_len = sizeof(buf);
    httpd_req_get_url_query_str(req, buf, recv_len);

    char ssid[32]={0}, pass[64]={0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "password", pass, sizeof(pass));

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
static const httpd_uri_t uri_scan = {.uri="/scan", .method=HTTP_GET, .handler=wifi_scan_handler};
static const httpd_uri_t uri_setwifi = {.uri="/setwifi", .method=HTTP_POST, .handler=setwifi_handler};

void start_web_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&web_server, &config);
    httpd_register_uri_handler(web_server, &uri_root);
    httpd_register_uri_handler(web_server, &uri_scan);
    httpd_register_uri_handler(web_server, &uri_setwifi);
    ESP_LOGI(TAG,"✅配网热点已启动：%s（无密码，直接连接）",AP_SSID);
}

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

// 加载保存的AI选择
void load_ai_select(void)
{
    nvs_handle_t nvs;
    if(nvs_open("ai_cfg", NVS_READONLY, &nvs) == ESP_OK)
    {
        uint8_t val=0;
        nvs_get_u8(nvs, "ai_sel", &val);
        if(val < AI_MAX) g_current_ai = (AiAgentType)val;
        nvs_close(nvs);
    }
    ESP_LOGI(TAG,"✅ 当前选中AI：%s", g_ai_name[g_current_ai]);
}
// 保存AI选择到NVS
void save_ai_select(void)
{
    nvs_handle_t nvs;
    nvs_open("ai_cfg", NVS_READWRITE, &nvs);
    nvs_set_u8(nvs, "ai_sel", (uint8_t)g_current_ai);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG,"🔄 切换AI至：%s", g_ai_name[g_current_ai]);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(!g_wifi_connected) {
            ESP_LOGW(TAG,"STA联网失败，切换AP配网模式");
            esp_wifi_stop();
            wifi_config_t ap_cfg;
            memset(&ap_cfg,0,sizeof(ap_cfg));
            strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
            ap_cfg.ap.max_connection = 1;
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
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

    load_ai_select();
    char ssid[32]={0}, pwd[64]={0};
    if(load_wifi_from_nvs(ssid,pwd))
    {
        ESP_LOGI(TAG,"读取NVS保存WiFi，尝试连接：%s",ssid);
        wifi_config_t sta_cfg;
        memset(&sta_cfg,0,sizeof(sta_cfg));
        strcpy((char*)sta_cfg.sta.ssid, ssid);
        strcpy((char*)sta_cfg.sta.password, pwd);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_start();
    }else{
        ESP_LOGI(TAG,"NVS无WiFi配置，启动无密码AP热点");
        wifi_config_t ap_cfg;
        memset(&ap_cfg,0,sizeof(ap_cfg));
        strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
        ap_cfg.ap.max_connection = 1;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
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

// 两个按键初始化
void key_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_TALK) | (1ULL << KEY_SWITCH_AI);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
}

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
    ESP_LOGI(TAG, "GPIO1=对话按键, GPIO2=切换AI(小智/Coze/豆包)");

    bool is_recording = false;
    bool key_switch_last = true;

    while(1)
    {
        int talk_level = gpio_get_level(KEY_TALK);
        int switch_level = gpio_get_level(KEY_SWITCH_AI);

        // ===== GPIO1 对话录音按键 =====
        if(talk_level == 0 && !is_recording)
        {
            ESP_LOGI(TAG,"【%s】按键按下，开始录音...", g_ai_name[g_current_ai]);
            is_recording = true;
        }
        else if(talk_level == 1 && is_recording)
        {
            ESP_LOGI(TAG,"【%s】松开按键，停止录音，准备发送请求", g_ai_name[g_current_ai]);
            is_recording = false;
        }

        if(is_recording)
        {
            size_t samples = read_mic_pcm(mic_buffer, RECORD_BUF_SAMPLES);
        }

        // ===== GPIO2 切换AI（边沿检测，防抖）=====
        if(switch_level == 0 && key_switch_last)
        {
            vTaskDelay(pdMS_TO_TICKS(20)); //防抖
            if(gpio_get_level(KEY_SWITCH_AI)==0)
            {
                g_current_ai = (AiAgentType)((g_current_ai + 1) % AI_MAX);
                save_ai_select();
            }
        }
        key_switch_last = (switch_level == 1);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
