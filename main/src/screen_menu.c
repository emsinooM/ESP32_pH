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

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG_MENU = "MENU";

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
static const page_def_t s_pages[PAGE_COUNT] = {

    /* ── Main Menu ── */
    [PAGE_MAIN_MENU] = {
        .title      = "Main Menu",
        .item_count = 3,
        .items      = { "1 System Settings",
                        "2 Sensor Settings",
                        "3 Output Settings" },
        .children   = { PAGE_SYSTEM_SETTINGS,
                        PAGE_SENSOR_SETTINGS,
                        PAGE_OUTPUT_SETTINGS },
        .parent     = PAGE_MEASUREMENT,
    },

    /* ── System Settings ── */
    [PAGE_SYSTEM_SETTINGS] = {
        .title      = "System Settings",
        .item_count = 3,
        .items      = { "1.1 Language",
                        "1.2 Date",
                        "1.3 History Mode" },
        .children   = { PAGE_LANGUAGE, PAGE_DATE, PAGE_LEAF },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_LANGUAGE] = {
        .title      = "Language",
        .item_count = 1,
        .items      = { "1.1.1 ENGLISH" },
        .children   = { PAGE_LEAF },
        .parent     = PAGE_SYSTEM_SETTINGS,
    },

    [PAGE_DATE] = {
        .title      = "Date",
        .item_count = 2,
        .items      = { "1.2.1 Day Format",
                        "1.2.2 Day Settings" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SYSTEM_SETTINGS,
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
        .children   = { PAGE_LEAF,
                        PAGE_CALIBRATION,
                        PAGE_DIGITAL_FILTER,
                        PAGE_TEMP_MODE,
                        PAGE_LEAF,
                        PAGE_LEAF },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_CALIBRATION] = {
        .title      = "Calibration",
        .item_count = 2,
        .items      = { "2.2.1 Cal. 2 point",
                        "2.2.2 Cal. 3 point" },
        .children   = { PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_SENSOR_SETTINGS,
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

    /* ── Output Settings ── */
    [PAGE_OUTPUT_SETTINGS] = {
        .title      = "Output Settings",
        .item_count = 5,
        .items      = { "3.1 Relay-1",
                        "3.2 Relay-2",
                        "3.3 Relay-3",
                        "3.4 Current-1",
                        "3.5 ModBus RTU" },
        .children   = { PAGE_RELAY1,
                        PAGE_RELAY2,
                        PAGE_RELAY3,
                        PAGE_CURRENT1,
                        PAGE_MODBUS },
        .parent     = PAGE_MAIN_MENU,
    },

    [PAGE_RELAY1] = {
        .title      = "Relay-1",
        .item_count = 3,
        .items      = { "3.1.1 Relay-1 Mode",
                        "3.1.2 Relay-1 SP",
                        "3.1.3 Relay-1 Hys" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_OUTPUT_SETTINGS,
    },

    [PAGE_RELAY2] = {
        .title      = "Relay-2",
        .item_count = 3,
        .items      = { "3.2.1 Relay-2 Mode",
                        "3.2.2 Relay-2 SP",
                        "3.2.3 Relay-2 Hys" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_OUTPUT_SETTINGS,
    },

    [PAGE_RELAY3] = {
        .title      = "Relay-3",
        .item_count = 3,
        .items      = { "3.3.1 Relay-3 Mode",
                        "3.3.2 Relay-3 SP",
                        "3.3.3 Relay-3 Hys" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_OUTPUT_SETTINGS,
    },

    [PAGE_CURRENT1] = {
        .title      = "Current-1",
        .item_count = 5,
        .items      = { "3.4.1 Mode",
                        "3.4.2 Set.4ma",
                        "3.4.3 Set.20ma",
                        "3.4.4 Cal.4ma",
                        "3.4.5 Cal.20ma" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF,
                        PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_OUTPUT_SETTINGS,
    },

    [PAGE_MODBUS] = {
        .title      = "ModBus RTU",
        .item_count = 4,
        .items      = { "3.5.1 MB Address",
                        "3.5.2 Baud Rate",
                        "3.5.3 Parity Check",
                        "3.5.4 Stop Bits" },
        .children   = { PAGE_LEAF, PAGE_LEAF, PAGE_LEAF, PAGE_LEAF },
        .parent     = PAGE_OUTPUT_SETTINGS,
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
    ESP_LOGI(TAG_MENU, "Navigate -> page %d", (int)page);
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

/**
 * @brief  Đọc trạng thái GPIO và chống rung (debounce) cho các nút bấm
 *         Yêu cầu trạng thái ổn định trong 2 chu kỳ quét liên tiếp (100 ms)
 */
static void poll_and_debounce_buttons(void)
{
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

/**
 * @brief  Trả về true nếu nút vừa được nhấn (phát hiện sườn lên đã chống rung)
 */
static bool btn_edge(btn_idx_t idx)
{
    return s_btn_state[idx] && !s_btn_prev_state[idx];
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
            g_menu.in_menu = true;
            goto_page(PAGE_MAIN_MENU);
            g_lcd_need_redraw = true;
        }
        if (btn_edge(BTN_IDX_UP)) {
            if (g_lcd_contrast < 63) {
                g_lcd_contrast++;
                LCD_SetContrast(g_lcd_contrast);
                g_lcd_need_redraw = true;
                ESP_LOGI(TAG_MENU, "Contrast tang: %d", g_lcd_contrast);
            }
        }
        if (btn_edge(BTN_IDX_DOWN)) {
            if (g_lcd_contrast > 0) {
                g_lcd_contrast--;
                LCD_SetContrast(g_lcd_contrast);
                g_lcd_need_redraw = true;
                ESP_LOGI(TAG_MENU, "Contrast giam: %d", g_lcd_contrast);
            }
        }
        return;
    }

    /* ── Trong menu ── */
    const page_def_t *page = &s_pages[g_menu.current_page];

    /* ESC – quay lại trang cha */
    if (btn_edge(BTN_IDX_ESC)) {
        menu_page_t parent = page->parent;
        if (parent == PAGE_MEASUREMENT) {
            g_menu.in_menu      = false;
            g_menu.current_page = PAGE_MEASUREMENT;
            g_lcd_need_redraw   = true;
            ESP_LOGI(TAG_MENU, "Thoat menu -> do luong");
        } else {
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
        }
    }

    /* ENTER – vào mục con */
    if (btn_edge(BTN_IDX_ENTER)) {
        menu_page_t child = page->children[g_menu.selected];
        if (child != PAGE_LEAF && child != PAGE_COUNT) {
            goto_page(child);
            g_lcd_need_redraw = true;
        } else {
            /* Mục lá: có thể thêm xử lý giá trị ở đây sau */
            ESP_LOGI(TAG_MENU, "Leaf: %s", page->items[g_menu.selected]);
        }
    }

    /* RIGHT – dự phòng (bỏ qua) */
    btn_edge(BTN_IDX_RIGHT);
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
    const page_def_t *page = &s_pages[g_menu.current_page];
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

        if (idx == g_menu.selected) {
            /* Mục được chọn: nền đen, chữ trắng */
            LCD_FillRect(0, y, 128, ITEM_ROW_H, LCD_COLOR_ON);
            LCD_DrawString(4, y + 1, page->items[idx], LCD_COLOR_OFF);
            /* Mũi tên ► ở bên phải để chỉ mục đang chọn */
            if (page->children[idx] != PAGE_LEAF) {
                draw_arrow_right(121, y + 2, LCD_COLOR_OFF);
            }
        } else {
            /* Mục bình thường: nền trắng, chữ đen */
            LCD_DrawString(4, y + 1, page->items[idx], LCD_COLOR_ON);
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
