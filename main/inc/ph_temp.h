#ifndef PH_TEMP_H
#define PH_TEMP_H

#include <stdbool.h>
#include <stdint.h>

// Hằng số dùng chung cho tính toán pH & Nhiệt độ
#define V_REF           1.25f
#define PGA             1.0f
#define ADC_SCALE       (1 << 23)   // 8388608
#define ADC_DIVISOR     (ADC_SCALE * PGA)  

#define NTC_T0_KELVIN               (25.0f + 273.15f)
#define NTC_NOMINAL_RESISTANCE      10000.0f
#define NTC_BETA                    3950.0f

// --- Cấu trúc dữ liệu Hiệu chuẩn pH 2 điểm nâng cao ---
typedef struct {
    float ph7_voltage_mv;   // Hiệu điện thế thực tế đo được tại pH 7.00 (mV)
    float ph7_temp_c;       // Nhiệt độ dung dịch đo lúc hiệu chuẩn pH 7 (C)
    float ph4_voltage_mv;   // Hiệu điện thế thực tế đo được tại pH 4.01 (mV)
    float ph4_temp_c;       // Nhiệt độ dung dịch đo lúc hiệu chuẩn pH 4 (C)
    float slope_norm;       // Độ dốc chuẩn hóa thực tế (pH * K / mV)
    float u7;               // Điểm lệch chuẩn hóa thực tế tại pH 7.00 (mV/K)
    bool is_calibrated;     // Trạng thái xác định hệ thống đã được hiệu chuẩn thành công hay chưa
} PhCalibration_t;

// Hàm cập nhật tham số hiệu chuẩn từ phép đo thực tế (Gọi khi nhúng dung dịch chuẩn thành công)
void update_ph_calibration(PhCalibration_t *cal, float v7, float t7_c, float v4, float t4_c);

// Tính toán nhiệt độ từ ADC thô (PT1000 hoặc NTC)
float calculate_temperature(int32_t raw_adc, bool is_pt1000);

// Tính toán pH bù nhiệt (ATC) kết hợp dữ liệu hiệu chuẩn thực tế 2 điểm
float calculate_ph_with_atc_calibrated(PhCalibration_t *cal, int32_t raw_adc, float temp_c, float *out_v_probe_mv);

#endif // PH_TEMP_H
