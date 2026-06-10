#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include "nvs_flash.h"

#include "cs1237.h"
#include "filter.h"
#include "ph_temp.h"
#include "screen_disp.h"
#include "user_system.h"
#include "wifi_config_manager.h"
#include "user_time.h"
#include "user_http_server.h"
#include "user_azure.h"
#include "ds3231.h"
#include <sys/time.h>

static const char *TAG = "PH_TEMP_SYSTEM";

MovingAverage_t ph_filter;
MovingAverage_t temp_filter;

TaskHandle_t Azure_Task_Handle = NULL;
TaskHandle_t Http_Server_Task_Handle = NULL;
TaskHandle_t Timer_Task_Handle = NULL;
TaskHandle_t OTA_Task_Handle = NULL;

static void system_startup_task(void *pvParameters) {
  // --- Khởi tạo Wifi Manager ---
  wifi_config_manager_init();

  // --- Tạo Http Server Task (luôn tạo để vào được Web Portal cấu hình) ---
  if (xTaskCreatePinnedToCore((TaskFunction_t)User_Http_Server_Task, "Http_Server_Task", 6144, NULL, 5, &Http_Server_Task_Handle, 0) == pdPASS) {
    ESP_LOGI(TAG, "Create Http_Server_Task successfully");
  } else {
    ESP_LOGE(TAG, "Create Http_Server_Task failed!");
  }

  // --- Đợi WiFi kết nối thành công ---
  while (!Sys_Info.isWifiConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vTaskDelay(pdMS_TO_TICKS(2000));

  // --- Tạo Time Task (Đồng bộ thời gian NTP) ---
  if (xTaskCreatePinnedToCore((TaskFunction_t)User_Time_Task, "Time_Task", 4096, NULL, 5, &Timer_Task_Handle, 1) == pdPASS) {
    ESP_LOGI(TAG, "Create Time_Task successfully");
  } else {
    ESP_LOGE(TAG, "Create Time_Task failed!");
  }

  // --- Đợi đồng bộ thời gian NTP thành công ---
  while (!Sys_Info.isTimeSync) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vTaskDelay(pdMS_TO_TICKS(2000));

  // --- Tạo Azure Task (Kết nối Cloud) ---
  if (xTaskCreatePinnedToCore((TaskFunction_t)User_Azure_Task, "Azure_Task", 6144, NULL, 5, &Azure_Task_Handle, 0) == pdPASS) {
    ESP_LOGI(TAG, "Create Azure_Task successfully");
  } else {
    ESP_LOGE(TAG, "Create Azure_Task failed!");
  }

  vTaskDelete(NULL);
}

void app_main(void) {
  init_system_gpios();

  init_moving_average(&temp_filter);
  init_moving_average(&ph_filter);

  vTaskDelay(pdMS_TO_TICKS(500));

  // Cấu hình cả hai chip: 40Hz, PGA = 1, Kênh A (0x10)
  write_cs1237_config(PH_SCLK_PIN, PH_DATA_PIN, CS1237_CFG_40HZ_PGA1_CHA);
  vTaskDelay(pdMS_TO_TICKS(50));
  write_cs1237_config(TEMP_SCLK_PIN, TEMP_DATA_PIN, CS1237_CFG_40HZ_PGA1_CHA);
  vTaskDelay(pdMS_TO_TICKS(300));
  uint8_t current_cfg = read_cs1237_config(TEMP_SCLK_PIN, TEMP_DATA_PIN);
  uint8_t current_cfg_ph = read_cs1237_config(PH_SCLK_PIN, PH_DATA_PIN);

  ESP_LOGI("CS1237_DEBUG", "Cấu hình hiện tại trong chip là: 0x%02X",
           current_cfg);
  ESP_LOGI("CS1237_DEBUG", "Cấu hình hiện tại trong chip ph la: 0x%02X",
           current_cfg_ph);

  // --- KHỞI TẠO RTC DS3231 ---
  if (ds3231_init(GPIO_NUM_3, GPIO_NUM_20) == ESP_OK) {
    bool stopped = false;
    if (ds3231_check_oscillator(&stopped) == ESP_OK && !stopped) {
      struct tm rtc_time;
      if (ds3231_get_time(&rtc_time) == ESP_OK) {
        struct timeval tv = {
          .tv_sec = mktime(&rtc_time),
          .tv_usec = 0
        };
        if (tv.tv_sec != -1) {
          settimeofday(&tv, NULL);
          setenv("TZ", "UTC-7", 1);
          tzset();
          char timeBuf[64];
          strftime(timeBuf, sizeof(timeBuf), "%c", &rtc_time);
          ESP_LOGI(TAG, "Đã đồng bộ thời gian từ RTC DS3231: %s", timeBuf);
        }
      }
    } else {
      ESP_LOGW(TAG, "RTC Oscillator bị dừng hoặc lỗi, thời gian chưa tin cậy!");
    }
  } else {
    ESP_LOGE(TAG, "Khởi tạo RTC DS3231 thất bại!");
  }

  // --- KHỞI TẠO MÀN HÌNH LCD ---
  if (LCD_Init() == LCD_OK) {
    LCD_Start_Task();
  } else {
    ESP_LOGE(TAG, "Khởi tạo LCD thất bại!");
  }

  // --- Khởi tạo NVS Flash ---
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // --- Tải cấu hình hiệu chuẩn ---
  Load_Calibration_From_Storage();

  // --- Khởi tạo User System ---
  User_System_Init();

  // --- Khởi chạy tiến trình khởi tạo mạng tuần tự ---
  xTaskCreatePinnedToCore(system_startup_task, "system_startup_task", 4096, NULL, 5, NULL, 0);

  ESP_LOGI(TAG, "Đã khởi tạo hệ thống giám sát pH & Nhiệt độ ATC hoàn chỉnh.");

  while (1) {
    // 1. KÊNH NHIỆT ĐỘ
    int32_t temp_raw = read_cs1237_raw(TEMP_SCLK_PIN, TEMP_DATA_PIN);
    int32_t temp_filtered = apply_moving_average(&temp_filter, temp_raw);
    float current_temp = calculate_temperature(temp_filtered, true); // true = PT1000

    // 2. KÊNH pH
    int32_t ph_raw = read_cs1237_raw(PH_SCLK_PIN, PH_DATA_PIN);
    int32_t ph_filtered = apply_moving_average(&ph_filter, ph_raw);
    
    // Tính v_diff cho cả raw và filtered (đổi ra mV)
    float v_diff_raw_mv = (((float)ph_raw * V_REF_ADC) / ADC_DIVISOR) * 1000.0f;
    float v_diff_filt_mv = (((float)ph_filtered * V_REF_ADC) / ADC_DIVISOR) * 1000.0f;

    float v_probe_mv = 0;
    float current_ph = calculate_ph_with_atc_calibrated(
        &ph_cal, ph_filtered, current_temp, &v_probe_mv);

    // 3. IN KẾT QUẢ
    ESP_LOGI(TAG,
             "Nhiệt độ: %.2f C | ADC Temp: %" PRId32
             " | ADC pH Raw: %" PRId32 " (v_diff: %.2f mV) | Filt: %" PRId32 " (v_diff: %.2f mV)"
             " | V_Probe: %.2f mV | pH (ATC): %.2f",
             current_temp, temp_filtered, 
             ph_raw, v_diff_raw_mv, ph_filtered, v_diff_filt_mv,
             v_probe_mv, current_ph);

    // Cập nhật giá trị pH và Nhiệt độ thực tế lên màn hình hiển thị
    screen_update_values(current_ph, current_temp);

    // Cập nhật trạng thái cảm biến toàn cục
    Update_Sensor_Measurements(current_ph, current_temp, v_probe_mv);

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
