#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

static const char *TAG = "main";
static esp_mqtt_client_handle_t s_mqtt_client;
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Connecting WiFi");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Connected!");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "WiFi Disconnected, attempting to reconnect...");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Obtained IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

typedef struct
{
    int16_t frame_cntl;
    int16_t duration;
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    int16_t seq_cntl;
    uint8_t payload[];

} __attribute__((packed)) wifi_mgmt_pkt_t;

#define SSID_MAX_LEN 32

static void mgmt_pkt_cb(wifi_promiscuous_pkt_t *prom_pkt)
{
    wifi_mgmt_pkt_t *pkt = (wifi_mgmt_pkt_t *)prom_pkt->payload;

    pkt->frame_cntl = ntohs(pkt->frame_cntl);

    if ((pkt->frame_cntl & 0xFF00) == 0x4000) // probe request
    {
        uint8_t ssid_len = pkt->payload[1];

        if (ssid_len > SSID_MAX_LEN)
            ssid_len = SSID_MAX_LEN;

        if (ssid_len)
        {
            char ssid[SSID_MAX_LEN + 1];

            memcpy(ssid, pkt->payload + 2, ssid_len);
            ssid[ssid_len] = 0;

            char tmp[256];
            snprintf(tmp, sizeof(tmp), "{\"type\":\"wifi-probe\",\"ts\":\"%llu\",\"da\":\"" MACSTR "\",\"sa\":\"" MACSTR "\",\"ssid\":\"%s\",\"rssi\":\"%d\"}",
                     time(NULL), MAC2STR(pkt->da), MAC2STR(pkt->sa), ssid, prom_pkt->rx_ctrl.rssi);

            ESP_LOGI(TAG, "%s", tmp);
            esp_mqtt_client_publish(s_mqtt_client, CONFIG_MQTT_TOPIC "/event", tmp, 0, 1, false);
        }
    }
}

static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    switch (type)
    {
    case WIFI_PKT_MGMT:
        mgmt_pkt_cb(pkt);
        break;
    default:
        break;
    }
}

static void start_mqtt()
{
    ESP_LOGI(TAG, "Configuring mqtt");
    esp_mqtt_client_config_t mqtt_cfg = {.broker.address.uri = CONFIG_MQTT_BROKER_URL};
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    esp_mqtt_client_publish(s_mqtt_client, CONFIG_MQTT_TOPIC "/status", "started", 0, 1, false);
}

void start_ntp()
{
    ESP_LOGI(TAG, "Configuring ntp...");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NTP_SERVER);
    esp_netif_sntp_init(&config);
    ESP_ERROR_CHECK(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)));
    ESP_LOGI(TAG, "Time set!");
}

static void start_wifi()
{
    ESP_LOGI(TAG, "Initializing networking");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "Initializing main event-loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing WiFi");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "esp32-wifi-monitor");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("US", true));

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

    wifi_config_t sta_wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PSK,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK}};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi Connected");
}

static void start_wifi_monitor()
{
    ESP_LOGI(TAG, "Enabling promiscuous mode");
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting");
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Initializing non-volatile storage");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    start_wifi();
    start_ntp();
    start_mqtt();
    start_wifi_monitor();

    ESP_LOGI(TAG, "Ready.");
}