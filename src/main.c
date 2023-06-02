#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"


void app_main() {
    ESP_LOGI("main", "Hello world!");
    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}