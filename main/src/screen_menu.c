/**
 * @file  menu.c
 * @brief Triển khai hệ thống menu phân cấp cho GMG12864-06D
 *
 *  Layout màn hình 128×64 (khi hiển thị menu):
 *   y =  0..10  → Title bar (nền đen, chữ trắng) – 11 px
 *   y = 11..51  → Danh sách mục (5 hàng × 8 px)
 *   y = 52..63  → Status bar (nền đen, chữ trắng) – 12 px
 *
 *  Phím điều hướng:
 *   UP / DOWN  → chọn mục (có scroll nếu > 5 mục)
 *   ENTER      → vào mục con (hoặc thực thi mục lá)
 *   ESC        → quay lại trang cha
 */

#include "screen_menu.h"
#include "screen_disp.h"
#include "user_storage.h"
#include "ph_temp.h"
#include "filter.h"
#include "do_sensor.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "ds3231.h"

static const char *TAG_MENU = "MENU";

sys_lang_t g_sys_lang = LANG_EN;
date_format_t g_date_format = DATE_FORMAT_YYYY_MM_DD;
display_mode_t g_display_mode = DISP_MODE_PH;

/* =====================================================================
 * Cài đặt thời gian (Time Settings)
 * ===================================================================== */
typedef struct {
    int16_t year;
    int8_t  month;
    int8_t  day;
    int8_t  hour;
    int8_t  minute;
    int8_t  second;
    uint8_t active_field; // 0 = Year, 1 = Month, 2 = Day, 3 = Hour, 4 = Minute, 5 = Second
} time_edit_state_t;

static time_edit_state_t s_time_edit;

typedef struct {
    uint8_t cal_type;     // 2 or 3
    uint8_t group_idx;    // 1 or 2
    uint8_t point_idx;    // 0, 1, 2
    float   target_ph;    // 4.00, 6.86, 7.00, 9.18, 10.00
    float   min_mv;
    float   max_mv;
} cal_exec_state_t;

static cal_exec_state_t s_cal_exec;

/* =====================================================================
 * Hiệu chuẩn DO (DO Calibration)
 * ===================================================================== */
typedef struct {
    uint8_t do_cal_type;  // 0 = Zero, 1 = Slope
} do_cal_exec_state_t;

static do_cal_exec_state_t s_do_cal_exec;

typedef struct {
    float ref_temp;
} do_temp_edit_state_t;

static do_temp_edit_state_t s_do_temp_edit;

static float s_manual_temp_edit = 25.0f;
static float s_temp_alpha_edit = 0.0f;
static float s_temp_offset_edit = 0.0f;

static uint8_t s_modbus_edit_port = 1; // 1 = Port 1, 2 = Port 2
static uint8_t s_modbus_addr_edit = 1;
static uint8_t s_contrast_edit = 5;
static uint8_t s_res_ratio_edit = 0;
static const uint32_t s_baud_rates[] = { 2400, 4800, 9600, 19200, 38400, 57600, 115200 };

static uint8_t get_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2) {
        // Kiểm tra năm nhuận
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            return 29;
        }
        return 28;
    }
    if (month >= 1 && month <= 12) {
        return days[month - 1];
    }
    return 31;
}

/* =====================================================================
 * Trạng thái menu (định nghĩa biến toàn cục)
 * ===================================================================== */
menu_state_t g_menu = {
    .current_page   = PAGE_MEASUREMENT,
    .selected       = 0,
    .scroll_offset  = 0,
    .in_menu        = false,
};

/* =====================================================================
 * Cấu trúc mô tả một trang menu
 * ===================================================================== */
#define MAX_ITEMS  8   /**< Số mục tối đa trong 1 trang */

typedef struct {
    const char    *title;                   /**< Tiêu đề trang                */
    uint8_t        item_count;              /**< Số lượng mục                  */
    const char    *items[MAX_ITEMS];        /**< Nhãn các mục                  */
    menu_page_t    children[MAX_ITEMS];     /**< Trang con (PAGE_LEAF nếu lá)  */
    menu_page_t    parent;                  /**< Trang cha                     */
} page_def_t;

/* =====================================================================
 * Cây menu toàn bộ
 * ===================================================================== */
static const page_def_t s_pages_en[PAGE_COUNT] = {

    /* ── Main Menu ── */
    [PAGE_MAIN_MENU] = {
        .title      = "Main Menu",
        .item_count = 3,
        .items      = { "1 System Settings",
                        "2 Sensor Settings",
                        "3 Modbus Settings" },
        .children   = { PAGE_SYSTEM_SETTINGS,
                        PAGE_SENSOR_SETTINGS,
                        PAGE_MODBUS_SETTINGS },
        .parent     = PAGE_MEASUREMENT,
    },

    /* ── System Settings ── */
    [PAGE_SYSTEM_SETTINGS] = {
        .title      = "System Settings",
        .item_count = 3,
        .items      = { "1.1 Language",
                        "1.2 Date",
                        "1.3 Screen Settings" },
        .children   = { PAGE_LANGUAGE, PAGE_DATE, PAGE_SCREEN_SETTINGS },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_SCREEN_SETTINGS] = {
        .title      = "Screen Settings",
        .item_count = 2,
        .items      = { "1.3.1 Contrast",
                        "1.3.2 Resistor Ratio" },
        .children   = { PAGE_SCREEN_CONTRAST, PAGE_SCREEN_RES_RATIO },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_LANGUAGE] = {
        .title      = "Language",
        .item_count = 2,
        .items      = { "1.1.1 ENGLISH",
                        "1.1.2 VIETNAMESE" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_DATE] = {
        .title      = "Date",
        .item_count = 2,
        .items      = { "1.2.1 Day Format",
                        "1.2.2 Time Settings" },
        .children   = { PAGE_DATE_FORMAT, PAGE_TIME_SETTINGS },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_DATE_FORMAT] = {
        .title      = "Day Format",
        .item_count = 3,
        .items      = { "1.2.1.1 YYYY-MM-DD",
                        "1.2.1.2 DD-MM-YYYY",
                        "1.2.1.3 MM-DD-YYYY" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_DATE,
    },

    /* ── Sensor Settings ── */
    [PAGE_SENSOR_SETTINGS] = {
        .title      = "Sensor Settings",
        .item_count = 6,
        .items      = { "2.1 Display Mode",
                        "2.2 Calibration",
                        "2.3 Digital Filter",
                        "2.4 Temp Mode",
                        "2.5 Temp Settings",
                        "2.6 Temp Lin COMP" },
        .children   = { PAGE_DISPLAY_MODE,
                        PAGE_CALIBRATION,
                        PAGE_DIGITAL_FILTER,
                        PAGE_TEMP_MODE,
                        PAGE_TEMP_SETTINGS,
                        PAGE_TEMP_LIN_COMP },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_DISPLAY_MODE] = {
        .title      = "Display Mode",
        .item_count = 3,
        .items      = { "2.1.1 pH Mode",
                        "2.1.2 DO Mode",
                        "2.1.3 Dual Mode" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_CALIBRATION] = {
        .title      = "Calibration",
        .item_count = 3,
        .items      = { "2.2.1 Cal. 2 point",
                        "2.2.2 Cal. 3 point",
                        "2.2.3 Cal. DO" },
        .children   = { PAGE_CAL_2PT, PAGE_CAL_3PT, PAGE_CAL_DO },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_CAL_DO] = {
        .title      = "DO Calibration",
        .item_count = 4,
        .items      = { "1. Cal. DO Zero",
                        "2. Cal. DO Slope",
                        "3. Cal. DO Temp",
                        "4. Reset Sensor" },
        .children   = { PAGE_CAL_DO_EXEC, PAGE_CAL_DO_EXEC, PAGE_CAL_DO_TEMP, PAGE_LEAF },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_2PT] = {
        .title      = "Cal. 2 point",
        .item_count = 2,
        .items      = { "1. Low Cal. 4.00",
                        "2. High Cal. 7.00" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_3PT] = {
        .title      = "Cal. 3 point",
        .item_count = 2,
        .items      = { "1. Group 1(4/6/9)",
                        "2. Group 2(4/7/10)" },
        .children   = { PAGE_CAL_3PT_G1, PAGE_CAL_3PT_G2 },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_3PT_G1] = {
        .title      = "Group 1(4/6/9)",
        .item_count = 3,
        .items      = { "1. Low Cal. 4.00",
                        "2. Mid Cal. 6.86",
                        "3. High Cal. 9.18" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CAL_3PT,
    },

    [PAGE_CAL_3PT_G2] = {
        .title      = "Group 2(4/7/10)",
        .item_count = 3,
        .items      = { "1. Low Cal. 4.00",
                        "2. Mid Cal. 7.00",
                        "3. High Cal. 10.00" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CAL_3PT,
    },

    [PAGE_DIGITAL_FILTER] = {
        .title      = "Digital Filter",
        .item_count = 3,
        .items      = { "2.3.1 L",
                        "2.3.2 M",
                        "2.3.3 H" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_MODE] = {
        .title      = "Temp Mode",
        .item_count = 4,
        .items      = { "2.4.1 ATC  C",
                        "2.4.2 MTC  C",
                        "2.4.3 ATF  F",
                        "2.4.4 MTF  F" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_SETTINGS] = {
        .title      = "Temp Settings",
        .item_count = 0,
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_LIN_COMP] = {
        .title      = "Temp Lin COMP",
        .item_count = 0,
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    /* ── Output Settings ── */
    [PAGE_MODBUS_SETTINGS] = {
        .title      = "Modbus Settings",
        .item_count = 2,
        .items      = { "3.1 Modbus Port 1 (Ext)",
                        "3.2 Modbus Port 2 (DO)" },
        .children   = { PAGE_MODBUS_PORT1,
                        PAGE_MODBUS_PORT2 },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_MODBUS_PORT1] = {
        .title      = "Modbus Port 1",
        .item_count = 4,
        .items      = { "3.1.1 MB Address",
                        "3.1.2 Baud Rate",
                        "3.1.3 Parity Check",
                        "3.1.4 Stop Bits" },
        .children   = { PAGE_MODBUS_EDIT_ADDR,
                        PAGE_MODBUS_SELECT_BAUD,
                        PAGE_MODBUS_SELECT_PARITY,
                        PAGE_MODBUS_SELECT_STOP },
        .parent     = PAGE_MODBUS_SETTINGS,
    },

    [PAGE_MODBUS_PORT2] = {
        .title      = "Modbus Port 2",
        .item_count = 4,
        .items      = { "3.2.1 MB Address",
                        "3.2.2 Baud Rate",
                        "3.2.3 Parity Check",
                        "3.2.4 Stop Bits" },
        .children   = { PAGE_MODBUS_EDIT_ADDR,
                        PAGE_MODBUS_SELECT_BAUD,
                        PAGE_MODBUS_SELECT_PARITY,
                        PAGE_MODBUS_SELECT_STOP },
        .parent     = PAGE_MODBUS_SETTINGS,
    },

    [PAGE_MODBUS_SELECT_BAUD] = {
        .title      = "Baud Rate",
        .item_count = 7,
        .items      = { "2400", "4800", "9600", "19200", "38400", "57600", "115200" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },

    [PAGE_MODBUS_SELECT_PARITY] = {
        .title      = "Parity Check",
        .item_count = 3,
        .items      = { "None", "Even", "Odd" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },

    [PAGE_MODBUS_SELECT_STOP] = {
        .title      = "Stop Bits",
        .item_count = 2,
        .items      = { "1 Stop Bit", "2 Stop Bits" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },
};

static const page_def_t s_pages_vi[PAGE_COUNT] = {

    /* ── Main Menu ── */
    [PAGE_MAIN_MENU] = {
        .title      = "Menu Chinh",
        .item_count = 3,
        .items      = { "1 Cai Dat He Thong",
                        "2 Cai Dat Cam Bien",
                        "3 Cai Dat Modbus" },
        .children   = { PAGE_SYSTEM_SETTINGS,
                        PAGE_SENSOR_SETTINGS,
                        PAGE_MODBUS_SETTINGS },
        .parent     = PAGE_MEASUREMENT,
    },

    /* ── System Settings ── */
    [PAGE_SYSTEM_SETTINGS] = {
        .title      = "Cai Dat He Thong",
        .item_count = 3,
        .items      = { "1.1 Ngon Ngu",
                        "1.2 Ngay Thang",
                        "1.3 Cai Dat Man Hinh" },
        .children   = { PAGE_LANGUAGE, PAGE_DATE, PAGE_SCREEN_SETTINGS },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_SCREEN_SETTINGS] = {
        .title      = "Cai Dat Man Hinh",
        .item_count = 2,
        .items      = { "1.3.1 Tuong Phan",
                        "1.3.2 Ty So Dien Tro" },
        .children   = { PAGE_SCREEN_CONTRAST, PAGE_SCREEN_RES_RATIO },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_LANGUAGE] = {
        .title      = "Ngon Ngu",
        .item_count = 2,
        .items      = { "1.1.1 TIENG ANH",
                        "1.1.2 TIENG VIET" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_DATE] = {
        .title      = "Ngay Thang",
        .item_count = 2,
        .items      = { "1.2.1 Dinh Dang Ngay",
                        "1.2.2 Cai Dat Gio" },
        .children   = { PAGE_DATE_FORMAT, PAGE_TIME_SETTINGS },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_DATE_FORMAT] = {
        .title      = "Dinh Dang Ngay",
        .item_count = 3,
        .items      = { "1.2.1.1 YYYY-MM-DD",
                        "1.2.1.2 DD-MM-YYYY",
                        "1.2.1.3 MM-DD-YYYY" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_DATE,
    },

    /* ── Sensor Settings ── */
    [PAGE_SENSOR_SETTINGS] = {
        .title      = "Cai Dat Cam Bien",
        .item_count = 6,
        .items      = { "2.1 Che Do Hien Thi",
                        "2.2 Hieu Chuan",
                        "2.3 Bo Loc So",
                        "2.4 Che Do Nhiet Do",
                        "2.5 Cai Dat Nhiet Do",
                        "2.6 Bu Tuyen Tinh T" },
        .children   = { PAGE_DISPLAY_MODE,
                        PAGE_CALIBRATION,
                        PAGE_DIGITAL_FILTER,
                        PAGE_TEMP_MODE,
                        PAGE_TEMP_SETTINGS,
                        PAGE_TEMP_LIN_COMP },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_DISPLAY_MODE] = {
        .title      = "Che Do Hien Thi",
        .item_count = 3,
        .items      = { "2.1.1 Che Do pH",
                        "2.1.2 Che Do DO",
                        "2.1.3 Che Do Song Song" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_CALIBRATION] = {
        .title      = "Hieu Chuan",
        .item_count = 3,
        .items      = { "2.2.1 Hieu Chuan 2D",
                        "2.2.2 Hieu Chuan 3D",
                        "2.2.3 Hieu Chuan DO" },
        .children   = { PAGE_CAL_2PT, PAGE_CAL_3PT, PAGE_CAL_DO },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_CAL_DO] = {
        .title      = "Hieu Chuan DO",
        .item_count = 4,
        .items      = { "1. Hieu Chuan Diem 0",
                        "2. Hieu Chuan Do Doc",
                        "3. Hieu Chinh Nhiet Do",
                        "4. Reset Cam Bien" },
        .children   = { PAGE_CAL_DO_EXEC, PAGE_CAL_DO_EXEC, PAGE_CAL_DO_TEMP, PAGE_LEAF },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_2PT] = {
        .title      = "Hieu Chuan 2D",
        .item_count = 2,
        .items      = { "1. Cal. Thap 4.00",
                        "2. Cal. Cao 7.00" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_3PT] = {
        .title      = "Hieu Chuan 3D",
        .item_count = 2,
        .items      = { "1. Nhom 1(4/6/9)",
                        "2. Nhom 2(4/7/10)" },
        .children   = { PAGE_CAL_3PT_G1, PAGE_CAL_3PT_G2 },
        .parent     = PAGE_CALIBRATION,
    },

    [PAGE_CAL_3PT_G1] = {
        .title      = "Nhom 1(4/6/9)",
        .item_count = 3,
        .items      = { "1. Cal. Thap 4.00",
                        "2. Cal. Trung 6.86",
                        "3. Cal. Cao 9.18" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CAL_3PT,
    },

    [PAGE_CAL_3PT_G2] = {
        .title      = "Nhom 2(4/7/10)",
        .item_count = 3,
        .items      = { "1. Cal. Thap 4.00",
                        "2. Cal. Trung 7.00",
                        "3. Cal. Cao 10.00" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_CAL_3PT,
    },

    [PAGE_DIGITAL_FILTER] = {
        .title      = "Bo Loc So",
        .item_count = 3,
        .items      = { "2.3.1 Thap",
                        "2.3.2 Vua",
                        "2.3.3 Cao" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_MODE] = {
        .title      = "Che Do Nhiet Do",
        .item_count = 4,
        .items      = { "2.4.1 ATC  C",
                        "2.4.2 MTC  C",
                        "2.4.3 ATF  F",
                        "2.4.4 MTF  F" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_SETTINGS] = {
        .title      = "Cai Dat Nhiet Do",
        .item_count = 0,
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    [PAGE_TEMP_LIN_COMP] = {
        .title      = "Bu Tuyen Tinh T",
        .item_count = 0,
        .parent     = PAGE_SENSOR_SETTINGS,
    },

    /* ── Output Settings ── */
    [PAGE_MODBUS_SETTINGS] = {
        .title      = "Cai Dat Modbus",
        .item_count = 2,
        .items      = { "3.1 Cong Modbus 1 (MR)",
                        "3.2 Cong Modbus 2 (DO)" },
        .children   = { PAGE_MODBUS_PORT1,
                        PAGE_MODBUS_PORT2 },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_MODBUS_PORT1] = {
        .title      = "Cong Modbus 1",
        .item_count = 4,
        .items      = { "3.1.1 Dia Chi MB",
                        "3.1.2 Toc Do Baud",
                        "3.1.3 Kiem Tra Parity",
                        "3.1.4 Bit Stop" },
        .children   = { PAGE_MODBUS_EDIT_ADDR,
                        PAGE_MODBUS_SELECT_BAUD,
                        PAGE_MODBUS_SELECT_PARITY,
                        PAGE_MODBUS_SELECT_STOP },
        .parent     = PAGE_MODBUS_SETTINGS,
    },

    [PAGE_MODBUS_PORT2] = {
        .title      = "Cong Modbus 2",
        .item_count = 4,
        .items      = { "3.2.1 Dia Chi MB",
                        "3.2.2 Toc Do Baud",
                        "3.2.3 Kiem Tra Parity",
                        "3.2.4 Bit Stop" },
        .children   = { PAGE_MODBUS_EDIT_ADDR,
                        PAGE_MODBUS_SELECT_BAUD,
                        PAGE_MODBUS_SELECT_PARITY,
                        PAGE_MODBUS_SELECT_STOP },
        .parent     = PAGE_MODBUS_SETTINGS,
    },

    [PAGE_MODBUS_SELECT_BAUD] = {
        .title      = "Toc Do Baud",
        .item_count = 7,
        .items      = { "2400", "4800", "9600", "19200", "38400", "57600", "115200" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },

    [PAGE_MODBUS_SELECT_PARITY] = {
        .title      = "Kiem Tra Parity",
        .item_count = 3,
        .items      = { "Khong", "Chan", "Le" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },

    [PAGE_MODBUS_SELECT_STOP] = {
        .title      = "Bit Stop",
        .item_count = 2,
        .items      = { "1 Bit Stop", "2 Bit Stop" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_MODBUS_PORT1,
    },
};

/* =====================================================================
 * Điều hướng nội bộ
 * ===================================================================== */
#define VISIBLE_ITEMS   5    /**< Số hàng mục hiển thị cùng lúc */
#define ITEM_ROW_H      8    /**< Chiều cao mỗi hàng (px)        */
#define TITLE_BAR_H     10   /**< Chiều cao title bar (px)       */
#define STATUS_BAR_H    12   /**< Chiều cao status bar (px)      */
#define ITEMS_Y_START   11   /**< y đầu tiên của danh sách       */
#define STATUS_BAR_Y    52   /**< y bắt đầu status bar           */

static void goto_page(menu_page_t page)
{
    g_menu.current_page  = page;
    g_menu.selected      = 0;
    g_menu.scroll_offset = 0;
    // bool s_waiting_for_enter_release = true; // Chờ người dùng nhả nút ENTER từ thao tác chuyển trang
    ESP_LOGI(TAG_MENU, "Navigate -> page %d", (int)page);

    if (page == PAGE_TIME_SETTINGS) {
        time_t now;
        struct tm timeInfo;
        time(&now);
        localtime_r(&now, &timeInfo);

        s_time_edit.year = timeInfo.tm_year + 1900;
        s_time_edit.month = timeInfo.tm_mon + 1;
        s_time_edit.day = timeInfo.tm_mday;
        s_time_edit.hour = timeInfo.tm_hour;
        s_time_edit.minute = timeInfo.tm_min;
        s_time_edit.second = timeInfo.tm_sec;
        s_time_edit.active_field = 0; // Bắt đầu ở Năm
        ESP_LOGI(TAG_MENU, "Cai dat thoi gian khoi tao: %04d-%02d-%02d %02d:%02d:%02d",
                 s_time_edit.year, s_time_edit.month, s_time_edit.day,
                 s_time_edit.hour, s_time_edit.minute, s_time_edit.second);
    } else if (page == PAGE_TEMP_SETTINGS) {
        bool is_f = (g_temp_mode == TEMP_MODE_ATC_F || g_temp_mode == TEMP_MODE_MTC_F);
        if (g_temp_mode == TEMP_MODE_MTC_C || g_temp_mode == TEMP_MODE_MTC_F) {
            if (is_f) {
                s_manual_temp_edit = g_manual_temp * 1.8f + 32.0f; // Đổi sang Fahrenheit
            } else {
                s_manual_temp_edit = g_manual_temp;
            }
        } else { // Chế độ ATC (bù nhiệt tự động) -> chỉnh offset
            if (is_f) {
                s_temp_offset_edit = g_temp_offset * 1.8f; // Đổi độ lệch sang Fahrenheit (delta_F = delta_C * 1.8)
            } else {
                s_temp_offset_edit = g_temp_offset;
            }
        }
    } else if (page == PAGE_TEMP_LIN_COMP) {
        s_temp_alpha_edit = g_temp_alpha;
    } else if (page == PAGE_MODBUS_EDIT_ADDR) {
        s_modbus_addr_edit = (s_modbus_edit_port == 1) ? g_mb1_addr : g_mb2_addr;
    } else if (page == PAGE_SCREEN_CONTRAST) {
        s_contrast_edit = g_lcd_contrast;
    } else if (page == PAGE_SCREEN_RES_RATIO) {
        s_res_ratio_edit = g_lcd_resistor_ratio;
    }
}

/* =====================================================================
 * Debounce nút bấm (edge-detect, polling 50 ms)
 * ===================================================================== */
typedef enum {
    BTN_IDX_ESC   = 0,
    BTN_IDX_DOWN  = 1,
    BTN_IDX_UP    = 2,
    BTN_IDX_RIGHT = 3,
    BTN_IDX_ENTER = 4,
    BTN_COUNT
} btn_idx_t;

static const uint8_t s_btn_gpios[BTN_COUNT] = {
    BTN_PIN_ESC,
    BTN_PIN_DOWN,
    BTN_PIN_UP,
    BTN_PIN_RIGHT,
    BTN_PIN_ENTER,
};

static uint8_t s_btn_debounce_counter[BTN_COUNT] = {0};
static bool    s_btn_state[BTN_COUNT] = {false};
static bool    s_btn_prev_state[BTN_COUNT] = {false};
static bool    s_waiting_for_enter_release = false;

/**
 * @brief  Đọc trạng thái GPIO và chống rung (debounce) cho các nút bấm
 *         Yêu cầu trạng thái ổn định trong 2 chu kỳ quét liên tiếp (100 ms)
 */
static void poll_and_debounce_buttons(void)
{
    // Nếu đang chờ nhả nút ENTER, kiểm tra xem nút ENTER đã nhả (về 0) chưa
    if (s_waiting_for_enter_release) {
        if (gpio_get_level(BTN_PIN_ENTER) == 0) {
            s_waiting_for_enter_release = false;
        }
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        // Lưu trạng thái trước đó của chu kỳ trước
        s_btn_prev_state[i] = s_btn_state[i];

        // Đọc trạng thái vật lý (1 = Đang nhấn/HIGH, 0 = Thả/LOW)
        bool raw = (gpio_get_level(s_btn_gpios[i]) == 1);

        if (raw != s_btn_state[i]) {
            s_btn_debounce_counter[i]++;
            if (s_btn_debounce_counter[i] >= 2) {
                s_btn_state[i] = raw;
                s_btn_debounce_counter[i] = 0;
            }
        } else {
            s_btn_debounce_counter[i] = 0;
        }
    }
}

static volatile bool s_btn_sim_event[BTN_COUNT] = {false};

/**
 * @brief  Trả về true nếu nút vừa được nhấn (phát hiện sườn lên đã chống rung hoặc sự kiện mô phỏng từ xa)
 */
static bool btn_edge(btn_idx_t idx)
{
    if (idx == BTN_IDX_ENTER && s_waiting_for_enter_release) {
        return false; // Bỏ qua sự kiện ENTER cho đến khi nút được nhả ra hoàn toàn
    }
    if (s_btn_sim_event[idx]) {
        s_btn_sim_event[idx] = false; // Tiêu thụ sự kiện mô phỏng từ xa
        return true;
    }
    return s_btn_state[idx] && !s_btn_prev_state[idx];
}

bool menu_simulate_press(const char *btn_name)
{
    btn_idx_t idx = BTN_COUNT;
    if (strcmp(btn_name, "esc") == 0)        idx = BTN_IDX_ESC;
    else if (strcmp(btn_name, "down") == 0)  idx = BTN_IDX_DOWN;
    else if (strcmp(btn_name, "up") == 0)    idx = BTN_IDX_UP;
    else if (strcmp(btn_name, "right") == 0) idx = BTN_IDX_RIGHT;
    else if (strcmp(btn_name, "enter") == 0) idx = BTN_IDX_ENTER;

    if (idx < BTN_COUNT) {
        s_btn_sim_event[idx] = true;
        return true;
    }
    return false;
}

static void menu_render_time_settings(void)
{
    LCD_Clear();
    
    // 1. Title bar (y=0..9): nền đen, chữ trắng
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(4, 1, "Cai Dat Thoi Gian", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(4, 1, "Time Settings", LCD_COLOR_OFF);
    }

    // 2. Nội dung chỉnh sửa (y=11..50)
    char date_buf[32];
    char time_buf[32];
    
    snprintf(date_buf, sizeof(date_buf), "%04d - %02d - %02d", s_time_edit.year, s_time_edit.month, s_time_edit.day);
    snprintf(time_buf, sizeof(time_buf), "%02d : %02d : %02d", s_time_edit.hour, s_time_edit.minute, s_time_edit.second);
    
    // Vẽ chuỗi ngày tháng ở hàng trên
    LCD_DrawString(16, 18, date_buf, LCD_COLOR_ON);
    // Vẽ chuỗi giờ phút ở hàng dưới
    LCD_DrawString(16, 30, time_buf, LCD_COLOR_ON);
    
    // Vẽ gạch chân dưới trường đang chỉnh sửa
    uint8_t line_x = 0;
    uint8_t line_y = 0;
    uint8_t line_w = 12;
    
    if (s_time_edit.active_field == 0) { // Year
        line_x = 16;
        line_y = 26;
        line_w = 24;
    } else if (s_time_edit.active_field == 1) { // Month
        line_x = 58;
        line_y = 26;
        line_w = 12;
    } else if (s_time_edit.active_field == 2) { // Day
        line_x = 88;
        line_y = 26;
        line_w = 12;
    } else if (s_time_edit.active_field == 3) { // Hour
        line_x = 16;
        line_y = 38;
        line_w = 12;
    } else if (s_time_edit.active_field == 4) { // Minute
        line_x = 46;
        line_y = 38;
        line_w = 12;
    } else if (s_time_edit.active_field == 5) { // Second
        line_x = 76;
        line_y = 38;
        line_w = 12;
    }
    
    // Vẽ đường gạch chân dày 2 pixel
    LCD_DrawHLine(line_x, line_y, line_w, LCD_COLOR_ON);
    LCD_DrawHLine(line_x, line_y + 1, line_w, LCD_COLOR_ON);

    // 3. Status bar (y=52..63): nền đen, nhãn nút
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "TIEP", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "NEXT", LCD_COLOR_OFF);
    }
    
    LCD_Flush();
}

static void menu_handle_time_settings_buttons(void)
{
    // ESC - Hủy bỏ chỉnh sửa, quay lại trang cha (PAGE_DATE)
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy cai dat thoi gian");
        goto_page(PAGE_DATE);
        g_lcd_need_redraw = true;
        return;
    }
    
    // UP - Tăng giá trị của trường đang chọn
    if (btn_edge(BTN_IDX_UP)) {
        if (s_time_edit.active_field == 0) { // Year
            if (s_time_edit.year < 2099) s_time_edit.year++;
        } else if (s_time_edit.active_field == 1) { // Month
            if (s_time_edit.month < 12) s_time_edit.month++;
            else s_time_edit.month = 1;
        } else if (s_time_edit.active_field == 2) { // Day
            uint8_t max_day = get_days_in_month(s_time_edit.year, s_time_edit.month);
            if (s_time_edit.day < max_day) s_time_edit.day++;
            else s_time_edit.day = 1;
        } else if (s_time_edit.active_field == 3) { // Hour
            if (s_time_edit.hour < 23) s_time_edit.hour++;
            else s_time_edit.hour = 0;
        } else if (s_time_edit.active_field == 4) { // Minute
            if (s_time_edit.minute < 59) s_time_edit.minute++;
            else s_time_edit.minute = 0;
        } else if (s_time_edit.active_field == 5) { // Second
            if (s_time_edit.second < 59) s_time_edit.second++;
            else s_time_edit.second = 0;
        }
        
        // Ràng buộc lại số ngày nếu chỉnh tháng/năm làm ngày hiện tại vượt quá tối đa
        uint8_t max_day = get_days_in_month(s_time_edit.year, s_time_edit.month);
        if (s_time_edit.day > max_day) s_time_edit.day = max_day;
        
        g_lcd_need_redraw = true;
        ESP_LOGI(TAG_MENU, "[UP] Chinh thoi gian: active_field=%d, value=%d", s_time_edit.active_field, 
                 (s_time_edit.active_field == 0) ? s_time_edit.year :
                 (s_time_edit.active_field == 1) ? s_time_edit.month :
                 (s_time_edit.active_field == 2) ? s_time_edit.day :
                 (s_time_edit.active_field == 3) ? s_time_edit.hour :
                 (s_time_edit.active_field == 4) ? s_time_edit.minute : s_time_edit.second);
    }
    
    // DOWN - Giảm giá trị của trường đang chọn
    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_time_edit.active_field == 0) { // Year
            if (s_time_edit.year > 2000) s_time_edit.year--;
        } else if (s_time_edit.active_field == 1) { // Month
            if (s_time_edit.month > 1) s_time_edit.month--;
            else s_time_edit.month = 12;
        } else if (s_time_edit.active_field == 2) { // Day
            uint8_t max_day = get_days_in_month(s_time_edit.year, s_time_edit.month);
            if (s_time_edit.day > 1) s_time_edit.day--;
            else s_time_edit.day = max_day;
        } else if (s_time_edit.active_field == 3) { // Hour
            if (s_time_edit.hour > 0) s_time_edit.hour--;
            else s_time_edit.hour = 23;
        } else if (s_time_edit.active_field == 4) { // Minute
            if (s_time_edit.minute > 0) s_time_edit.minute--;
            else s_time_edit.minute = 59;
        } else if (s_time_edit.active_field == 5) { // Second
            if (s_time_edit.second > 0) s_time_edit.second--;
            else s_time_edit.second = 59;
        }
        
        // Ràng buộc lại số ngày nếu chỉnh tháng/năm làm ngày hiện tại vượt quá tối đa
        uint8_t max_day = get_days_in_month(s_time_edit.year, s_time_edit.month);
        if (s_time_edit.day > max_day) s_time_edit.day = max_day;
        
        g_lcd_need_redraw = true;
        ESP_LOGI(TAG_MENU, "[DOWN] Chinh thoi gian: active_field=%d, value=%d", s_time_edit.active_field, 
                 (s_time_edit.active_field == 0) ? s_time_edit.year :
                 (s_time_edit.active_field == 1) ? s_time_edit.month :
                 (s_time_edit.active_field == 2) ? s_time_edit.day :
                 (s_time_edit.active_field == 3) ? s_time_edit.hour :
                 (s_time_edit.active_field == 4) ? s_time_edit.minute : s_time_edit.second);
    }
    
    // ENTER - Chuyển sang trường tiếp theo, hoặc Lưu và thoát nếu ở trường Giây
    if (btn_edge(BTN_IDX_ENTER)) {
        if (s_time_edit.active_field < 5) {
            s_time_edit.active_field++;
            g_lcd_need_redraw = true;
            ESP_LOGI(TAG_MENU, "[ENTER] Chuyen sang truong index %d", s_time_edit.active_field);
        } else {
            // Lưu thời gian vào RTC DS3231 và System time
            struct tm tm_info = {
                .tm_year = s_time_edit.year - 1900,
                .tm_mon = s_time_edit.month - 1,
                .tm_mday = s_time_edit.day,
                .tm_hour = s_time_edit.hour,
                .tm_min = s_time_edit.minute,
                .tm_sec = s_time_edit.second,
                .tm_isdst = -1
            };
            
            // 1. Ghi vào DS3231 RTC
            esp_err_t err = ds3231_set_time(&tm_info);
            if (err == ESP_OK) {
                ESP_LOGI(TAG_MENU, "Da luu thoi gian vao DS3231 RTC!");
            } else {
                ESP_LOGE(TAG_MENU, "Luu thoi gian vao RTC that bai: %s", esp_err_to_name(err));
            }
            
            // 2. Ghi vào System Time của ESP32
            time_t t = mktime(&tm_info);
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            
            ESP_LOGI(TAG_MENU, "Da cap nhat thoi gian he thong: %04d-%02d-%02d %02d:%02d:%02d",
                     s_time_edit.year, s_time_edit.month, s_time_edit.day,
                     s_time_edit.hour, s_time_edit.minute, s_time_edit.second);
                     
            // Quay lại trang cha
            goto_page(PAGE_DATE);
            g_lcd_need_redraw = true;
        }
    }
}

static void menu_show_alert_dialog(const char *title, const char *msg, bool success)
{
    // Clear the box area
    LCD_FillRect(12, 14, 104, 36, LCD_COLOR_OFF);
    // Draw outer border
    LCD_DrawRect(12, 14, 104, 36, LCD_COLOR_ON);
    // Draw inner border (double border)
    LCD_DrawRect(14, 16, 100, 32, LCD_COLOR_ON);
    
    // Header bar (inverted)
    LCD_FillRect(15, 17, 98, 9, LCD_COLOR_ON);
    // Center title
    int title_len = strlen(title);
    int title_x = 12 + (104 - title_len * 6) / 2;
    LCD_DrawString(title_x, 18, title, LCD_COLOR_OFF);
    
    // Center message
    int msg_len = strlen(msg);
    int msg_x = 12 + (104 - msg_len * 6) / 2;
    LCD_DrawString(msg_x, 34, msg, LCD_COLOR_ON);
    
    LCD_Flush();
    vTaskDelay(pdMS_TO_TICKS(1500)); // Show for 1.5 seconds
}

static void menu_render_cal_do_exec(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (s_do_cal_exec.do_cal_type == 0) {
        if (g_sys_lang == LANG_VI) {
            snprintf(title_buf, sizeof(title_buf), "Hieu Chuan DO Diem 0");
        } else {
            snprintf(title_buf, sizeof(title_buf), "Cal. DO Zero");
        }
    } else {
        if (g_sys_lang == LANG_VI) {
            snprintf(title_buf, sizeof(title_buf), "Hieu Chuan DO Do Doc");
        } else {
            snprintf(title_buf, sizeof(title_buf), "Cal. DO Slope");
        }
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Main target / live readings in center
    PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
    char val_str[32];
    
    // Display DO value in large font
    snprintf(val_str, sizeof(val_str), "%.2f mg/L", status.do_mg_l);
    
    uint8_t w = strlen(val_str) * 6 * 2; 
    uint8_t x = (LCD_WIDTH - w) / 2;
    LCD_DrawStringScaled(x, 16, val_str, 2, 2, LCD_COLOR_ON);

    // 3. Live temp & saturation
    char extra_str[64];
    snprintf(extra_str, sizeof(extra_str), "T:%.1f C  Sat:%.1f%%", status.do_temp_c, status.do_saturation_pct);
    uint8_t ext_w = strlen(extra_str) * 6;
    uint8_t ext_x = (LCD_WIDTH - ext_w) / 2;
    LCD_DrawString(ext_x, 36, extra_str, LCD_COLOR_ON);

    // 4. Status bar (y=52..63)
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }
    
    LCD_Flush();
}

static void menu_render_cal_do_temp(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Hieu Chinh Nhiet Do");
    } else {
        snprintf(title_buf, sizeof(title_buf), "Cal. DO Temp");
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Live temp
    PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
    char live_str[32];
    snprintf(live_str, sizeof(live_str), "Live: %.1f C", status.do_valid ? status.do_temp_c : status.temperature);
    uint8_t live_w = strlen(live_str) * 6;
    uint8_t live_x = (LCD_WIDTH - live_w) / 2;
    LCD_DrawString(live_x, 14, live_str, LCD_COLOR_ON);

    // 3. Target/Reference temperature in center (large font, 2x2)
    char ref_str[32];
    snprintf(ref_str, sizeof(ref_str), "%.1f C", s_do_temp_edit.ref_temp);
    uint8_t ref_w = strlen(ref_str) * 6 * 2;
    uint8_t ref_x = (LCD_WIDTH - ref_w) / 2;
    LCD_DrawStringScaled(ref_x, 26, ref_str, 2, 2, LCD_COLOR_ON);
    
    // Draw an underline or indicator under the value to show it is editable
    LCD_DrawHLine(ref_x, 43, ref_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(ref_x, 44, ref_w - 6, LCD_COLOR_ON);

    // 4. Status bar (y=52..63)
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }
    
    LCD_Flush();
}

static void menu_render_temp_settings(void)
{
    LCD_Clear();

    bool is_f = (g_temp_mode == TEMP_MODE_ATC_F || g_temp_mode == TEMP_MODE_MTC_F);
    bool is_mtc = (g_temp_mode == TEMP_MODE_MTC_C || g_temp_mode == TEMP_MODE_MTC_F);

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (is_mtc) {
        if (g_sys_lang == LANG_VI) {
            snprintf(title_buf, sizeof(title_buf), "Nhiet Do Thu Cong");
        } else {
            snprintf(title_buf, sizeof(title_buf), "Manual Temp");
        }
    } else {
        if (g_sys_lang == LANG_VI) {
            snprintf(title_buf, sizeof(title_buf), "Hieu Chinh Nhiet Do");
        } else {
            snprintf(title_buf, sizeof(title_buf), "Temp Calibration");
        }
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Info / Unit (y=12)
    char info_str[32];
    if (is_mtc) {
        if (g_sys_lang == LANG_VI) {
            snprintf(info_str, sizeof(info_str), "Don vi: Do %s (MTC)", is_f ? "F" : "C");
        } else {
            snprintf(info_str, sizeof(info_str), "Unit: deg %s (MTC)", is_f ? "F" : "C");
        }
    } else {
        if (g_sys_lang == LANG_VI) {
            snprintf(info_str, sizeof(info_str), "Don vi: Do %s (ATC)", is_f ? "F" : "C");
        } else {
            snprintf(info_str, sizeof(info_str), "Unit: deg %s (ATC)", is_f ? "F" : "C");
        }
    }
    uint8_t info_w = strlen(info_str) * 6;
    uint8_t info_x = (LCD_WIDTH - info_w) / 2;
    LCD_DrawString(info_x, 12, info_str, LCD_COLOR_ON);

    // 3. Edit value in center (large font 2x2, y=22)
    char val_str[32];
    if (is_mtc) {
        snprintf(val_str, sizeof(val_str), "%.1f %s", s_manual_temp_edit, is_f ? "F" : "C");
    } else {
        snprintf(val_str, sizeof(val_str), "%s%.1f %s", (s_temp_offset_edit >= 0.0f) ? "+" : "", s_temp_offset_edit, is_f ? "F" : "C");
    }
    uint8_t val_w = strlen(val_str) * 6 * 2;
    uint8_t val_x = (LCD_WIDTH - val_w) / 2;
    LCD_DrawStringScaled(val_x, 22, val_str, 2, 2, LCD_COLOR_ON);
    
    // Draw an underline or indicator under the value to show it is editable
    LCD_DrawHLine(val_x, 39, val_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(val_x, 40, val_w - 6, LCD_COLOR_ON);

    // 4. Display final temperature in lower part (y=43, 1x1 font)
    char res_str[48];
    PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
    float final_temp = 25.0f;

    if (is_mtc) {
        final_temp = s_manual_temp_edit;
    } else {
        // Lấy nhiệt độ thực tế thô (raw = calibrated - current_offset)
        float raw_temp_c = status.temperature - g_temp_offset;
        if (is_f) {
            float offset_c = s_temp_offset_edit / 1.8f;
            float final_c = raw_temp_c + offset_c;
            final_temp = final_c * 1.8f + 32.0f;
        } else {
            final_temp = raw_temp_c + s_temp_offset_edit;
        }
    }

    if (g_sys_lang == LANG_VI) {
        snprintf(res_str, sizeof(res_str), "Nhiet do: %.1f %s", final_temp, is_f ? "Do F" : "Do C");
    } else {
        snprintf(res_str, sizeof(res_str), "Result: %.1f deg %s", final_temp, is_f ? "F" : "C");
    }
    uint8_t res_w = strlen(res_str) * 6;
    uint8_t res_x = (LCD_WIDTH - res_w) / 2;
    LCD_DrawString(res_x, 43, res_str, LCD_COLOR_ON);

    // 5. Status bar (y=52..63)
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }
    
    LCD_Flush();
}

static void menu_render_temp_lin_comp(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Bu Tuyen Tinh Nhiet");
    } else {
        snprintf(title_buf, sizeof(title_buf), "Temp Lin COMP");
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Info (y=12)
    char info_str[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(info_str, sizeof(info_str), "He so alpha (%%/C)");
    } else {
        snprintf(info_str, sizeof(info_str), "Alpha coeff (%%/C)");
    }
    uint8_t info_w = strlen(info_str) * 6;
    uint8_t info_x = (LCD_WIDTH - info_w) / 2;
    LCD_DrawString(info_x, 12, info_str, LCD_COLOR_ON);

    // 3. Edit alpha in center (large font, 2x2, y=22)
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%+.2f %%", s_temp_alpha_edit * 100.0f);
    uint8_t val_w = strlen(val_str) * 6 * 2;
    uint8_t val_x = (LCD_WIDTH - val_w) / 2;
    LCD_DrawStringScaled(val_x, 22, val_str, 2, 2, LCD_COLOR_ON);
    
    // Draw an underline or indicator under the value to show it is editable
    LCD_DrawHLine(val_x, 39, val_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(val_x, 40, val_w - 6, LCD_COLOR_ON);

    // 4. Display final recalculated pH in lower part (y=43, 1x1 font)
    char res_str[48];
    PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
    float t_diff = status.temperature - 25.0f;
    // Khôi phục pH thô ban đầu (chưa bù alpha)
    float ph_raw = status.ph * (1.0f + g_temp_alpha * t_diff);
    // Tính toán lại pH đã bù theo hệ số alpha mới đang hiệu chỉnh
    float final_ph = ph_raw / (1.0f + s_temp_alpha_edit * t_diff);
    if (final_ph < 0.0f) final_ph = 0.0f;
    if (final_ph > 14.0f) final_ph = 14.0f;

    if (g_sys_lang == LANG_VI) {
        snprintf(res_str, sizeof(res_str), "pH (25C): %.2f pH", final_ph);
    } else {
        snprintf(res_str, sizeof(res_str), "pH (25C): %.2f pH", final_ph);
    }
    uint8_t res_w = strlen(res_str) * 6;
    uint8_t res_x = (LCD_WIDTH - res_w) / 2;
    LCD_DrawString(res_x, 43, res_str, LCD_COLOR_ON);

    // 5. Status bar (y=52..63)
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }
    
    LCD_Flush();
}

static void menu_render_modbus_addr(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Dia Chi Cong %d", s_modbus_edit_port);
    } else {
        snprintf(title_buf, sizeof(title_buf), "Port %d Address", s_modbus_edit_port);
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Info / Range (y=12)
    char info_str[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(info_str, sizeof(info_str), "Gia tri: 1 - 247");
    } else {
        snprintf(info_str, sizeof(info_str), "Range: 1 - 247");
    }
    uint8_t info_w = strlen(info_str) * 6;
    uint8_t info_x = (LCD_WIDTH - info_w) / 2;
    LCD_DrawString(info_x, 12, info_str, LCD_COLOR_ON);

    // 3. Edit value in center
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%d", s_modbus_addr_edit);
    uint8_t val_w = strlen(val_str) * 6 * 2;
    uint8_t val_x = (LCD_WIDTH - val_w) / 2;
    LCD_DrawStringScaled(val_x, 22, val_str, 2, 2, LCD_COLOR_ON);

    // Draw underline
    LCD_DrawHLine(val_x, 39, val_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(val_x, 40, val_w - 6, LCD_COLOR_ON);

    // 4. Status bar
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }

    LCD_Flush();
}

static void menu_render_screen_contrast(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Tuong Phan");
    } else {
        snprintf(title_buf, sizeof(title_buf), "Contrast");
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Info / Range (y=12)
    char info_str[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(info_str, sizeof(info_str), "Gia tri: 0 - 63");
    } else {
        snprintf(info_str, sizeof(info_str), "Range: 0 - 63");
    }
    uint8_t info_w = strlen(info_str) * 6;
    uint8_t info_x = (LCD_WIDTH - info_w) / 2;
    LCD_DrawString(info_x, 12, info_str, LCD_COLOR_ON);

    // 3. Edit value in center
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%d", s_contrast_edit);
    uint8_t val_w = strlen(val_str) * 6 * 2;
    uint8_t val_x = (LCD_WIDTH - val_w) / 2;
    LCD_DrawStringScaled(val_x, 22, val_str, 2, 2, LCD_COLOR_ON);

    // Draw underline
    LCD_DrawHLine(val_x, 39, val_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(val_x, 40, val_w - 6, LCD_COLOR_ON);

    // 4. Status bar
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }

    LCD_Flush();
}

static void menu_render_screen_res_ratio(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Ty So Dien Tro");
    } else {
        snprintf(title_buf, sizeof(title_buf), "Resistor Ratio");
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Info / Range (y=12)
    char info_str[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(info_str, sizeof(info_str), "Gia tri: 0 - 7");
    } else {
        snprintf(info_str, sizeof(info_str), "Range: 0 - 7");
    }
    uint8_t info_w = strlen(info_str) * 6;
    uint8_t info_x = (LCD_WIDTH - info_w) / 2;
    LCD_DrawString(info_x, 12, info_str, LCD_COLOR_ON);

    // 3. Edit value in center
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%d", s_res_ratio_edit);
    uint8_t val_w = strlen(val_str) * 6 * 2;
    uint8_t val_x = (LCD_WIDTH - val_w) / 2;
    LCD_DrawStringScaled(val_x, 22, val_str, 2, 2, LCD_COLOR_ON);

    // Draw underline
    LCD_DrawHLine(val_x, 39, val_w - 6, LCD_COLOR_ON);
    LCD_DrawHLine(val_x, 40, val_w - 6, LCD_COLOR_ON);

    // 4. Status bar
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        LCD_DrawString(44, STATUS_BAR_Y + 3, "-/+", LCD_COLOR_OFF);
        LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
    }

    LCD_Flush();
}


static void menu_render_cal_exec(void)
{
    LCD_Clear();

    // 1. Title bar (y=0..9)
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    char title_buf[32];
    if (g_sys_lang == LANG_VI) {
        snprintf(title_buf, sizeof(title_buf), "Hieu Chuan %.2f pH", s_cal_exec.target_ph);
    } else {
        snprintf(title_buf, sizeof(title_buf), "Cal. Point: %.2f pH", s_cal_exec.target_ph);
    }
    LCD_DrawString(4, 1, title_buf, LCD_COLOR_OFF);

    // 2. Main target pH in center
    char target_str[16];
    snprintf(target_str, sizeof(target_str), "%.2f pH", s_cal_exec.target_ph);
    uint8_t w = strlen(target_str) * 6 * 3;
    uint8_t x = (LCD_WIDTH - w) / 2;
    LCD_DrawStringScaled(x, 16, target_str, 3, 4, LCD_COLOR_ON);

    // 3. Live measurements from sensor status
    PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
    char temp_buf[32];
    char mv_buf[32];
    snprintf(temp_buf, sizeof(temp_buf), "%.1f C", status.temperature);

    // Check if live mV is within bounds
    bool is_valid = (status.v_probe_mv >= s_cal_exec.min_mv && status.v_probe_mv <= s_cal_exec.max_mv);
    snprintf(mv_buf, sizeof(mv_buf), "%.1f mV %s", status.v_probe_mv, is_valid ? "OK" : "ERR");

    LCD_DrawString(4, 38, temp_buf, LCD_COLOR_ON);
    LCD_DrawString(64, 38, mv_buf, LCD_COLOR_ON);

    // 4. Status bar (y=52..63)
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);
    if (g_sys_lang == LANG_VI) {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "HUY", LCD_COLOR_OFF);
        if (is_valid) {
            LCD_DrawString(100, STATUS_BAR_Y + 3, "LUU", LCD_COLOR_OFF);
        }
    } else {
        LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);
        if (is_valid) {
            LCD_DrawString(100, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);
        }
    }
    
    LCD_Flush();
}

static void menu_handle_cal_exec_buttons(void)
{
    // ESC - Hủy bỏ, quay về trang cha
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy hieu chuan.");
        g_lcd_need_redraw = true;
        
        if (s_cal_exec.cal_type == 2) {
            goto_page(PAGE_CAL_2PT);
        } else if (s_cal_exec.group_idx == 1) {
            goto_page(PAGE_CAL_3PT_G1);
        } else {
            goto_page(PAGE_CAL_3PT_G2);
        }
        return;
    }

    // ENTER - Xác nhận lưu điểm hiệu chuẩn (nếu điện áp hợp lệ)
    if (btn_edge(BTN_IDX_ENTER)) {
        PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
        bool is_valid = (status.v_probe_mv >= s_cal_exec.min_mv && status.v_probe_mv <= s_cal_exec.max_mv);
        if (is_valid) {
            bool ok = Calibrate_PH_Point(s_cal_exec.target_ph, status.v_probe_mv, status.temperature, s_cal_exec.cal_type);
            if (ok) {
                ESP_LOGI(TAG_MENU, "Da hieu chuan thanh cong moc pH %.2f: %.1f mV", s_cal_exec.target_ph, status.v_probe_mv);
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan pH", "Thanh Cong!", true);
                } else {
                    menu_show_alert_dialog("pH Calibration", "SUCCESSFUL!", true);
                }
            } else {
                ESP_LOGE(TAG_MENU, "Hieu chuan moc pH %.2f that bai!", s_cal_exec.target_ph);
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan pH", "That Bai!", false);
                } else {
                    menu_show_alert_dialog("pH Calibration", "FAILED!", false);
                }
            }
            g_lcd_need_redraw = true;
            
            if (s_cal_exec.cal_type == 2) {
                goto_page(PAGE_CAL_2PT);
            } else if (s_cal_exec.group_idx == 1) {
                goto_page(PAGE_CAL_3PT_G1);
            } else {
                goto_page(PAGE_CAL_3PT_G2);
            }
        } else {
            ESP_LOGW(TAG_MENU, "[ENTER] Nhan nut hieu chuan nhung mV ngoai le: %.1f mV", status.v_probe_mv);
        }
    }
}

/* =====================================================================
 * Khởi tạo GPIO nút bấm
 * ===================================================================== */
void menu_init(void)
{
    ESP_LOGI(TAG_MENU, "Khoi tao GPIO nut bam voi PULL-DOWN noi bo...");

    /* Tất cả các chân nút nhấn (1, 2, 3, 4, 5) đều hỗ trợ pull-down nội trên ESP32-S3 */
    gpio_config_t cfg_pulldown = {
        .pin_bit_mask = (1ULL << BTN_PIN_ESC)
                      | (1ULL << BTN_PIN_DOWN)
                      | (1ULL << BTN_PIN_UP)
                      | (1ULL << BTN_PIN_RIGHT)
                      | (1ULL << BTN_PIN_ENTER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg_pulldown);

    ESP_LOGI(TAG_MENU, "GPIO nut bam (ho tro pull-down) san sang.");

    /* Khoi tao trang thai nut bam ban dau de tranh kich hoat gia khi vua boot */
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn_state[i] = (gpio_get_level(s_btn_gpios[i]) == 1);
        s_btn_prev_state[i] = s_btn_state[i];
    }

    /* Doc cau hinh ngon ngu tu NVS */
    uint32_t lang_val = 0;
    if (Nvs_Read_Number("sys_lang", &lang_val)) {
        g_sys_lang = (lang_val < LANG_COUNT) ? (sys_lang_t)lang_val : LANG_EN;
        ESP_LOGI(TAG_MENU, "Doc ngon ngu tu NVS: %s", (g_sys_lang == LANG_VI) ? "TIENG VIET" : "ENGLISH");
    } else {
        g_sys_lang = LANG_EN;
        ESP_LOGI(TAG_MENU, "Chua co cau hinh ngon ngu, mac dinh: ENGLISH");
    }

    /* Doc cau hinh dinh dang ngay tu NVS */
    uint32_t fmt_val = 0;
    if (Nvs_Read_Number("date_fmt", &fmt_val)) {
        g_date_format = (fmt_val < DATE_FORMAT_COUNT) ? (date_format_t)fmt_val : DATE_FORMAT_YYYY_MM_DD;
        ESP_LOGI(TAG_MENU, "Doc dinh dang ngay tu NVS: %d", g_date_format);
    } else {
        g_date_format = DATE_FORMAT_YYYY_MM_DD;
        ESP_LOGI(TAG_MENU, "Chua co dinh dang ngay tu NVS, mac dinh: YYYY-MM-DD");
    }

    /* Doc cau hinh che do hien thi tu NVS */
    uint32_t mode_val = 0;
    if (Nvs_Read_Number("disp_mode", &mode_val)) {
        g_display_mode = (mode_val < DISP_MODE_COUNT) ? (display_mode_t)mode_val : DISP_MODE_PH;
        ESP_LOGI(TAG_MENU, "Doc che do hien thi tu NVS: %d", g_display_mode);
    } else {
        g_display_mode = DISP_MODE_PH;
        ESP_LOGI(TAG_MENU, "Chua co che do hien thi tu NVS, mac dinh: pH Mode");
    }

    /* Doc cau hinh bo loc so tu NVS */
    uint32_t filter_val = 0;
    if (Nvs_Read_Number("filter_lvl", &filter_val)) {
        g_filter_level = (filter_val < FILTER_LEVEL_COUNT) ? (filter_level_t)filter_val : FILTER_LEVEL_L;
        ESP_LOGI(TAG_MENU, "Doc bo loc so tu NVS: %d", g_filter_level);
    } else {
        g_filter_level = FILTER_LEVEL_L;
        ESP_LOGI(TAG_MENU, "Chua co bo loc so tu NVS, mac dinh: Low (5s)");
    }
    update_system_filters_level(g_filter_level);

    /* Doc cau hinh tuong phan tu NVS */
    uint32_t contrast_val = 0;
    if (Nvs_Read_Number("lcd_contrast", &contrast_val)) {
        g_lcd_contrast = (contrast_val <= 63) ? (uint8_t)contrast_val : 5;
        LCD_SetContrast(g_lcd_contrast);
        ESP_LOGI(TAG_MENU, "Doc tuong phan tu NVS: %d", g_lcd_contrast);
    } else {
        ESP_LOGI(TAG_MENU, "Chua co tuong phan tu NVS, mac dinh: %d", g_lcd_contrast);
    }

    /* Doc cau hinh ty so dien tro noi tu NVS */
    uint32_t res_ratio_val = 0;
    if (Nvs_Read_Number("lcd_res_ratio", &res_ratio_val)) {
        g_lcd_resistor_ratio = (res_ratio_val <= 7) ? (uint8_t)res_ratio_val : 0;
        LCD_SetResistorRatio(g_lcd_resistor_ratio);
        ESP_LOGI(TAG_MENU, "Doc ty so dien tro tu NVS: %d", g_lcd_resistor_ratio);
    } else {
        ESP_LOGI(TAG_MENU, "Chua co ty so dien tro tu NVS, mac dinh: %d", g_lcd_resistor_ratio);
    }
}

static void menu_handle_cal_do_exec_buttons(void)
{
    // ESC - Hủy bỏ, quay về trang cha PAGE_CAL_DO
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy hieu chuan DO.");
        goto_page(PAGE_CAL_DO);
        g_lcd_need_redraw = true;
        return;
    }

    // ENTER - Xác nhận lưu điểm hiệu chuẩn
    if (btn_edge(BTN_IDX_ENTER)) {
        esp_err_t err;
        if (s_do_cal_exec.do_cal_type == 0) {
            err = do_sensor_calibrate_zero();
            if (err == ESP_OK) {
                ESP_LOGI(TAG_MENU, "Hieu chuan DO Diem 0 thanh cong!");
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan DO", "Diem 0: OK!", true);
                } else {
                    menu_show_alert_dialog("DO Calib.", "Zero: Success!", true);
                }
            } else {
                ESP_LOGE(TAG_MENU, "Hieu chuan DO Diem 0 that bai!");
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan DO", "Diem 0: That Bai", false);
                } else {
                    menu_show_alert_dialog("DO Calib.", "Zero: Failed", false);
                }
            }
        } else {
            err = do_sensor_calibrate_slope();
            if (err == ESP_OK) {
                ESP_LOGI(TAG_MENU, "Hieu chuan DO Do Doc thanh cong!");
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan DO", "Do Doc: OK!", true);
                } else {
                    menu_show_alert_dialog("DO Calib.", "Slope: Success!", true);
                }
            } else {
                ESP_LOGE(TAG_MENU, "Hieu chuan DO Do Doc that bai!");
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Hieu Chuan DO", "Do Doc: That Bai", false);
                } else {
                    menu_show_alert_dialog("DO Calib.", "Slope: Failed", false);
                }
            }
        }
        goto_page(PAGE_CAL_DO);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_cal_do_temp_buttons(void)
{
    // ESC - Hủy bỏ, quay về trang cha PAGE_CAL_DO
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy hieu chinh nhiet do DO.");
        goto_page(PAGE_CAL_DO);
        g_lcd_need_redraw = true;
        return;
    }

    // UP - Tăng giá trị nhiệt độ hiệu chỉnh
    if (btn_edge(BTN_IDX_UP)) {
        if (s_do_temp_edit.ref_temp < 100.0f) {
            s_do_temp_edit.ref_temp += 0.1f;
        }
        g_lcd_need_redraw = true;
    }

    // DOWN - Giảm giá trị nhiệt độ hiệu chỉnh
    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_do_temp_edit.ref_temp > -10.0f) {
            s_do_temp_edit.ref_temp -= 0.1f;
        }
        g_lcd_need_redraw = true;
    }

    // ENTER - Lưu giá trị nhiệt độ hiệu chỉnh
    if (btn_edge(BTN_IDX_ENTER)) {
        esp_err_t err = do_sensor_correct_temp(s_do_temp_edit.ref_temp);
        if (err == ESP_OK) {
            ESP_LOGI(TAG_MENU, "Hieu chinh nhiet do DO thanh cong: %.1f C", s_do_temp_edit.ref_temp);
            if (g_sys_lang == LANG_VI) {
                menu_show_alert_dialog("Hieu Chinh T", "Thanh Cong!", true);
            } else {
                menu_show_alert_dialog("Temp Calib.", "SUCCESS!", true);
            }
        } else {
            ESP_LOGE(TAG_MENU, "Hieu chinh nhiet do DO that bai!");
            if (g_sys_lang == LANG_VI) {
                menu_show_alert_dialog("Hieu Chinh T", "That Bai!", false);
            } else {
                menu_show_alert_dialog("Temp Calib.", "FAILED!", false);
            }
        }
        goto_page(PAGE_CAL_DO);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_temp_settings_buttons(void)
{
    bool is_f = (g_temp_mode == TEMP_MODE_ATC_F || g_temp_mode == TEMP_MODE_MTC_F);
    bool is_mtc = (g_temp_mode == TEMP_MODE_MTC_C || g_temp_mode == TEMP_MODE_MTC_F);

    // ESC - Hủy bỏ, quay về trang cha PAGE_SENSOR_SETTINGS
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy cai dat nhiet do.");
        goto_page(PAGE_SENSOR_SETTINGS);
        g_lcd_need_redraw = true;
        return;
    }

    // UP - Tăng giá trị
    if (btn_edge(BTN_IDX_UP)) {
        if (is_mtc) {
            float max_limit = is_f ? 212.0f : 100.0f;
            if (s_manual_temp_edit < max_limit - 0.05f) {
                s_manual_temp_edit += 0.1f;
            }
        } else {
            float max_limit = is_f ? 18.0f : 10.0f; // Độ lệch tối đa: 10 C (18 F)
            if (s_temp_offset_edit < max_limit - 0.05f) {
                s_temp_offset_edit += 0.1f;
            }
        }
        g_lcd_need_redraw = true;
    }

    // DOWN - Giảm giá trị
    if (btn_edge(BTN_IDX_DOWN)) {
        if (is_mtc) {
            float min_limit = is_f ? 32.0f : 0.0f;
            if (s_manual_temp_edit > min_limit + 0.05f) {
                s_manual_temp_edit -= 0.1f;
            }
        } else {
            float min_limit = is_f ? -18.0f : -10.0f; // Độ lệch tối thiểu: -10 C (-18 F)
            if (s_temp_offset_edit > min_limit + 0.05f) {
                s_temp_offset_edit -= 0.1f;
            }
        }
        g_lcd_need_redraw = true;
    }

    // ENTER - Lưu giá trị
    if (btn_edge(BTN_IDX_ENTER)) {
        if (is_mtc) {
            if (is_f) {
                g_manual_temp = (s_manual_temp_edit - 32.0f) / 1.8f;
            } else {
                g_manual_temp = s_manual_temp_edit;
            }
            // Giới hạn an toàn Celsius
            if (g_manual_temp < 0.0f) g_manual_temp = 0.0f;
            if (g_manual_temp > 100.0f) g_manual_temp = 100.0f;

            Save_Temp_Settings_To_Storage();
            ESP_LOGI(TAG_MENU, "Da luu manual_temp: %.1f C (%.1f F)", g_manual_temp, g_manual_temp * 1.8f + 32.0f);
            
            if (g_sys_lang == LANG_VI) {
                menu_show_alert_dialog("Nhiet Do Thu Cong", "Thanh Cong!", true);
            } else {
                menu_show_alert_dialog("Manual Temp", "SUCCESS!", true);
            }
        } else {
            if (is_f) {
                g_temp_offset = s_temp_offset_edit / 1.8f;
            } else {
                g_temp_offset = s_temp_offset_edit;
            }
            // Giới hạn an toàn Celsius
            if (g_temp_offset < -10.0f) g_temp_offset = -10.0f;
            if (g_temp_offset > 10.0f) g_temp_offset = 10.0f;

            Save_Temp_Settings_To_Storage();
            ESP_LOGI(TAG_MENU, "Da luu temp_offset: %.1f C (%.1f F)", g_temp_offset, g_temp_offset * 1.8f);

            if (g_sys_lang == LANG_VI) {
                menu_show_alert_dialog("Hieu Chinh T", "Thanh Cong!", true);
            } else {
                menu_show_alert_dialog("Temp Calib.", "SUCCESS!", true);
            }
        }
        goto_page(PAGE_SENSOR_SETTINGS);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_temp_lin_comp_buttons(void)
{
    // ESC - Hủy bỏ, quay về trang cha PAGE_SENSOR_SETTINGS
    if (btn_edge(BTN_IDX_ESC)) {
        ESP_LOGI(TAG_MENU, "[ESC] Huy cai dat he so bu tuyen tinh.");
        goto_page(PAGE_SENSOR_SETTINGS);
        g_lcd_need_redraw = true;
        return;
    }

    // UP - Tăng giá trị hệ số alpha
    if (btn_edge(BTN_IDX_UP)) {
        if (s_temp_alpha_edit < 0.100f - 0.0005f) {
            s_temp_alpha_edit += 0.001f;
        }
        g_lcd_need_redraw = true;
    }

    // DOWN - Giảm giá trị hệ số alpha
    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_temp_alpha_edit > -0.100f + 0.0005f) {
            s_temp_alpha_edit -= 0.001f;
        }
        g_lcd_need_redraw = true;
    }

    // ENTER - Lưu giá trị hệ số alpha
    if (btn_edge(BTN_IDX_ENTER)) {
        g_temp_alpha = s_temp_alpha_edit;
        // Giới hạn an toàn
        if (g_temp_alpha < -0.100f) g_temp_alpha = -0.100f;
        if (g_temp_alpha > 0.100f) g_temp_alpha = 0.100f;

        Save_Temp_Settings_To_Storage();
        ESP_LOGI(TAG_MENU, "Da luu temp_alpha: %.3f", g_temp_alpha);
        
        if (g_sys_lang == LANG_VI) {
            menu_show_alert_dialog("He So Alpha", "Thanh Cong!", true);
        } else {
            menu_show_alert_dialog("Alpha Coeff.", "SUCCESS!", true);
        }
        
        goto_page(PAGE_SENSOR_SETTINGS);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_modbus_addr_buttons(void)
{
    if (btn_edge(BTN_IDX_ESC)) {
        goto_page(s_modbus_edit_port == 1 ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2);
        g_lcd_need_redraw = true;
        return;
    }

    if (btn_edge(BTN_IDX_UP)) {
        if (s_modbus_addr_edit < 247) {
            s_modbus_addr_edit++;
        } else {
            s_modbus_addr_edit = 1;
        }
        g_lcd_need_redraw = true;
    }

    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_modbus_addr_edit > 1) {
            s_modbus_addr_edit--;
        } else {
            s_modbus_addr_edit = 247;
        }
        g_lcd_need_redraw = true;
    }

    if (btn_edge(BTN_IDX_ENTER)) {
        if (s_modbus_edit_port == 1) {
            g_mb1_addr = s_modbus_addr_edit;
            Nvs_Write_Number("mb1_addr", g_mb1_addr);
            ESP_LOGI("MENU", "Luu Dia Chi Cong 1: %d", g_mb1_addr);
        } else {
            g_mb2_addr = s_modbus_addr_edit;
            Nvs_Write_Number("mb2_addr", g_mb2_addr);
            ESP_LOGI("MENU", "Luu Dia Chi Cong 2: %d", g_mb2_addr);
            
            do_sensor_update_config(g_mb2_addr, g_mb2_baud, g_mb2_parity, g_mb2_stop);
        }

        if (g_sys_lang == LANG_VI) {
            menu_show_alert_dialog("Dia Chi MB", "Thanh Cong!", true);
        } else {
            menu_show_alert_dialog("MB Address", "SUCCESS!", true);
        }

        goto_page(s_modbus_edit_port == 1 ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_screen_contrast_buttons(void)
{
    // ESC - Huy bo, khoi phuc gia tri cu va quay ve PAGE_SCREEN_SETTINGS
    if (btn_edge(BTN_IDX_ESC)) {
        LCD_SetContrast(g_lcd_contrast);
        goto_page(PAGE_SCREEN_SETTINGS);
        g_lcd_need_redraw = true;
        return;
    }

    // UP - Tang gia tri tuong phan
    if (btn_edge(BTN_IDX_UP)) {
        if (s_contrast_edit < 63) {
            s_contrast_edit++;
            LCD_SetContrast(s_contrast_edit);
        }
        g_lcd_need_redraw = true;
    }

    // DOWN - Giam gia tri tuong phan
    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_contrast_edit > 0) {
            s_contrast_edit--;
            LCD_SetContrast(s_contrast_edit);
        }
        g_lcd_need_redraw = true;
    }

    // ENTER - Luu va quay ve PAGE_SCREEN_SETTINGS
    if (btn_edge(BTN_IDX_ENTER)) {
        g_lcd_contrast = s_contrast_edit;
        Nvs_Write_Number("lcd_contrast", g_lcd_contrast);
        ESP_LOGI(TAG_MENU, "Da luu tuong phan: %d", g_lcd_contrast);

        if (g_sys_lang == LANG_VI) {
            menu_show_alert_dialog("Tuong Phan", "Thanh Cong!", true);
        } else {
            menu_show_alert_dialog("Contrast", "SUCCESS!", true);
        }
        goto_page(PAGE_SCREEN_SETTINGS);
        g_lcd_need_redraw = true;
    }
}

static void menu_handle_screen_res_ratio_buttons(void)
{
    // ESC - Huy bo, khoi phuc gia tri cu va quay ve PAGE_SCREEN_SETTINGS
    if (btn_edge(BTN_IDX_ESC)) {
        LCD_SetResistorRatio(g_lcd_resistor_ratio);
        goto_page(PAGE_SCREEN_SETTINGS);
        g_lcd_need_redraw = true;
        return;
    }

    // UP - Tang ty so dien tro
    if (btn_edge(BTN_IDX_UP)) {
        if (s_res_ratio_edit < 7) {
            s_res_ratio_edit++;
            LCD_SetResistorRatio(s_res_ratio_edit);
        }
        g_lcd_need_redraw = true;
    }

    // DOWN - Giam ty so dien tro
    if (btn_edge(BTN_IDX_DOWN)) {
        if (s_res_ratio_edit > 0) {
            s_res_ratio_edit--;
            LCD_SetResistorRatio(s_res_ratio_edit);
        }
        g_lcd_need_redraw = true;
    }

    // ENTER - Luu va quay ve PAGE_SCREEN_SETTINGS
    if (btn_edge(BTN_IDX_ENTER)) {
        g_lcd_resistor_ratio = s_res_ratio_edit;
        Nvs_Write_Number("lcd_res_ratio", g_lcd_resistor_ratio);
        ESP_LOGI(TAG_MENU, "Da luu ty so dien tro: %d", g_lcd_resistor_ratio);

        if (g_sys_lang == LANG_VI) {
            menu_show_alert_dialog("Ty So Dien Tro", "Thanh Cong!", true);
        } else {
            menu_show_alert_dialog("Resistor Ratio", "SUCCESS!", true);
        }
        goto_page(PAGE_SCREEN_SETTINGS);
        g_lcd_need_redraw = true;
    }
}

/* =====================================================================
 * Xử lý nút bấm & cập nhật trạng thái menu
 * ===================================================================== */
void menu_handle_buttons(void)
{
    /* Đọc và chống rung toàn bộ nút bấm */
    poll_and_debounce_buttons();

    /* ── Màn hình đo lường: chỉ ENTER mới vào menu ── */
    if (!g_menu.in_menu) {
        if (btn_edge(BTN_IDX_ENTER)) {
            ESP_LOGI(TAG_MENU, "[ENTER] Nhan nut vao Menu.");
            g_menu.in_menu = true;
            goto_page(PAGE_MAIN_MENU);
            g_lcd_need_redraw = true;
        }
        if (btn_edge(BTN_IDX_UP)) {
            if (g_lcd_contrast < 63) {
                g_lcd_contrast++;
                LCD_SetContrast(g_lcd_contrast);
                g_lcd_need_redraw = true;
                ESP_LOGI(TAG_MENU, "[UP] Tang tuong phan: %d", g_lcd_contrast);
            }
        }
        if (btn_edge(BTN_IDX_DOWN)) {
            if (g_lcd_contrast > 0) {
                g_lcd_contrast--;
                LCD_SetContrast(g_lcd_contrast);
                g_lcd_need_redraw = true;
                ESP_LOGI(TAG_MENU, "[DOWN] Giam tuong phan: %d", g_lcd_contrast);
            }
        }
        if (btn_edge(BTN_IDX_ESC)) {
            ESP_LOGI(TAG_MENU, "[ESC] Nhan nut (Moi truong do luong - khong co chuc nang).");
        }
        if (btn_edge(BTN_IDX_RIGHT)) {
            ESP_LOGI(TAG_MENU, "[RIGHT] Nhan nut (Moi truong do luong - khong co chuc nang).");
        }
        return;
    }

    /* ── Trong menu ── */
    if (g_menu.current_page == PAGE_TIME_SETTINGS) {
        menu_handle_time_settings_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_MODBUS_EDIT_ADDR) {
        menu_handle_modbus_addr_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_EXEC) {
        menu_handle_cal_exec_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_DO_EXEC) {
        menu_handle_cal_do_exec_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_DO_TEMP) {
        menu_handle_cal_do_temp_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_TEMP_SETTINGS) {
        menu_handle_temp_settings_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_TEMP_LIN_COMP) {
        menu_handle_temp_lin_comp_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_SCREEN_CONTRAST) {
        menu_handle_screen_contrast_buttons();
        return;
    }
    if (g_menu.current_page == PAGE_SCREEN_RES_RATIO) {
        menu_handle_screen_res_ratio_buttons();
        return;
    }

    const page_def_t *page = (g_sys_lang == LANG_VI) ? &s_pages_vi[g_menu.current_page] : &s_pages_en[g_menu.current_page];

    /* ESC – quay lại trang cha */
    if (btn_edge(BTN_IDX_ESC)) {
        menu_page_t parent = page->parent;
        if (g_menu.current_page == PAGE_MODBUS_EDIT_ADDR ||
            g_menu.current_page == PAGE_MODBUS_SELECT_BAUD ||
            g_menu.current_page == PAGE_MODBUS_SELECT_PARITY ||
            g_menu.current_page == PAGE_MODBUS_SELECT_STOP) {
            parent = (s_modbus_edit_port == 1) ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2;
        }
        if (parent == PAGE_MEASUREMENT) {
            ESP_LOGI(TAG_MENU, "[ESC] Thoat menu -> quay ve do luong.");
            g_menu.in_menu      = false;
            g_menu.current_page = PAGE_MEASUREMENT;
            g_lcd_need_redraw   = true;
        } else {
            ESP_LOGI(TAG_MENU, "[ESC] Quay ve menu cha.");
            goto_page(parent);
            g_lcd_need_redraw = true;
        }
    }

    /* UP – di chuyển lên */
    if (btn_edge(BTN_IDX_UP)) {
        if (g_menu.selected > 0) {
            g_menu.selected--;
            if (g_menu.selected < g_menu.scroll_offset) {
                g_menu.scroll_offset = g_menu.selected;
            }
            g_lcd_need_redraw = true;
            ESP_LOGI(TAG_MENU, "[UP] Tro den: %s (Index: %d)", page->items[g_menu.selected], g_menu.selected);
        } else {
            ESP_LOGI(TAG_MENU, "[UP] Da dung o dau trang: %s", page->title);
        }
    }

    /* DOWN – di chuyển xuống */
    if (btn_edge(BTN_IDX_DOWN)) {
        if (g_menu.selected < page->item_count - 1) {
            g_menu.selected++;
            if (g_menu.selected >= g_menu.scroll_offset + VISIBLE_ITEMS) {
                g_menu.scroll_offset = g_menu.selected - VISIBLE_ITEMS + 1;
            }
            g_lcd_need_redraw = true;
            ESP_LOGI(TAG_MENU, "[DOWN] Tro den: %s (Index: %d)", page->items[g_menu.selected], g_menu.selected);
        } else {
            ESP_LOGI(TAG_MENU, "[DOWN] Da dung o cuoi trang: %s", page->title);
        }
    }

    /* ENTER – vào mục con */
    if (btn_edge(BTN_IDX_ENTER)) {
        menu_page_t child = page->children[g_menu.selected];
        ESP_LOGI(TAG_MENU, "[ENTER] Chon muc: %s", page->items[g_menu.selected]);
        if (child != PAGE_LEAF && child != PAGE_COUNT) {
            if (child == PAGE_CAL_DO_TEMP) {
                PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
                s_do_temp_edit.ref_temp = status.do_valid ? status.do_temp_c : status.temperature;
                if (s_do_temp_edit.ref_temp < 0.0f || s_do_temp_edit.ref_temp > 100.0f) {
                    s_do_temp_edit.ref_temp = 25.0f;
                }
            } else if (child == PAGE_CAL_DO_EXEC) {
                s_do_cal_exec.do_cal_type = g_menu.selected;
            } else if (child == PAGE_MODBUS_PORT1) {
                s_modbus_edit_port = 1;
            } else if (child == PAGE_MODBUS_PORT2) {
                s_modbus_edit_port = 2;
            }
            goto_page(child);
            g_lcd_need_redraw = true;
        } else {
            /* Muc la: xu ly thay doi gia tri / ngon ngu / dinh dang ngay */
            if (g_menu.current_page == PAGE_LANGUAGE) {
                sys_lang_t new_lang = (g_menu.selected == 1) ? LANG_VI : LANG_EN;
                if (new_lang != g_sys_lang) {
                    g_sys_lang = new_lang;
                    Nvs_Write_Number("sys_lang", (uint32_t)g_sys_lang);
                    ESP_LOGI(TAG_MENU, "Da doi ngon ngu thanh: %s", (g_sys_lang == LANG_VI) ? "TIENG VIET" : "ENGLISH");
                }
                // Quay lai trang cha (System Settings) de hien thi ngon ngu moi
                menu_page_t parent = page->parent;
                goto_page(parent);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_DATE_FORMAT) {
                date_format_t new_fmt = (date_format_t)g_menu.selected;
                if (new_fmt < DATE_FORMAT_COUNT) {
                    g_date_format = new_fmt;
                    Nvs_Write_Number("date_fmt", (uint32_t)g_date_format);
                    ESP_LOGI(TAG_MENU, "Da doi dinh dang ngay thanh: %d", g_date_format);
                }
                // Quay lai trang cha
                menu_page_t parent = page->parent;
                goto_page(parent);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_DISPLAY_MODE) {
                display_mode_t new_mode = (display_mode_t)g_menu.selected;
                if (new_mode < DISP_MODE_COUNT) {
                    g_display_mode = new_mode;
                    Nvs_Write_Number("disp_mode", (uint32_t)g_display_mode);
                    ESP_LOGI(TAG_MENU, "Da doi che do hien thi thanh: %d", g_display_mode);
                }
                // Quay lai trang cha
                menu_page_t parent = page->parent;
                goto_page(parent);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_DIGITAL_FILTER) {
                filter_level_t new_lvl = (filter_level_t)g_menu.selected;
                if (new_lvl < FILTER_LEVEL_COUNT) {
                    g_filter_level = new_lvl;
                    Nvs_Write_Number("filter_lvl", (uint32_t)g_filter_level);
                    update_system_filters_level(g_filter_level);
                    ESP_LOGI(TAG_MENU, "Da doi bo loc so thanh: %d", g_filter_level);
                }
                // Quay lai trang cha
                menu_page_t parent = page->parent;
                goto_page(parent);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_TEMP_MODE) {
                temp_mode_t new_mode = (temp_mode_t)g_menu.selected;
                if (new_mode < TEMP_MODE_COUNT) {
                    g_temp_mode = new_mode;
                    Save_Temp_Settings_To_Storage();
                    ESP_LOGI(TAG_MENU, "Da doi che do nhiet do thanh: %d", g_temp_mode);
                }
                // Quay lai trang cha
                menu_page_t parent = page->parent;
                goto_page(parent);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_CAL_2PT) {
                s_cal_exec.cal_type = 2;
                s_cal_exec.group_idx = 0;
                s_cal_exec.point_idx = g_menu.selected;
                if (g_menu.selected == 0) { // pH 4.00
                    s_cal_exec.target_ph = 4.00f;
                    s_cal_exec.min_mv = 157.0f;
                    s_cal_exec.max_mv = 197.0f;
                } else { // pH 7.00
                    s_cal_exec.target_ph = 7.00f;
                    s_cal_exec.min_mv = -20.0f;
                    s_cal_exec.max_mv = 20.0f;
                }
                goto_page(PAGE_CAL_EXEC);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_CAL_3PT_G1) {
                s_cal_exec.cal_type = 3;
                s_cal_exec.group_idx = 1;
                s_cal_exec.point_idx = g_menu.selected;
                if (g_menu.selected == 0) { // pH 4.00
                    s_cal_exec.target_ph = 4.00f;
                    s_cal_exec.min_mv = 157.0f;
                    s_cal_exec.max_mv = 197.0f;
                } else if (g_menu.selected == 1) { // pH 6.86
                    s_cal_exec.target_ph = 6.86f;
                    s_cal_exec.min_mv = -20.0f;
                    s_cal_exec.max_mv = 20.0f;
                } else { // pH 9.18
                    s_cal_exec.target_ph = 9.18f;
                    s_cal_exec.min_mv = -148.0f;
                    s_cal_exec.max_mv = -108.0f;
                }
                goto_page(PAGE_CAL_EXEC);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_CAL_3PT_G2) {
                s_cal_exec.cal_type = 3;
                s_cal_exec.group_idx = 2;
                s_cal_exec.point_idx = g_menu.selected;
                if (g_menu.selected == 0) { // pH 4.00
                    s_cal_exec.target_ph = 4.00f;
                    s_cal_exec.min_mv = 157.0f;
                    s_cal_exec.max_mv = 197.0f;
                } else if (g_menu.selected == 1) { // pH 7.00
                    s_cal_exec.target_ph = 7.00f;
                    s_cal_exec.min_mv = -20.0f;
                    s_cal_exec.max_mv = 20.0f;
                } else { // pH 10.00
                    s_cal_exec.target_ph = 10.00f;
                    s_cal_exec.min_mv = -197.0f;
                    s_cal_exec.max_mv = -157.0f;
                }
                goto_page(PAGE_CAL_EXEC);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_CAL_DO) {
                if (g_menu.selected == 3) {
                    esp_err_t err = do_sensor_reset();
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG_MENU, "Reset cam bien DO thanh cong!");
                        if (g_sys_lang == LANG_VI) {
                            menu_show_alert_dialog("Khoi Phuc DO", "Thanh Cong!", true);
                        } else {
                            menu_show_alert_dialog("Reset DO", "SUCCESSFUL!", true);
                        }
                    } else {
                        ESP_LOGE(TAG_MENU, "Reset cam bien DO that bai!");
                        if (g_sys_lang == LANG_VI) {
                            menu_show_alert_dialog("Khoi Phuc DO", "That Bai!", false);
                        } else {
                            menu_show_alert_dialog("Reset DO", "FAILED!", false);
                        }
                    }
                    goto_page(PAGE_CAL_DO);
                    g_lcd_need_redraw = true;
                }
            } else if (g_menu.current_page == PAGE_MODBUS_SELECT_BAUD) {
                uint32_t selected_baud = s_baud_rates[g_menu.selected];
                if (s_modbus_edit_port == 1) {
                    g_mb1_baud = selected_baud;
                    Nvs_Write_Number("mb1_baud", g_mb1_baud);
                    ESP_LOGI(TAG_MENU, "Saved Port 1 Baud: %lu", (unsigned long)g_mb1_baud);
                } else {
                    g_mb2_baud = selected_baud;
                    Nvs_Write_Number("mb2_baud", g_mb2_baud);
                    ESP_LOGI(TAG_MENU, "Saved Port 2 Baud: %lu", (unsigned long)g_mb2_baud);
                    do_sensor_update_config(g_mb2_addr, g_mb2_baud, g_mb2_parity, g_mb2_stop);
                }
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Toc Do Baud", "Thanh Cong!", true);
                } else {
                    menu_show_alert_dialog("Baud Rate", "SUCCESS!", true);
                }
                goto_page(s_modbus_edit_port == 1 ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_MODBUS_SELECT_PARITY) {
                uint8_t selected_parity = g_menu.selected; // 0 = None, 1 = Even, 2 = Odd
                if (s_modbus_edit_port == 1) {
                    g_mb1_parity = selected_parity;
                    Nvs_Write_Number("mb1_parity", g_mb1_parity);
                    ESP_LOGI(TAG_MENU, "Saved Port 1 Parity: %d", g_mb1_parity);
                } else {
                    g_mb2_parity = selected_parity;
                    Nvs_Write_Number("mb2_parity", g_mb2_parity);
                    ESP_LOGI(TAG_MENU, "Saved Port 2 Parity: %d", g_mb2_parity);
                    do_sensor_update_config(g_mb2_addr, g_mb2_baud, g_mb2_parity, g_mb2_stop);
                }
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Kiem Tra Parity", "Thanh Cong!", true);
                } else {
                    menu_show_alert_dialog("Parity Check", "SUCCESS!", true);
                }
                goto_page(s_modbus_edit_port == 1 ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2);
                g_lcd_need_redraw = true;
            } else if (g_menu.current_page == PAGE_MODBUS_SELECT_STOP) {
                uint8_t selected_stop = g_menu.selected + 1; // 1 = 1 stop bit, 2 = 2 stop bits
                if (s_modbus_edit_port == 1) {
                    g_mb1_stop = selected_stop;
                    Nvs_Write_Number("mb1_stop", g_mb1_stop);
                    ESP_LOGI(TAG_MENU, "Saved Port 1 Stop Bits: %d", g_mb1_stop);
                } else {
                    g_mb2_stop = selected_stop;
                    Nvs_Write_Number("mb2_stop", g_mb2_stop);
                    ESP_LOGI(TAG_MENU, "Saved Port 2 Stop Bits: %d", g_mb2_stop);
                    do_sensor_update_config(g_mb2_addr, g_mb2_baud, g_mb2_parity, g_mb2_stop);
                }
                if (g_sys_lang == LANG_VI) {
                    menu_show_alert_dialog("Bit Stop", "Thanh Cong!", true);
                } else {
                    menu_show_alert_dialog("Stop Bits", "SUCCESS!", true);
                }
                goto_page(s_modbus_edit_port == 1 ? PAGE_MODBUS_PORT1 : PAGE_MODBUS_PORT2);
                g_lcd_need_redraw = true;
            } else {
                ESP_LOGI(TAG_MENU, "Nhan nut muc la (chua gan logic): %s", page->items[g_menu.selected]);
            }
        }
    }

    /* RIGHT – dự phòng (bỏ qua) */
    if (btn_edge(BTN_IDX_RIGHT)) {
        ESP_LOGI(TAG_MENU, "[RIGHT] Nhan nut du phong (khong co chuc nang).");
    }
}

/* =====================================================================
 * Vẽ màn hình menu lên LCD
 * ===================================================================== */

/** Vẽ mũi tên đơn giản bằng pixel (▲ hoặc ▼) tại (x,y) */
static void draw_arrow_up(uint8_t x, uint8_t y, uint8_t color)
{
    /* 5×4 px arrow ▲ */
    LCD_DrawPixel(x + 2, y,     color);
    LCD_DrawPixel(x + 1, y + 1, color);
    LCD_DrawPixel(x + 2, y + 1, color);
    LCD_DrawPixel(x + 3, y + 1, color);
    LCD_DrawHLine(x,     y + 2, 5, color);
    LCD_DrawHLine(x,     y + 3, 5, color);
}

static void draw_arrow_down(uint8_t x, uint8_t y, uint8_t color)
{
    /* 5×4 px arrow ▼ */
    LCD_DrawHLine(x,     y,     5, color);
    LCD_DrawHLine(x,     y + 1, 5, color);
    LCD_DrawPixel(x + 1, y + 2, color);
    LCD_DrawPixel(x + 2, y + 2, color);
    LCD_DrawPixel(x + 3, y + 2, color);
    LCD_DrawPixel(x + 2, y + 3, color);
}

static void draw_arrow_right(uint8_t x, uint8_t y, uint8_t color)
{
    /* 3×5 px arrow ► chỉ mục con */
    LCD_DrawVLine(x,     y,     5, color);
    LCD_DrawVLine(x + 1, y + 1, 3, color);
    LCD_DrawPixel(x + 2, y + 2, color);
}

/** Vẽ các mũi tên đặc lớn (7×7 px) cho thanh công cụ phía dưới */
static void draw_arrow_up_large(uint8_t x, uint8_t y, uint8_t color)
{
    /* 7×7 px solid arrow ▲ */
    LCD_DrawPixel(x + 3, y,     color);
    LCD_DrawHLine(x + 2, y + 1, 3, color);
    LCD_DrawHLine(x + 2, y + 2, 3, color);
    LCD_DrawHLine(x + 1, y + 3, 5, color);
    LCD_DrawHLine(x + 1, y + 4, 5, color);
    LCD_DrawHLine(x,     y + 5, 7, color);
    LCD_DrawHLine(x,     y + 6, 7, color);
}

static void draw_arrow_down_large(uint8_t x, uint8_t y, uint8_t color)
{
    /* 7×7 px solid arrow ▼ */
    LCD_DrawHLine(x,     y,     7, color);
    LCD_DrawHLine(x,     y + 1, 7, color);
    LCD_DrawHLine(x + 1, y + 2, 5, color);
    LCD_DrawHLine(x + 1, y + 3, 5, color);
    LCD_DrawHLine(x + 2, y + 4, 3, color);
    LCD_DrawHLine(x + 2, y + 5, 3, color);
    LCD_DrawPixel(x + 3, y + 6, color);
}

static void draw_arrow_right_large(uint8_t x, uint8_t y, uint8_t color)
{
    /* 7×7 px solid arrow ► */
    LCD_DrawVLine(x,     y,     7, color);
    LCD_DrawVLine(x + 1, y,     7, color);
    LCD_DrawVLine(x + 2, y + 1, 5, color);
    LCD_DrawVLine(x + 3, y + 1, 5, color);
    LCD_DrawVLine(x + 4, y + 2, 3, color);
    LCD_DrawVLine(x + 5, y + 2, 3, color);
    LCD_DrawPixel(x + 6, y + 3, color);
}

void menu_render(void)
{
    if (g_menu.current_page == PAGE_TIME_SETTINGS) {
        menu_render_time_settings();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_EXEC) {
        menu_render_cal_exec();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_DO_EXEC) {
        menu_render_cal_do_exec();
        return;
    }
    if (g_menu.current_page == PAGE_CAL_DO_TEMP) {
        menu_render_cal_do_temp();
        return;
    }
    if (g_menu.current_page == PAGE_TEMP_SETTINGS) {
        menu_render_temp_settings();
        return;
    }
    if (g_menu.current_page == PAGE_TEMP_LIN_COMP) {
        menu_render_temp_lin_comp();
        return;
    }
    if (g_menu.current_page == PAGE_MODBUS_EDIT_ADDR) {
        menu_render_modbus_addr();
        return;
    }
    if (g_menu.current_page == PAGE_SCREEN_CONTRAST) {
        menu_render_screen_contrast();
        return;
    }
    if (g_menu.current_page == PAGE_SCREEN_RES_RATIO) {
        menu_render_screen_res_ratio();
        return;
    }

    const page_def_t *page = (g_sys_lang == LANG_VI) ? &s_pages_vi[g_menu.current_page] : &s_pages_en[g_menu.current_page];
    LCD_Clear();

    /* ── 1. Title bar (y=0..9): nền đen, chữ trắng ── */
    LCD_FillRect(0, 0, 128, TITLE_BAR_H, LCD_COLOR_ON);
    LCD_DrawString(4, 1, page->title, LCD_COLOR_OFF);

    /* ── 2. Danh sách mục (y=11..50) ── */
    uint8_t visible = page->item_count - g_menu.scroll_offset;
    if (visible > VISIBLE_ITEMS) visible = VISIBLE_ITEMS;

    for (uint8_t i = 0; i < visible; i++) {
        uint8_t idx = g_menu.scroll_offset + i;
        uint8_t y   = ITEMS_Y_START + i * ITEM_ROW_H;

        char item_str[32];
        strncpy(item_str, page->items[idx], sizeof(item_str) - 1);
        item_str[sizeof(item_str) - 1] = '\0';
        if (g_menu.current_page == PAGE_DATE_FORMAT && idx == (uint8_t)g_date_format) {
            strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
        } else if (g_menu.current_page == PAGE_DISPLAY_MODE && idx == (uint8_t)g_display_mode) {
            strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
        } else if (g_menu.current_page == PAGE_DIGITAL_FILTER && idx == (uint8_t)g_filter_level) {
            strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
        } else if (g_menu.current_page == PAGE_TEMP_MODE && idx == (uint8_t)g_temp_mode) {
            strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
        } else if (g_menu.current_page == PAGE_MODBUS_SELECT_BAUD) {
            uint32_t active_baud = (s_modbus_edit_port == 1) ? g_mb1_baud : g_mb2_baud;
            if (s_baud_rates[idx] == active_baud) {
                strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
            }
        } else if (g_menu.current_page == PAGE_MODBUS_SELECT_PARITY) {
            uint8_t active_parity = (s_modbus_edit_port == 1) ? g_mb1_parity : g_mb2_parity;
            if (idx == active_parity) {
                strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
            }
        } else if (g_menu.current_page == PAGE_MODBUS_SELECT_STOP) {
            uint8_t active_stop = (s_modbus_edit_port == 1) ? g_mb1_stop : g_mb2_stop;
            if (idx == (active_stop - 1)) {
                strncat(item_str, " *", sizeof(item_str) - strlen(item_str) - 1);
            }
        } else if (g_menu.current_page == PAGE_CAL_2PT) {
            if (idx == 0) {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "1. Thap 4.00/%.1f" : "1. Low  4.00/%.1f", ph_cal.ph4_voltage_mv);
            } else {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "2. Cao  7.00/%.1f" : "2. High 7.00/%.1f", ph_cal.ph7_voltage_mv);
            }
        } else if (g_menu.current_page == PAGE_CAL_3PT_G1) {
            if (idx == 0) {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "1. Thap 4.00/%.1f" : "1. Low  4.00/%.1f", ph_cal.ph4_voltage_mv);
            } else if (idx == 1) {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "2. Trung 6.86/%.1f" : "2. Mid  6.86/%.1f", ph_cal.ph7_voltage_mv);
            } else {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "3. Cao  9.18/%.1f" : "3. High 9.18/%.1f", ph_cal.ph10_voltage_mv);
            }
        } else if (g_menu.current_page == PAGE_CAL_3PT_G2) {
            if (idx == 0) {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "1. Thap 4.00/%.1f" : "1. Low  4.00/%.1f", ph_cal.ph4_voltage_mv);
            } else if (idx == 1) {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "2. Trung 7.00/%.1f" : "2. Mid  7.00/%.1f", ph_cal.ph7_voltage_mv);
            } else {
                snprintf(item_str, sizeof(item_str), (g_sys_lang == LANG_VI) ? "3. Cao  10.0/%.1f" : "3. High 10.0/%.1f", ph_cal.ph10_voltage_mv);
            }
        }

        if (idx == g_menu.selected) {
            /* Mục được chọn: nền đen, chữ trắng */
            LCD_FillRect(0, y, 128, ITEM_ROW_H, LCD_COLOR_ON);
            LCD_DrawString(4, y + 1, item_str, LCD_COLOR_OFF);
            /* Mũi tên ► ở bên phải để chỉ mục đang chọn */
            if (page->children[idx] != PAGE_LEAF) {
                draw_arrow_right(121, y + 2, LCD_COLOR_OFF);
            }
        } else {
            /* Mục bình thường: nền trắng, chữ đen */
            LCD_DrawString(4, y + 1, item_str, LCD_COLOR_ON);
        }
    }

    /* ── 3. Scroll indicator (mũi tên nhỏ ngoài rìa phải) ── */
    if (g_menu.scroll_offset > 0) {
        /* Còn mục phía trên */
        draw_arrow_up(120, ITEMS_Y_START + 1, LCD_COLOR_ON);
    }
    if (g_menu.scroll_offset + VISIBLE_ITEMS < page->item_count) {
        /* Còn mục phía dưới */
        draw_arrow_down(120, STATUS_BAR_Y - 6, LCD_COLOR_ON);
    }

    /* ── 4. Status bar (y=52..63): nền đen, nhãn nút ── */
    LCD_FillRect(0, STATUS_BAR_Y, 128, STATUS_BAR_H, LCD_COLOR_ON);

    /* "ESC" */
    LCD_DrawString(8, STATUS_BAR_Y + 3, "ESC", LCD_COLOR_OFF);

    /* Mũi tên ▼ (DOWN) */
    draw_arrow_down_large(39, STATUS_BAR_Y + 3, LCD_COLOR_OFF);

    /* Mũi tên ▲ (UP) */
    draw_arrow_up_large(60, STATUS_BAR_Y + 3, LCD_COLOR_OFF);

    /* Mũi tên ► (RIGHT) */
    draw_arrow_right_large(81, STATUS_BAR_Y + 3, LCD_COLOR_OFF);

    /* "ENT" */
    LCD_DrawString(102, STATUS_BAR_Y + 3, "ENT", LCD_COLOR_OFF);

    LCD_Flush();
}
