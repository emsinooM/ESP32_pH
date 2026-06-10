#include "ph_temp.h"
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "PH_TEMP_ALGO";

// Đối tượng hiệu chuẩn toàn cục dùng chung cho hệ thống
PhCalibration_t ph_cal = {
    .ph7_voltage_mv = 24.2f,
    .ph7_temp_c = 25.0f,
    .ph4_voltage_mv = 195.6f,
    .ph4_temp_c = 25.0f,
    .slope_norm = 0.015f,
    .u7 = 0.081f,
    .is_calibrated = false
};

// Cấu trúc trạng thái cảm biến toàn cục
static PH_Temp_Sensor_Status_t s_sensor_status = {0};

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
    float v_diff = ((float)raw_adc * V_REF_ADC) / ADC_DIVISOR;

    // Phòng ngừa lỗi chia cho 0 hoặc giá trị quá nhỏ
    if (fabsf(v_diff) < 1e-6f) {
        v_diff = 1e-6f;
    }

    if (is_pt1000) {
        float ratio = V_REF_BRIDGE  / v_diff;
        if (ratio < 1.0f) ratio = 1.0f; // Tránh điện trở âm bất thường
        float r_pt1000 = R_CALIB * (ratio - 1.0f);
        return (r_pt1000 - 1000.0f) / 3.9083f;
    } else {
        float v_ntc = V_REF_BRIDGE  + v_diff;
        if (v_ntc <= 0.0f) v_ntc = 1e-6f;
        
        float ratio = V_REF_BRIDGE  / v_ntc;
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
    float v_diff = ((float)raw_adc * V_REF_ADC) / ADC_DIVISOR;
    // float v_probe =  v_diff - 0.83333f;
    float offset_v = 1.098f; // Chân AIN_P (3.3V) - Chân AIN_N (nền 2.2V) = Chênh lệch 1.1V
    float v_probe =  (v_diff - offset_v) * 2.0f; // 3. Giải mã điện áp đầu dò thực tế (Khử offset và bù hệ số suy hao 0.5 của Op-Amp)

    float v_probe_mv = v_probe * 1000.0f;
    
    if (out_v_probe_mv) {
        *out_v_probe_mv = v_probe_mv;
    }

    // Đề phòng lỗi đo nhiệt độ bất thường
    if (temp_c < -40.0f || temp_c > 150.0f) temp_c = 25.0f; 

    float ph_val;

    // Nếu hệ thống chưa được hiệu chuẩn thực tế: Fallback về công thức tuyến tính lý thuyết
    if (!cal->is_calibrated) {
        float slope_at_25 = -59.16f; // Mặc định lý thuyết ở 25C (298.15K)
        // Nếu đã có dữ liệu cấu hình hiệu chuẩn (V4 và V7 khác nhau)
        if (fabsf(cal->ph4_voltage_mv - cal->ph7_voltage_mv) > 1.0f) {
            slope_at_25 = (cal->ph4_voltage_mv - cal->ph7_voltage_mv) / (4.01f - 7.00f);
        }
        float slope_at_temp = slope_at_25 * ((temp_c + 273.15f) / 298.15f);
        ph_val = 7.00f + (v_probe_mv / slope_at_temp);
    } else {
        // Thực hiện áp dụng công thức bù nhiệt ATC 2 điểm chuẩn hóa Kelvin
        float temp_k = temp_c + 273.15f;
        float u_probe = v_probe_mv / temp_k;
        ph_val = 7.00f + (u_probe - cal->u7) * cal->slope_norm;
    }

    // Giới hạn dải đo bảo vệ hiển thị ngoài thực tế
    if (ph_val < 0.0f) ph_val = 0.0f;
    if (ph_val > 14.0f) ph_val = 14.0f;

    return ph_val;
}

// --- Các API Toàn cục mới phục vụ Web Server & Azure ---

PH_Temp_Sensor_Status_t Get_Sensor_Status(void) {
    return s_sensor_status;
}

void Update_Sensor_Measurements(float ph, float temp, float v_probe_mv) {
    s_sensor_status.ph = ph;
    s_sensor_status.temperature = temp;
    s_sensor_status.v_probe_mv = v_probe_mv;
    
    // Đồng bộ các tham số hiệu chuẩn hiện tại
    s_sensor_status.is_calibrated = ph_cal.is_calibrated;
    s_sensor_status.ph7_voltage_mv = ph_cal.ph7_voltage_mv;
    s_sensor_status.ph7_temp_c = ph_cal.ph7_temp_c;
    s_sensor_status.ph4_voltage_mv = ph_cal.ph4_voltage_mv;
    s_sensor_status.ph4_temp_c = ph_cal.ph4_temp_c;
    s_sensor_status.slope_norm = ph_cal.slope_norm;
    s_sensor_status.u7 = ph_cal.u7;
}

bool Save_Calibration_To_Storage(const PhCalibration_t *cal) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sys_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi mở NVS namespace sys_cfg để ghi: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, "ph_calib", cal, sizeof(PhCalibration_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi lưu/commit cấu hình hiệu chuẩn vào NVS: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Đã lưu cấu hình hiệu chuẩn pH vào NVS thành công.");
    return true;
}

bool Load_Calibration_From_Storage(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sys_cfg", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Không mở được NVS (Lần đầu boot?): %s. Khởi tạo giá trị mặc định.", esp_err_to_name(err));
        update_ph_calibration(&ph_cal, 24.2f, 25.0f, 195.6f, 25.0f);
        ph_cal.is_calibrated = false;
        Update_Sensor_Measurements(7.0f, 25.0f, 0.0f);
        return false;
    }

    size_t size = sizeof(PhCalibration_t);
    err = nvs_get_blob(handle, "ph_calib", &ph_cal, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(PhCalibration_t)) {
        ESP_LOGW(TAG, "Không tìm thấy dữ liệu hiệu chuẩn trong NVS, sử dụng giá trị fallback mặc định.");
        update_ph_calibration(&ph_cal, 24.2f, 25.0f, 195.6f, 25.0f);
        ph_cal.is_calibrated = false;
        Update_Sensor_Measurements(7.0f, 25.0f, 0.0f);
        return false;
    }

    ESP_LOGI(TAG, "Đã tải cấu hình hiệu chuẩn thành công từ NVS: is_calibrated=%d, pH7_mV=%.2f, pH4_mV=%.2f",
             ph_cal.is_calibrated, ph_cal.ph7_voltage_mv, ph_cal.ph4_voltage_mv);
    Update_Sensor_Measurements(7.0f, 25.0f, 0.0f);
    return true;
}

bool Calibrate_PH_7(float current_v_mv, float current_temp_c) {
    ESP_LOGI(TAG, "Yêu cầu hiệu chuẩn pH 7.00: raw mV=%.2f, Temp=%.2f C", current_v_mv, current_temp_c);
    
    PhCalibration_t temp_cal = ph_cal;
    update_ph_calibration(&temp_cal, current_v_mv, current_temp_c, temp_cal.ph4_voltage_mv, temp_cal.ph4_temp_c);
    
    if (temp_cal.is_calibrated) {
        ph_cal = temp_cal;
        Save_Calibration_To_Storage(&ph_cal);
        Update_Sensor_Measurements(s_sensor_status.ph, s_sensor_status.temperature, s_sensor_status.v_probe_mv);
        return true;
    }
    return false;
}

bool Calibrate_PH_4(float current_v_mv, float current_temp_c) {
    ESP_LOGI(TAG, "Yêu cầu hiệu chuẩn pH 4.01: raw mV=%.2f, Temp=%.2f C", current_v_mv, current_temp_c);
    
    PhCalibration_t temp_cal = ph_cal;
    update_ph_calibration(&temp_cal, temp_cal.ph7_voltage_mv, temp_cal.ph7_temp_c, current_v_mv, current_temp_c);
    
    if (temp_cal.is_calibrated) {
        ph_cal = temp_cal;
        Save_Calibration_To_Storage(&ph_cal);
        Update_Sensor_Measurements(s_sensor_status.ph, s_sensor_status.temperature, s_sensor_status.v_probe_mv);
        return true;
    }
    return false;
}
