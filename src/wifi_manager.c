#include "wifi_manager.h"
#include "secrets.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_console.h"

static const char *TAG = "wifi_manager";
#define WIFI_CONNECT_SUCCESS_BIT   BIT0
#define WIFI_CONNECT_FAIL_BIT      BIT1
#define WIFI_CONNECTING_BIT        BIT2
#define WIFI_SCANNING              BIT3
#define CONNECTING_LED_PIN         GPIO_NUM_7
#define WIFI_MANAGER_RETRY_COUNT        5


static struct {
    EventGroupHandle_t wifi_status_event_group;
    int retry_count;
    esp_netif_t* esp_netif;
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    int disconnect_count;

} wifi_manager;


static void status_blink_task(void *pvParameter) {
    gpio_reset_pin(CONNECTING_LED_PIN);
    gpio_set_direction(CONNECTING_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONNECTING_LED_PIN, 0);

    while(1) {
        EventBits_t bits = xEventGroupWaitBits(wifi_manager.wifi_status_event_group,
        WIFI_CONNECTING_BIT | WIFI_CONNECT_SUCCESS_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_SCANNING,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
        
        if((bits & WIFI_CONNECT_SUCCESS_BIT) == WIFI_CONNECT_SUCCESS_BIT) {
            gpio_set_level(CONNECTING_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(40));
            gpio_set_level(CONNECTING_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(4960));
        }
        
        else if((bits & WIFI_SCANNING) == WIFI_SCANNING) {
            gpio_set_level(CONNECTING_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(1960));
            gpio_set_level(CONNECTING_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(40));
        }

        else if((bits & WIFI_CONNECT_FAIL_BIT) == WIFI_CONNECT_FAIL_BIT) {
            gpio_set_level(CONNECTING_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        else if((bits & WIFI_CONNECTING_BIT) == WIFI_CONNECTING_BIT) {
            gpio_set_level(CONNECTING_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            gpio_set_level(CONNECTING_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        
        else {
            gpio_set_level(CONNECTING_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}


static void wifi_manager_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    /* Process WiFi related events */
    if (event_base == WIFI_EVENT) {
        if(event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect(); //todo check error
            xEventGroupSetBits(wifi_manager.wifi_status_event_group, WIFI_CONNECTING_BIT);
        } 

        else if (event_id == WIFI_EVENT_SCAN_DONE) {
            ESP_LOGI(TAG, "Scan done");
        }        
        
        else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "Station connected");
        }
        
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_manager.disconnect_count++;
            if (wifi_manager.retry_count < WIFI_MANAGER_RETRY_COUNT) {
                esp_wifi_connect(); //todo check error
                wifi_manager.retry_count++;
                ESP_LOGI(TAG, "retry to connect to the AP");
                xEventGroupSetBits(wifi_manager.wifi_status_event_group, WIFI_CONNECTING_BIT);
            } 
            else {
                xEventGroupClearBits(wifi_manager.wifi_status_event_group, WIFI_CONNECTING_BIT);
                xEventGroupSetBits(wifi_manager.wifi_status_event_group, WIFI_CONNECT_FAIL_BIT);
            }
            ESP_LOGI(TAG,"Connect to the AP fail");
        }

        else {
            ESP_LOGI(TAG, "UNEXPECTED WIFI EVENT %d", (int)event_id);
        }
    } 

    /* Process IP related events */
    if (event_base == IP_EVENT) {
        if(event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            wifi_manager.retry_count = 0;
            xEventGroupSetBits(wifi_manager.wifi_status_event_group, WIFI_CONNECT_SUCCESS_BIT);
            xEventGroupClearBits(wifi_manager.wifi_status_event_group, WIFI_CONNECTING_BIT);
        }
        else {
            ESP_LOGI(TAG, "UNEXPECTED IP EVENT %d", (int)event_id);
        }
    }
}

esp_err_t wifi_manager_start(void) {
    /* Init data structure */
    ESP_LOGI(TAG, "Initializing wifi manager...");
    memset(&wifi_manager, 0, sizeof(wifi_manager));
    wifi_manager.wifi_status_event_group = xEventGroupCreate();
    wifi_manager.disconnect_count = 0;

    /* Create status blink task */
    xEventGroupSetBits(wifi_manager.wifi_status_event_group, WIFI_CONNECTING_BIT);
    xTaskCreate(status_blink_task, "Status blink", 2048, NULL, 5, NULL);

    /* Setup NVS */
    ESP_LOGI(TAG, "Setting up NVS...");

    esp_err_t ret = ESP_OK;
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if(ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed");
            return ret;
        }
        ret = nvs_flash_init();
        if(ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init failed");
            return ret;
        }
    }

    /* Starting wifi manager */
    ESP_LOGI(TAG, "Starting wifi manager...");


    ret = esp_netif_init();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed");
        return ret;
    }

    ret = esp_event_loop_create_default();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed");
        return ret;
    }

    wifi_manager.esp_netif = esp_netif_create_default_wifi_sta();
    if(!wifi_manager.esp_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return ret;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_manager_event_handler,
        NULL,
        &wifi_manager.instance_any_id
    );

    ret = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_manager_event_handler,
        NULL,
        &wifi_manager.instance_got_ip
    );
    
    
    /* WiFi config */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed");
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed");
        return ret;
    }
    
    ret = esp_wifi_start();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return ret;
    }

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    //  * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    // // EventBits_t bits = xEventGroupWaitBits(wifi_manager.wifi_event_group,
    // //         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    // //         pdFALSE,
    // //         pdFALSE,
    // //         portMAX_DELAY);

    // /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    //  * happened. */
    // // if (bits & WIFI_CONNECTED_BIT) {
    // //     ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
    // //              EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    // // } else if (bits & WIFI_FAIL_BIT) {
    // //     ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
    // //              EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    // // } else {
    // //     ESP_LOGE(TAG, "UNEXPECTED EVENT");
    // // }



    // ESP_LOGI(TAG, "wifi_manager started!");
    return ESP_OK;
}
