/**
 * @file    Module_man_hinh_cam_bien.c
 * @brief   Driver màn hình đồ họa GMG12864-06D (ST7565) – ESP32 WROVER-E
 *
 *  Kết nối chân:
 *   CS   → GPIO 13   SCL → GPIO 12 (SPI CLK)
 *   RST  → Mạch ngoài SDA → GPIO 11 (SPI MOSI)
 *   A0   → GPIO 10 (DC)
 */

#include "screen_disp.h"
#include "ph_temp.h"
#include "screen_menu.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "GMG12864";

/* =====================================================================
 * Font 5×7 ASCII (space = 0x20 … '~' = 0x7E)
 * ===================================================================== */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* '}' */
    {0x10, 0x08, 0x08, 0x10, 0x08}, /* '~' */
};

/* =====================================================================
 * Framebuffer RAM (128 × 64 / 8 = 1024 bytes)
 * ===================================================================== */
static uint8_t fb[LCD_PAGES][LCD_WIDTH]
    __attribute__((aligned(4))); /* fb[page][col] */

static spi_device_handle_t s_spi;

uint8_t g_lcd_contrast = 5;
uint8_t g_lcd_resistor_ratio = 0;
volatile bool g_lcd_need_redraw = true;
static volatile float s_real_ph = 7.00f;
static volatile float s_real_temp = 25.0f;

/* =====================================================================
 * Giao tiếp SPI nội bộ
 * ===================================================================== */
static void lcd_send(const uint8_t *data, int len, bool is_data) {
  gpio_set_level(LCD_PIN_DC, is_data ? 1 : 0);
  gpio_set_level(LCD_PIN_CS, 0); // Kéo CS xuống LOW để chọn thiết bị

  esp_rom_delay_us(50);

  /*
   * Luôn dùng SPI_TRANS_USE_TXDATA (tối đa 4 byte/lần) để bỏ qua DMA.
   * DMA trên ESP32-S3 gặp lỗi cache coherency khi truyền buffer lớn,
   * gây ra hiện tượng sọc dọc trên màn hình LCD.
   * Gửi 4 byte/lần qua SPI FIFO trực tiếp: chậm hơn nhưng 100% chính xác.
   */
  // while (len > 0) {
  //     spi_transaction_t t = {0};
  //     int chunk = (len >= 4) ? 4 : len;
  //     t.flags = SPI_TRANS_USE_TXDATA;
  //     t.length = chunk * 8;
  //     memcpy(t.tx_data, data, chunk);
  //     ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
  //     data += chunk;
  //     len -= chunk;
  // }
  // Cấu hình transaction truyền trực tiếp từ mảng (Bỏ hoàn toàn vòng lặp while
  // chunk 4 byte)
  spi_transaction_t t = {
      .flags = 0, /* Không dùng cờ TXDATA để tránh lỗi đảo byte */
      .length = len * 8, /* Độ dài tính bằng số bit */
      .tx_buffer = data, /* Truyền trực tiếp từ con trỏ mảng */
      .rx_buffer = NULL};
  // Gửi toàn bộ gói tin (3 byte lệnh hoặc 132 byte dữ liệu) trong 1 lần duy
  // nhất
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));

  gpio_set_level(LCD_PIN_CS, 1); // Kéo CS lên HIGH sau khi truyền xong
}

static inline void lcd_cmd(uint8_t cmd) { lcd_send(&cmd, 1, false); }

static inline void lcd_dat(uint8_t dat) { lcd_send(&dat, 1, true); }

void LCD_Clear_DDRAM(void) {
  static uint8_t zero_buf[132] __attribute__((aligned(4)));
  memset(zero_buf, 0x00, sizeof(zero_buf));
  for (uint8_t page = 0; page < LCD_PAGES; page++) {
    /* Gộp 3 lệnh chọn Page và Cột thành 1 mảng để gửi đồng thời */
    uint8_t cmds[3] = {
        0xB0 | page, /* Set page address */
        0x10,        /* Set column address high nibble to 0 */
        0x00         /* Set column address low nibble to 0 */
    };
    lcd_send(cmds, 3, false);      // Gửi lệnh định vị
    lcd_send(zero_buf, 132, true); // Gửi dữ liệu xóa
  }
}

/* =====================================================================
 * Khởi tạo
 * ===================================================================== */
lcd_err_t LCD_Init(void) {
  ESP_LOGI(TAG, "Khoi tao SPI + GMG12864-06D (ST7565)...");

  /* --- Cấu hình GPIO DC và CS --- */
  gpio_config_t io_cfg = {
      .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_CS),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_cfg);
  gpio_set_level(LCD_PIN_CS, 1); // CS mặc định ở mức cao (idle HIGH)

  /* --- Khởi tạo SPI bus (HSPI) --- */
  spi_bus_config_t buscfg = {
      .miso_io_num = -1,
      .mosi_io_num = LCD_PIN_MOSI,
      .sclk_io_num = LCD_PIN_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 132 * LCD_PAGES + 8,
  };
  esp_err_t ret = spi_bus_initialize(
      LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO); // Kích hoạt lại DMA cho các gói
                                               // tin lớn (như Flush màn hình)
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize that bai: %s", esp_err_to_name(ret));
    return LCD_ERR;
  }

  /* --- Thêm thiết bị vào bus --- */
  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = LCD_SPI_FREQ_HZ,
      .mode = LCD_SPI_MODE,
      .spics_io_num = -1, // Điều khiển CS bằng phần mềm (Software CS)
      .queue_size = 7,
  };
  ret = spi_bus_add_device(LCD_SPI_HOST, &devcfg, &s_spi);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device that bai: %s", esp_err_to_name(ret));
    return LCD_ERR;
  }

  /* --- Reset phần cứng (đã đấu nối cứng bên ngoài mạch) --- */
  vTaskDelay(pdMS_TO_TICKS(100));

  /* --- Chuỗi lệnh khởi tạo ST7565R (Đồng bộ U8g2 NHD-C12864) --- */
  lcd_cmd(0xE2); /* Software Reset */
  vTaskDelay(pdMS_TO_TICKS(200));

  lcd_cmd(0xAE); /* Display OFF */
  lcd_cmd(0x40); /* Set Display Start Line = 0 */

  lcd_cmd(0xA1); /* ADC set to reverse (SEG direction) */
  lcd_cmd(0xC0); /* COM Output scan direction normal (0xC8 if reversed) */

  lcd_cmd(0xA6); /* Normal Display */
  lcd_cmd(0xA2); /* LCD Bias: 1/9 bias */

  /* Thiết lập điện áp tỷ số điện trở nội */
  lcd_cmd(0x20 | (g_lcd_resistor_ratio & 0x07)); /* Resistor Ratio (v0 voltage resistor ratio, 0x20 - 0x27) */

  /* Thiết lập tương phản (Contrast) */
  lcd_cmd(0x81);           /* Electronic Volume (Contrast command) */
  lcd_cmd(g_lcd_contrast); /* Sử dụng biến tương phản toàn cục */

  /* Bật toàn bộ các mạch nguồn (Booster + Regulator + Follower) */
  // lcd_cmd(0x2F);
  lcd_cmd(0x2C);
  vTaskDelay(pdMS_TO_TICKS(250));
  lcd_cmd(0x2E);
  vTaskDelay(pdMS_TO_TICKS(250));
  lcd_cmd(0x2F);
  vTaskDelay(pdMS_TO_TICKS(250));

  // /* Thiết lập tỷ số nhân áp (Booster Ratio Select) */
  // lcd_cmd(0xF8); /* Booster Ratio Command */
  // lcd_cmd(0x00); /* 4x Booster */

  lcd_cmd(0xA4); /* Gửi lệnh hiển thị bình thường từ bộ đệm RAM */

  /* --- Xóa sạch toàn bộ bộ nhớ DDRAM 132 cột của ST7565R --- */
  LCD_Clear_DDRAM();

  lcd_cmd(0xAF); /* Display ON */

  /* --- Xóa framebuffer và flush --- */
  LCD_Clear();
  LCD_Flush();

  /* ── BÀI TEST CHẨN ĐOÁN SPI ──
   * Tô đen toàn bộ màn hình 3 giây để kiểm tra SPI hoạt động đúng.
   * Nếu màn hình đen hoàn toàn (không sọc) → SPI OK.
   * Nếu vẫn sọc → SPI bị lỗi hoặc DMA có vấn đề.
   * Sau khi xác nhận xong, có thể comment đoạn này lại.
   */
  // memset(fb, 0xFF, sizeof(fb));   /* Điền toàn bộ framebuffer = pixel ON
  // (đen) */ LCD_Flush(); ESP_LOGI(TAG, "=== SPI DIAGNOSTIC: Man hinh phai den
  // HOAN TOAN trong 3 giay ==="); vTaskDelay(pdMS_TO_TICKS(3000)); LCD_Clear();
  // /* Xóa trắng lại */ LCD_Flush();

  ESP_LOGI(TAG, "ST7565R san sang!");
  return LCD_OK;
}

/* =====================================================================
 * Điều khiển hiển thị
 * ===================================================================== */
void LCD_DisplayOn(void) { lcd_cmd(0xAF); }
void LCD_DisplayOff(void) { lcd_cmd(0xAE); }

void LCD_SetContrast(uint8_t val) {
  if (val > 0x3F)
    val = 0x3F;
  lcd_cmd(0x81);
  lcd_cmd(val);
}

void LCD_SetResistorRatio(uint8_t val) {
  if (val > 7)
    val = 7;
  lcd_cmd(0x20 | val);
}

/* =====================================================================
 * Framebuffer
 * ===================================================================== */
void LCD_Clear(void) { memset(fb, 0x00, sizeof(fb)); }

void LCD_Flush(void) {
  /*
   * ST7565R có 132 cột DDRAM nhưng màn hình chỉ hiển thị 128 cột.
   * LCD_COLUMN_OFFSET = 4 → pixel hiển thị bắt đầu từ cột DDRAM thứ 4.
   * Để triệt để loại bỏ rác DDRAM (sọc dọc), ta ghi đè toàn bộ 132 cột:
   *   - Cột 0..3   : padding 0x00 (vùng ẩn trước offset)
   *   - Cột 4..131 : dữ liệu framebuffer (128 byte)
   *
   * Buffer phải static + aligned(4) để đảm bảo tương thích GDMA trên ESP32-S3.
   */
  static uint8_t row_buf[132] __attribute__((aligned(4)));

  for (uint8_t page = 0; page < LCD_PAGES; page++) {
    /* Gộp 3 lệnh chọn Page và Cột thành 1 mảng để gửi đồng thời */
    uint8_t cmds[3] = {
        0xB0 | page, /* Set page address */
        0x10,        /* Set column address high nibble to 0 */
        0x00         /* Set column address low nibble to 0 */
    };
    /* Gửi 3 lệnh định vị trong 1 transaction */
    lcd_send(cmds, 3, false);
    /* Điền zero cho vùng offset đầu */
    memset(row_buf, 0x00, LCD_COLUMN_OFFSET);
    /* Copy framebuffer vào sau vùng offset */
    memcpy(row_buf + LCD_COLUMN_OFFSET, fb[page], LCD_WIDTH);
    /* Gửi toàn bộ 132 byte trong 1 transaction SPI */
    lcd_send(row_buf, 132, true);
  }
}

/* =====================================================================
 * Vẽ pixel
 * ===================================================================== */
void LCD_DrawPixel(uint8_t x, uint8_t y, uint8_t color) {
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
    return;
  if (color) {
    fb[y >> 3][x] |= (1 << (y & 7));
  } else {
    fb[y >> 3][x] &= ~(1 << (y & 7));
  }
}

/* =====================================================================
 * Đường thẳng
 * ===================================================================== */
void LCD_DrawHLine(uint8_t x, uint8_t y, uint8_t w, uint8_t color) {
  for (uint8_t i = 0; i < w && (x + i) < LCD_WIDTH; i++)
    LCD_DrawPixel(x + i, y, color);
}

void LCD_DrawVLine(uint8_t x, uint8_t y, uint8_t h, uint8_t color) {
  for (uint8_t i = 0; i < h && (y + i) < LCD_HEIGHT; i++)
    LCD_DrawPixel(x, y + i, color);
}

void LCD_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1,
                  uint8_t color) {
  int dx = abs((int)x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs((int)y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (1) {
    LCD_DrawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* =====================================================================
 * Hình chữ nhật
 * ===================================================================== */
void LCD_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t color) {
  LCD_DrawHLine(x, y, w, color);
  LCD_DrawHLine(x, y + h - 1, w, color);
  LCD_DrawVLine(x, y, h, color);
  LCD_DrawVLine(x + w - 1, y, h, color);
}

void LCD_FillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t color) {
  for (uint8_t row = 0; row < h && (y + row) < LCD_HEIGHT; row++)
    LCD_DrawHLine(x, y + row, w, color);
}

/* =====================================================================
 * Hình tròn (Midpoint Circle)
 * ===================================================================== */
void LCD_DrawCircle(uint8_t cx, uint8_t cy, uint8_t r, uint8_t color) {
  int x = 0, y = r, d = 1 - r;
  while (x <= y) {
    LCD_DrawPixel(cx + x, cy + y, color);
    LCD_DrawPixel(cx - x, cy + y, color);
    LCD_DrawPixel(cx + x, cy - y, color);
    LCD_DrawPixel(cx - x, cy - y, color);
    LCD_DrawPixel(cx + y, cy + x, color);
    LCD_DrawPixel(cx - y, cy + x, color);
    LCD_DrawPixel(cx + y, cy - x, color);
    LCD_DrawPixel(cx - y, cy - x, color);
    if (d < 0)
      d += 2 * x + 3;
    else {
      d += 2 * (x - y) + 5;
      y--;
    }
    x++;
  }
}

/* =====================================================================
 * Ký tự / chuỗi (font 5×7)
 * ===================================================================== */
void LCD_DrawChar(uint8_t x, uint8_t y, char c, uint8_t color) {
  if (c < 0x20 || c > 0x7E)
    c = '?';
  const uint8_t *col_data = font5x7[c - 0x20];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t line = col_data[col];
    for (uint8_t row = 0; row < 7; row++) {
      if (line & (1 << row))
        LCD_DrawPixel(x + col, y + row, color);
      else
        LCD_DrawPixel(x + col, y + row, color ^ 1);
    }
  }
  /* Cột trống giữa các ký tự */
  for (uint8_t row = 0; row < 7; row++)
    LCD_DrawPixel(x + 5, y + row, color ^ 1);
}

void LCD_DrawString(uint8_t x, uint8_t y, const char *str, uint8_t color) {
  uint8_t cx = x;
  while (*str) {
    if (*str == '\n') {
      cx = x;
      y += 8;
    } else {
      if (cx + 6 > LCD_WIDTH) {
        cx = x;
        y += 8;
      }
      LCD_DrawChar(cx, y, *str, color);
      cx += 6;
    }
    str++;
  }
}

void LCD_DrawCharScaled(uint8_t x, uint8_t y, char c, uint8_t scale_x,
                        uint8_t scale_y, uint8_t color) {
  if (c < 0x20 || c > 0x7E)
    c = '?';
  const uint8_t *col_data = font5x7[c - 0x20];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t line = col_data[col];
    for (uint8_t row = 0; row < 7; row++) {
      uint8_t px_color = (line & (1 << row)) ? color : (color ^ 1);
      for (uint8_t sx = 0; sx < scale_x; sx++) {
        for (uint8_t sy = 0; sy < scale_y; sy++) {
          LCD_DrawPixel(x + col * scale_x + sx, y + row * scale_y + sy,
                        px_color);
        }
      }
    }
  }
  /* Cột trống giữa các ký tự */
  for (uint8_t row = 0; row < 7; row++) {
    for (uint8_t sx = 0; sx < scale_x; sx++) {
      for (uint8_t sy = 0; sy < scale_y; sy++) {
        LCD_DrawPixel(x + 5 * scale_x + sx, y + row * scale_y + sy, color ^ 1);
      }
    }
  }
}

void LCD_DrawStringScaled(uint8_t x, uint8_t y, const char *str,
                          uint8_t scale_x, uint8_t scale_y, uint8_t color) {
  uint8_t cx = x;
  while (*str) {
    if (*str == '\n') {
      cx = x;
      y += 8 * scale_y;
    } else {
      if (cx + 6 * scale_x > LCD_WIDTH) {
        cx = x;
        y += 8 * scale_y;
      }
      LCD_DrawCharScaled(cx, y, *str, scale_x, scale_y, color);
      cx += 6 * scale_x;
    }
    str++;
  }
}

void LCD_DrawInt(uint8_t x, uint8_t y, int32_t val, uint8_t color) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%ld", (long)val);
  LCD_DrawString(x, y, buf, color);
}

/* =====================================================================
 * Bitmap 1-bit (MSB trái, row-major)
 * ===================================================================== */
void LCD_DrawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                    const uint8_t *bmp) {
  uint16_t byte_idx = 0;
  for (uint8_t row = 0; row < h; row++) {
    for (uint8_t col = 0; col < w; col++) {
      uint8_t bit_idx = col % 8;
      if (bit_idx == 0 && col != 0)
        byte_idx++;
      uint8_t pix = (bmp[byte_idx] >> (7 - bit_idx)) & 1;
      LCD_DrawPixel(x + col, y + row, pix ? LCD_COLOR_ON : LCD_COLOR_OFF);
    }
    byte_idx++;
  }
}

/* =====================================================================
 * FreeRTOS Task demo với chế độ tự chẩn đoán (Diagnostic)
 * ===================================================================== */
static void lcd_demo_task(void *arg) {
  ESP_LOGI(TAG, "VE GIAO DIEN APURE A10 + MENU");

  /* ── Khởi tạo nút bấm ── */
  menu_init();

  /* ── Biến trạng thái màn hình đo lường ── */
  float ph_val = 7.00f;
  float do_val = 0.00f;
  float temp_val = 25.3f;
  bool do_valid = false;
  int tick = 0; /* 0..19  → 20×50 ms = 1 giây    */

  char ph_str[10] = "7.00";
  char do_str[12] = "0.00";
  char do_sat_str[16] = "Sat: --.-%";
  char temp_str[12] = "25.3 C";
  char time_str[32] = "--:-- --";
  char date_str[32] = "2026-05-22";

  while (1) {
    /* ── 1. Xử lý nút bấm ── */
    menu_handle_buttons();

    /* ── 2. Nếu đang ở menu → render menu và lặp lại ── */
    if (g_menu.in_menu) {
      menu_render();
      vTaskDelay(pdMS_TO_TICKS(50));
      tick = 0;                 /* reset tick khi vừa thoát menu */
      g_lcd_need_redraw = true; /* Vẽ lại màn hình đo lường khi thoát menu */
      continue;
    }

    /* ── 3. Màn hình đo lường ── */

    /* Cập nhật giá trị mỗi 1 giây (tick=0) */
    if (tick == 0) {
      PH_Temp_Sensor_Status_t status = Get_Sensor_Status();
      ph_val = status.ph;
      do_val = status.do_mg_l;
      do_valid = status.do_valid;

      // Chế độ pH dùng nhiệt độ pH. Chế độ DO và song song dùng nhiệt độ DO
      // (nếu lỗi dùng pH làm fallback).
      if (g_display_mode == DISP_MODE_PH) {
        temp_val = status.temperature;
      } else {
        temp_val = status.do_valid ? status.do_temp_c : status.temperature;
      }

      /* Format chuỗi */
      snprintf(ph_str, sizeof(ph_str), "%.2f", ph_val);
      if (do_valid) {
        snprintf(do_str, sizeof(do_str), "%.2f", do_val);
      } else {
        snprintf(do_str, sizeof(do_str), "N/A");
      }
      if (g_sys_lang == LANG_VI) {
        if (do_valid) {
          snprintf(do_sat_str, sizeof(do_sat_str), "Oxy: %.1f%%", status.do_saturation_pct);
        } else {
          snprintf(do_sat_str, sizeof(do_sat_str), "Oxy: N/A");
        }
      } else {
        if (do_valid) {
          snprintf(do_sat_str, sizeof(do_sat_str), "Sat: %.1f%%", status.do_saturation_pct);
        } else {
          snprintf(do_sat_str, sizeof(do_sat_str), "Sat: N/A");
        }
      }
    //   snprintf(temp_str, sizeof(temp_str), "%.1f C", temp_val);
    bool is_f = (g_temp_mode == TEMP_MODE_ATC_F || g_temp_mode == TEMP_MODE_MTC_F);
        if (is_f) {
            float temp_val_f = temp_val * 1.8f + 32.0f;
            snprintf(temp_str, sizeof(temp_str), "%.1f F", temp_val_f);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%.1f C", temp_val);
        }

      // Đọc thời gian thực tế từ hệ thống
      time_t now;
      struct tm timeInfo;
      time(&now);
      localtime_r(&now, &timeInfo);

      // Nếu thời gian đã được đồng bộ (năm lớn hơn hoặc bằng 2000, tương ứng
      // tm_year >= 100)
      if (timeInfo.tm_year >= 100) {
        strftime(time_str, sizeof(time_str), "%I:%M %p", &timeInfo);
        if (g_date_format == DATE_FORMAT_DD_MM_YYYY) {
          strftime(date_str, sizeof(date_str), "%d-%m-%Y", &timeInfo);
        } else if (g_date_format == DATE_FORMAT_MM_DD_YYYY) {
          strftime(date_str, sizeof(date_str), "%m-%d-%Y", &timeInfo);
        } else {
          strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeInfo);
        }
      } else {
        snprintf(time_str, sizeof(time_str), "--:-- --");
        if (g_date_format == DATE_FORMAT_DD_MM_YYYY) {
          snprintf(date_str, sizeof(date_str), "22-05-2026");
        } else if (g_date_format == DATE_FORMAT_MM_DD_YYYY) {
          snprintf(date_str, sizeof(date_str), "05-22-2026");
        } else {
          snprintf(date_str, sizeof(date_str), "2026-05-22");
        }
      }

      static uint8_t log_cnt = 0;
      if (++log_cnt >=
          5) { // tick == 0 xảy ra mỗi 2 giây, do đó 5 lần là 10 giây
        log_cnt = 0;
        ESP_LOGI(TAG, "Thoi gian hien thi tren LCD: %s %s", date_str, time_str);
      }

      g_lcd_need_redraw = true;
    }
    tick = (tick + 1) % 20;

    if (g_lcd_need_redraw) {
      /* Vẽ màn hình đo lường
       * ─────────────────────────────────────────────────────────────
       *  y= 0..11 : Top bar    (nền đen, chữ trắng)
       *  y=12..43 : Giá trị pH lớn / DO lớn / Song song
       *  y=44..63 : Bottom bar (nền đen, chữ trắng)
       * ─────────────────────────────────────────────────────────────
       */
      LCD_Clear();

      /* Top bar */
      LCD_FillRect(0, 0, 128, 12, LCD_COLOR_ON);
      if (g_sys_lang == LANG_VI) {
        LCD_DrawString(4, 2, "Dang Do...", LCD_COLOR_OFF);
      } else {
        LCD_DrawString(4, 2, "Measuring", LCD_COLOR_OFF);
      }
      LCD_DrawString(80, 2, "RS485", LCD_COLOR_OFF);

      /* Đường viền dọc vùng giữa (2px mỗi bên) */
      LCD_DrawVLine(0, 12, 32, LCD_COLOR_ON);
      LCD_DrawVLine(1, 12, 32, LCD_COLOR_ON);
      LCD_DrawVLine(126, 12, 32, LCD_COLOR_ON);
      LCD_DrawVLine(127, 12, 32, LCD_COLOR_ON);

      if (g_display_mode == DISP_MODE_PH) {
        /* Giá trị pH lớn (scale 3×4) */
        uint8_t ph_width = (uint8_t)(strlen(ph_str) * 6 * 3);
        uint8_t ph_x = (LCD_WIDTH - ph_width) / 2 - 5;
        LCD_DrawStringScaled(ph_x, 14, ph_str, 3, 4, LCD_COLOR_ON);
        LCD_DrawString(ph_x + ph_width + 2, 34, "pH", LCD_COLOR_ON);
      } else if (g_display_mode == DISP_MODE_DO) {
        /* Giá trị DO lớn (scale 3×4) */
        uint8_t do_width = (uint8_t)(strlen(do_str) * 6 * 3);
        uint8_t do_x = (LCD_WIDTH - do_width) / 2 - 12;
        LCD_DrawStringScaled(do_x, 14, do_str, 3, 4, LCD_COLOR_ON);
        LCD_DrawString(do_x + do_width + 2, 34, "mg/L", LCD_COLOR_ON);
      } else { // DISP_MODE_DUAL
        /* Hiển thị song song pH và DO (scale 2×2) */
        char dual_ph[32];
        char dual_do[32];
        snprintf(dual_ph, sizeof(dual_ph), "pH: %s", ph_str);
        snprintf(dual_do, sizeof(dual_do), "DO: %s mg/L", do_str);

        LCD_DrawStringScaled(16, 14, dual_ph, 2, 2, LCD_COLOR_ON);
        LCD_DrawStringScaled(16, 30, dual_do, 2, 2, LCD_COLOR_ON);
      }

      /* Bottom bar */
      LCD_FillRect(0, 44, 128, 20, LCD_COLOR_ON);
      if (g_display_mode == DISP_MODE_PH) {
        if (g_sys_lang == LANG_VI) {
          LCD_DrawString(4, 45, "Dien Cuc pH", LCD_COLOR_OFF);
        } else {
          LCD_DrawString(4, 45, "pH Electrode", LCD_COLOR_OFF);
        }
      } else { // DISP_MODE_DO hoặc DISP_MODE_DUAL
        LCD_DrawString(4, 45, do_sat_str, LCD_COLOR_OFF);
      }
    //   /* Ký hiệu độ (2×2 px) */
    //   LCD_DrawPixel(113, 45, LCD_COLOR_OFF);
    //   LCD_DrawPixel(114, 45, LCD_COLOR_OFF);
    //   LCD_DrawPixel(113, 46, LCD_COLOR_OFF);
    //   LCD_DrawPixel(114, 46, LCD_COLOR_OFF);
    /* Ký hiệu độ (2×2 px) đặt động trước ký tự đơn vị (C hoặc F) */
    LCD_DrawString(88, 45, temp_str, LCD_COLOR_OFF);
    int unit_idx = 0;
    while (temp_str[unit_idx] != '\0' && temp_str[unit_idx] != 'C' &&
           temp_str[unit_idx] != 'F') {
      unit_idx++;
    }
    int degree_x = 88 + unit_idx * 6 - 5;
    LCD_DrawPixel(degree_x, 45, LCD_COLOR_OFF);
    LCD_DrawPixel(degree_x + 1, 45, LCD_COLOR_OFF);
    LCD_DrawPixel(degree_x, 46, LCD_COLOR_OFF);
    LCD_DrawPixel(degree_x + 1, 46, LCD_COLOR_OFF);
        LCD_DrawString(4, 54, date_str, LCD_COLOR_OFF);
        LCD_DrawString(76, 54, time_str, LCD_COLOR_OFF);

      LCD_Flush();
      g_lcd_need_redraw = false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void LCD_Start_Task(void) {
  xTaskCreatePinnedToCore(lcd_demo_task, "LCD_Task", 4096, NULL, 5, NULL, 1);
}

void screen_update_values(float ph, float temp) {
  s_real_ph = ph;
  s_real_temp = temp;
  g_lcd_need_redraw =
      true; // Báo hiệu để task vẽ lại màn hình ngay khi có dữ liệu mới
}

void LCD_GetFramebuffer(uint8_t *dest) {
  if (dest != NULL) {
    memcpy(dest, fb, sizeof(fb));
  }
}
