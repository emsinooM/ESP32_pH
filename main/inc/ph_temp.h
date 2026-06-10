#ifndef PH_TEMP_H
#define PH_TEMP_H

#include <stdbool.h>
#include <stdint.h>

// Hằng số dùng chung cho tính toán pH & Nhiệt độ
// Định nghĩa lại 2 mốc điện áp riêng biệt
#define V_REF_ADC       3.302f  // <-- THAY BẰNG ÁP VDD BẠN ĐO ĐƯỢC TẠI CHÂN 7
#define V_REF_BRIDGE    3.302f  // ÁP cấp cho cầu phân áp PT1000
#define V_PH_REF_ACTUAL 2.201f  // Giá trị thực tế tại chân ph_REFM
// #define V_REF           1.25f
#define PGA             1.0f
#define ADC_SCALE       8388607.0f
#define ADC_DIVISOR     (ADC_SCALE * 2.0f * PGA)  

#define NTC_T0_KELVIN               (25.0f + 273.15f)
#define NTC_NOMINAL_RESISTANCE      10000.0f
#define NTC_BETA                    3950.0f

#define R_CALIB 725.0f

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

extern PhCalibration_t ph_cal;

// --- Cấu trúc trạng thái toàn cục của Cảm biến ---
typedef struct {
    float ph;               // Giá trị pH hiện tại
    float temperature;      // Giá trị nhiệt độ hiện tại (C)
    float v_probe_mv;       // Điện áp đo được từ đầu dò (mV)
    bool is_calibrated;     // Đã được hiệu chuẩn hay chưa
    
    // Các tham số hiệu chuẩn hiện tại
    float ph7_voltage_mv;
    float ph7_temp_c;
    float ph4_voltage_mv;
    float ph4_temp_c;
    float slope_norm;
    float u7;
} PH_Temp_Sensor_Status_t;

// Hàm cập nhật tham số hiệu chuẩn từ phép đo thực tế (Gọi khi nhúng dung dịch chuẩn thành công)
void update_ph_calibration(PhCalibration_t *cal, float v7, float t7_c, float v4, float t4_c);

// Tính toán nhiệt độ từ ADC thô (PT1000 hoặc NTC)
float calculate_temperature(int32_t raw_adc, bool is_pt1000);

// Tính toán pH bù nhiệt (ATC) kết hợp dữ liệu hiệu chuẩn thực tế 2 điểm
float calculate_ph_with_atc_calibrated(PhCalibration_t *cal, int32_t raw_adc, float temp_c, float *out_v_probe_mv);

// --- Các API Toàn cục mới phục vụ Web Server & Azure ---
PH_Temp_Sensor_Status_t Get_Sensor_Status(void);
void Update_Sensor_Measurements(float ph, float temp, float v_probe_mv);
bool Load_Calibration_From_Storage(void);
bool Save_Calibration_To_Storage(const PhCalibration_t *cal);
bool Calibrate_PH_7(float current_v_mv, float current_temp_c);
bool Calibrate_PH_4(float current_v_mv, float current_temp_c);

#endif // PH_TEMP_H
