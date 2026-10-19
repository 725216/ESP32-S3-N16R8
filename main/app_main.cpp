#include <iostream>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "brookesia.h"

static const char *TAG = "MULTI_AGENT_WIFI_CFG";

// ========== 按键定义 ==========
#define BTN_TALK_GPIO      GPIO_NUM_1
#define BTN_XIAOZHI_GPIO   GPIO_NUM_2
#define BTN_COZE_GPIO      GPIO_NUM_3
#define BTN_DOUBAO_GPIO    GPIO_NUM_4
#define RGB_LED_GPIO       GPIO_NUM_48

// ========== WIFI配网定义 ==========
#define AP_SSID "ESP32S3-AI配网"
#define AP_PASS "12345678"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool is_ap_mode = false;

// ========== AI代理枚举 ==========
enum AgentType {
    AGENT_XIAOZHI,
    AGENT_COZE,
    AGENT_DOUBAO
};
AgentType current_agent = AGENT_XIAOZHI;
static led_strip_handle_t led_strip;

// ========== RGB灯光 ==========
void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

// ========== AI切换 ==========
void switch_agent(AgentType target)
{
    if(current_agent == target) return;
    current_agent = target;
    brookesia_stop_session();
    switch(target)
    {
        case AGENT_XIAOZHI:
            ESP_LOGI(TAG, ">>> 切换到 小智AI");
            set_rgb(0,30,0); //绿色
            brookesia_set_agent("xiaozhi");
            break;
        case AGENT_COZE:
            ESP_LOGI(TAG, ">>> 切换到 Coze扣子");
            set_rgb(30,0,30); //紫色
            brookesia_set_agent("coze");
            break;
        case AGENT_DOUBAO:
            ESP_LOGI(TAG, ">>> 切换到 豆包");
            set_rgb(30,30,0); //黄色
            brookesia_set_agent("doubao");
            break;
    }
}

// ========== HTTP网页配网接口 ==========
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* resp = R"HTML(
<html>
<head><meta charset="utf-8"><title>ESP32S3 AI配网</title></head>
<body>
<h2>WiFi配置</h2>
<form action="/setwifi" method="post">
SSID: <input name="ssid"><br>
密码: <input name="pwd"><br>
<button type="submit">保存并重启</button>
</form>
</body>
</html>
)HTML";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t setwifi_post_handler(httpd_req_t *req)
{
    char buf[128];
    httpd_req_recv(req, buf, sizeof(buf));
    char ssid[32]={0}, pwd[64]={0};
    sscanf(buf,"ssid=%[^&]&pwd=%s",ssid,pwd);

    nvs_handle_t nvs;
    nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    nvs_set_str(nvs,"ssid",ssid);
    nvs_set_str(nvs,"pwd",pwd);
    nvs_commit(nvs);
    nvs_close(nvs);

    httpd_resp_send(req,"OK，设备重启连接WiFi",HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_uri_t root = {.uri="/",.method=HTTP_GET,.handler=root_get_handler};
    httpd_uri_t setwifi = {.uri="/setwifi",.method=HTTP_POST,.handler=setwifi_post_handler};

    httpd_start(&server, &config);
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &setwifi);
    return server;
}

// ========== WiFi事件回调 ==========
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(s_retry_num < 5){
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG,"WiFi断开，重试 %d",s_retry_num);
        }else{
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG,"WiFi已连接,ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// STA模式：连接保存的WiFi
void wifi_start_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_event_handler,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_event_handler,NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    nvs_handle_t nvs;
    char ssid[32]={0},pwd[64]={0};
    nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    nvs_get_str(nvs,"ssid",ssid,sizeof(ssid));
    nvs_get_str(nvs,"pwd",pwd,sizeof(pwd));
    nvs_close(nvs);

    if(strlen(ssid)==0){
        ESP_LOGW(TAG,"无保存WiFi，直接开启AP配网");
        is_ap_mode=true;
        return;
    }

    wifi_config_t wifi_cfg={};
    strcpy((char*)wifi_cfg.sta.ssid,ssid);
    strcpy((char*)wifi_cfg.sta.password,pwd);
    wifi_cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG,"STA模式启动，尝试连接：%s",ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,WIFI_CONNECTED_BIT|WIFI_FAIL_BIT,pdFALSE,pdTRUE,pdMS_TO_TICKS(15000));
    if(bits & WIFI_CONNECTED_BIT){
        set_rgb(0,0,20); //蓝色：WiFi联网成功
        is_ap_mode=false;
    }else{
        ESP_LOGW(TAG,"WiFi多次连接失败，切AP热点配网");
        is_ap_mode=true;
    }
    esp_event_handler_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_event_handler);
    vEventGroupDelete(s_wifi_event_group);
}

// AP模式：开启热点网页配网
void wifi_start_ap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t ap_cfg={};
    strcpy((char*)ap_cfg.ap.ssid,AP_SSID);
    strcpy((char*)ap_cfg.ap.password,AP_PASS);
    ap_cfg.ap.max_connection=4;
    ap_cfg.ap.authmode=WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG,"AP热点开启：%s，密码：%s",AP_SSID,AP_PASS);
    set_rgb(40,15,0); //橙色：配网模式
    start_webserver();
}

// ========== 按键扫描任务 ==========
void btn_task(void *arg)
{
    gpio_set_direction(BTN_TALK_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_TALK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_XIAOZHI_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_XIAOZHI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_COZE_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_COZE_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(BTN_DOUBAO_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_DOUBAO_GPIO, GPIO_PULLUP_ONLY);

    while(1)
    {
        //切换按键
        if(gpio_get_level(BTN_XIAOZHI_GPIO) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if(gpio_get_level(BTN_XIAOZHI_GPIO) == 0)
            {
                switch_agent(AGENT_XIAOZHI);
                while(gpio_get_level(BTN_XIAOZHI_GPIO)==0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if(gpio_get_level(BTN_COZE_GPIO) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if(gpio_get_level(BTN_COZE_GPIO) == 0)
            {
                switch_agent(AGENT_COZE);
                while(gpio_get_level(BTN_COZE_GPIO)==0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if(gpio_get_level(BTN_DOUBAO_GPIO) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if(gpio_get_level(BTN_DOUBAO_GPIO) == 0)
            {
                switch_agent(AGENT_DOUBAO);
                while(gpio_get_level(BTN_DOUBAO_GPIO)==0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        //对话按键GPIO1 按住说话
        if(gpio_get_level(BTN_TALK_GPIO) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(30));
            if(gpio_get_level(BTN_TALK_GPIO) == 0)
            {
                if(is_ap_mode){
                    ESP_LOGW(TAG,"配网模式无法对话");
                }else{
                    ESP_LOGI(TAG, "开始录音对话");
                    brookesia_start_session();
                    while(gpio_get_level(BTN_TALK_GPIO)==0) vTaskDelay(pdMS_TO_TICKS(10));
                    ESP_LOGI(TAG, "松开，发送语音");
                    brookesia_stop_session();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void)
{
    //初始化NVS（保存WiFi账号密码）
    ESP_ERROR_CHECK(nvs_flash_init());

    //RGB初始化
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10000000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_strip));
    set_rgb(10,10,10);

    //WiFi逻辑
    wifi_start_sta();
    if(is_ap_mode){
        wifi_start_ap();
    }

    //语音框架
    brookesia_cfg_t b_cfg = BROOKESIA_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(brookesia_init(&b_cfg));
    switch_agent(AGENT_XIAOZHI);

    //按键任务
    xTaskCreate(btn_task, "btn_scan", 4*1024, NULL, 5, NULL);
}
