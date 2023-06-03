#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "wifi_manager.h"
static const char *TAG = "main";


void app_main() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    esp_err_t ret = wifi_manager_start();
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start failed");
        ESP_ERROR_CHECK(ret);
    }
    
    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}