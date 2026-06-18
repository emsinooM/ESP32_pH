/**
 * Demo: log đọc DO KOG-206 qua Serial Monitor 115200
 */

#include "do_sensor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

static void on_do_reading(const do_sensor_reading_t *reading, void *user_ctx)
{
    (void)user_ctx;
    if (!reading->valid) {
        ESP_LOGW(TAG, "callback: loi code=%d", reading->error_code);
        return;
    }
    ESP_LOGI(TAG, ">>> DO=%.2f mg/L | sat=%.1f%% | temp=%.1f C",
             reading->do_mg_l, reading->saturation_pct, reading->temp_c);
}

void app_main(void)
{
    do_sensor_config_t cfg = DO_SENSOR_CONFIG_DEFAULT();
    cfg.slave_id = 5;
    cfg.poll_interval_ms = 10000;  /* 10 giây/lần — chỉnh số này khi test (vd: 5000, 30000) */
    cfg.debug = false;             /* true = in them hex RX Modbus */

    ESP_LOGI(TAG, "Doc DO moi %lu giay/lan", (unsigned long)cfg.poll_interval_ms / 1000);

    ESP_ERROR_CHECK(do_sensor_init(&cfg));
    ESP_ERROR_CHECK(do_sensor_start(on_do_reading, NULL));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
