#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "cs1237.h"
#include "filter.h"
#include "ph_temp.h"
#include "screen_disp.h"

static const char *TAG = "PH_TEMP_SYSTEM";

PhCalibration_t ph_cal;
MovingAverage_t ph_filter;
MovingAverage_t temp_filter;

void app_main(void) {
    init_system_gpios();
    
    init_moving_average(&temp_filter);
    // init_moving_average(&ph_filter);

    vTaskDelay(pdMS_TO_TICKS(300));

    // --- KHỞI TẠO MÀN HÌNH LCD ---
    if (LCD_Init() == LCD_OK) {
        LCD_Start_Task();
    } else {
        ESP_LOGE(TAG, "Khởi tạo LCD thất bại!");
    }


    // Cấu hình cả hai chip: 40Hz, PGA = 1, Kênh A (0x10)
    // write_cs1237_config(PH_SCLK_PIN, PH_DATA_PIN, CS1237_CFG_40HZ_PGA1_CHA);
    write_cs1237_config(TEMP_SCLK_PIN, TEMP_DATA_PIN, CS1237_CFG_40HZ_PGA1_CHA);

    // --- Giả lập cấu hình dữ liệu hiệu chuẩn mẫu của người dùng ---
    // Ví dụ: Nhúng dung dịch 7.00 ở 25C đo được -15mV, nhúng dung dịch 4.01 ở 26C đo được 162mV
    // ph_cal.is_calibrated = false; // Đặt là false ban đầu
    // update_ph_calibration(&ph_cal, 24.2f, 25.0f, 195.6f, 25.0f);

    ESP_LOGI(TAG, "Đã khởi tạo hệ thống giám sát pH & Nhiệt độ ATC hoàn chỉnh.");

    while (1) {
        // 1. KÊNH NHIỆT ĐỘ
        int32_t temp_raw = read_cs1237_raw(TEMP_SCLK_PIN, TEMP_DATA_PIN);
        int32_t temp_filtered = apply_moving_average(&temp_filter, temp_raw);
        float current_temp = calculate_temperature(temp_filtered, true); // true = PT1000

        // 2. KÊNH pH (Tạm tắt)
        // int32_t ph_raw = read_cs1237_raw(PH_SCLK_PIN, PH_DATA_PIN);
        // int32_t ph_filtered = apply_moving_average(&ph_filter, ph_raw);
        // float v_probe_mv = 0;
        // float current_ph = calculate_ph_with_atc_calibrated(&ph_cal, ph_filtered, current_temp, &v_probe_mv);

        // 3. IN KẾT QUẢ
        // ESP_LOGI(TAG, "Nhiệt độ: %.2f C (ADC Temp: %" PRId32 ") | ADC pH: %8" PRId32 " | V_Probe: %7.2f mV | pH (ATC): %.2f", 
        // current_temp, temp_filtered, ph_filtered, v_probe_mv, current_ph);
        ESP_LOGI(TAG, "Nhiệt độ: %.2f C (ADC Temp: %" PRId32 ")", current_temp, temp_filtered);

        // Cập nhật giá trị pH và Nhiệt độ thực tế lên màn hình hiển thị
        screen_update_values(0.0f, current_temp);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
