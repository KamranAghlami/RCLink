#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <lvgl/lvgl.h>

void parallel_task(void *parameter)
{
    ESP_LOGI("application", "Hello from core %d!", xPortGetCoreID());

    while (true)
        vTaskDelay(1000 / portTICK_PERIOD_MS);
}

extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(parallel_task, "parallel_task", 10000, NULL, 1, NULL, !xPortGetCoreID());

    ESP_LOGI("application", "Hello from core %d!", xPortGetCoreID());

    lv_init();

    auto timer_xcb = [](lv_timer_t *timer)
    {
        ESP_LOGI("application", "Hello from lvgl");
    };

    lv_timer_create(timer_xcb, 1000, nullptr);

    while (true)
        vTaskDelay(pdMS_TO_TICKS(lv_timer_handler()));

    esp_restart();
}
