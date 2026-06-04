#include "ph_temp.h"
#include <math.h>
#include "esp_log.h"

static const char *TAG = "PH_TEMP_ALGO";

void update_ph_calibration(PhCalibration_t *cal, float v7, float t7_c, float v4, float t4_c) {
    cal->ph7_voltage_mv = v7;
    cal->ph7_temp_c = t7_c;
    cal->ph4_voltage_mv = v4;
    cal->ph4_temp_c = t4_c;

    float t7_k = t7_c + 273.15f;
    float t4_k = t4_c + 273.15f;

    cal->u7 = v7 / t7_k;
    float u4 = v4 / t4_k;

    // Đề phòng lỗi chia cho 0 nếu người dùng cắm nhầm dung dịch đo (chênh lệch U7 - U4 tối thiểu 0.2)
    if (fabsf(cal->u7 - u4) > 0.1f) {
        cal->slope_norm = (7.00f - 4.01f) / (cal->u7 - u4);
        cal->is_calibrated = true;
        ESP_LOGI(TAG, "Hiệu chuẩn thành công! Slope_norm: %.4f, U7: %.4f", cal->slope_norm, cal->u7);
    } else {
        cal->is_calibrated = false;
        ESP_LOGE(TAG, "Lỗi hiệu chuẩn: Điện áp dung dịch chuẩn quá gần nhau hoặc cảm biến lỗi!");
    }
}

float calculate_temperature(int32_t raw_adc, bool is_pt1000) {
    float v_diff = ((float)raw_adc * V_REF) / ADC_DIVISOR;

    // Phòng ngừa lỗi chia cho 0 hoặc giá trị quá nhỏ
    if (fabsf(v_diff) < 1e-6f) {
        v_diff = 1e-6f;
    }

    if (is_pt1000) {
        float ratio = V_REF / v_diff;
        if (ratio < 1.0f) ratio = 1.0f; // Tránh điện trở âm bất thường
        float r_pt1000 = 750.0f * (ratio - 1.0f);
        return (r_pt1000 - 1000.0f) / 3.9083f;
    } else {
        float v_ntc = V_REF + v_diff;
        if (v_ntc <= 0.0f) v_ntc = 1e-6f;
        
        float ratio = V_REF / v_ntc;
        if (ratio <= 1.0001f) ratio = 1.0001f; // Tránh lấy log của số âm hoặc bằng 0
        
        float r_ntc = 750.0f / (ratio - 1.0f);
        float steinhart = r_ntc / NTC_NOMINAL_RESISTANCE;           
        steinhart = logf(steinhart);            
        steinhart /= NTC_BETA;                   
        steinhart += 1.0f / (NTC_T0_KELVIN);    
        steinhart = 1.0f / steinhart;           
        steinhart -= 273.15f;                   
        return steinhart;
    }
}

float calculate_ph_with_atc_calibrated(PhCalibration_t *cal, int32_t raw_adc, float temp_c, float *out_v_probe_mv) {
    float v_diff = ((float)raw_adc * V_REF) / ((float)ADC_SCALE * PGA);
    float v_probe = 2.0f * v_diff - 0.83333f; 
    float v_probe_mv = v_probe * 1000.0f;
    
    if (out_v_probe_mv) {
        *out_v_probe_mv = v_probe_mv;
    }

    // Đề phòng lỗi đo nhiệt độ bất thường
    if (temp_c < -40.0f || temp_c > 150.0f) temp_c = 25.0f; 

    // Nếu hệ thống chưa được hiệu chuẩn thực tế: Fallback về công thức tuyến tính lý thuyết
    if (!cal->is_calibrated) {
        float slope_at_temp = -59.16f * ((temp_c + 273.15f) / 298.15f);
        return 7.00f + (v_probe_mv / slope_at_temp);
    }

    // Thực hiện áp dụng công thức bù nhiệt ATC 2 điểm chuẩn hóa Kelvin
    float temp_k = temp_c + 273.15f;
    float u_probe = v_probe_mv / temp_k;

    float ph_val = 7.00f + (u_probe - cal->u7) * cal->slope_norm;

    // Giới hạn dải đo bảo vệ hiển thị ngoài thực tế
    if (ph_val < 0.0f) ph_val = 0.0f;
    if (ph_val > 14.0f) ph_val = 14.0f;

    return ph_val;
}
