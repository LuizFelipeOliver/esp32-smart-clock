#include "board_cyd.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "board_cyd";

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static spi_device_handle_t s_touch_spi = NULL;
static lv_display_t *s_lvgl_disp = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;

// Buffer de desenho para o LVGL (1/10 da tela em DMA RAM = 320 * 24 * 2 = 15.360 bytes)
#define DRAW_BUF_LINES 24

typedef struct {
  uint8_t cmd;
  uint8_t data[16];
  uint8_t data_bytes;
  uint16_t delay_ms;
} ili9341_init_cmd_t;

// Sequência padrão de inicialização do controlador ILI9341 (320x240 Landscape)
static const ili9341_init_cmd_t s_ili9341_init_cmds[] = {
    {0x01, {0}, 0, 120}, // Software reset
    {0xCF, {0x00, 0xC1, 0x30}, 3, 0},
    {0xED, {0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, {0x85, 0x00, 0x78}, 3, 0},
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, {0x20}, 1, 0},
    {0xEA, {0x00, 0x00}, 2, 0},
    {0xC0, {0x23}, 1, 0},       // Power control 1
    {0xC1, {0x10}, 1, 0},       // Power control 2
    {0xC5, {0x3E, 0x28}, 2, 0}, // VCOM control 1
    {0xC7, {0x86}, 1, 0},       // VCOM control 2
    {0x36, {0x28}, 1, 0},       // MADCTL: MV=1, BGR=1 (Landscape 320x240)
    {0x3A, {0x55}, 1, 0},       // COLMOD: 16-bit RGB565
    {0xB1, {0x00, 0x18}, 2, 0}, // Frame rate control
    {0xB6, {0x08, 0x82, 0x27}, 3, 0},
    {0xF2, {0x00}, 1, 0}, // 3Gamma Function Disable
    {0x26, {0x01}, 1, 0}, // Gamma curve selected
    {0xE0,
     {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03,
      0x0E, 0x09, 0x00},
     15, 0},
    {0xE1,
     {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C,
      0x31, 0x36, 0x0F},
     15, 0},
    {0x11, {0}, 0, 120}, // Exit Sleep
    {0x29, {0}, 0, 20},  // Display ON
};

// Callback chamado pelo DMA SPI quando termina de enviar os pixels ao ILI9341
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx) {
  if (s_lvgl_disp) {
    lv_display_flush_ready(s_lvgl_disp);
  }
  return false;
}

// Callback do LVGL 9 para enviar uma região da tela para o ILI9341
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map) {
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;
  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  size_t pixel_count = (x2 - x1 + 1) * (y2 - y1 + 1);
  // ILI9341 via SPI espera RGB565 em ordem Big-Endian
  lv_draw_sw_rgb565_swap(px_map, pixel_count);

  uint8_t col_data[4] = {(uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
                         (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)};
  uint8_t row_data[4] = {(uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
                         (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)};

  esp_lcd_panel_io_tx_param(s_lcd_io, 0x2A, col_data, 4);
  esp_lcd_panel_io_tx_param(s_lcd_io, 0x2B, row_data, 4);
  esp_lcd_panel_io_tx_color(s_lcd_io, 0x2C, px_map, pixel_count * 2);
}

// Leitura de canal de 12 bits do controlador Touch XPT2046
static uint16_t xpt2046_read_cmd(uint8_t cmd) {
  if (!s_touch_spi) {
    return 0;
  }
  uint8_t tx_data[3] = {cmd, 0x00, 0x00};
  uint8_t rx_data[3] = {0};

  spi_transaction_t t = {};
  t.length = 24;
  t.tx_buffer = tx_data;
  t.rx_buffer = rx_data;

  if (spi_device_polling_transmit(s_touch_spi, &t) != ESP_OK) {
    return 0;
  }
  return ((uint16_t)rx_data[1] << 8 | rx_data[2]) >> 3;
}

// Callback de leitura do Touch XPT2046 para o LVGL 9
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  // No CYD, o pino IRQ (GPIO 36) fica em nível BAIXO quando a tela é pressionada
  if (gpio_get_level((gpio_num_t)CYD_TOUCH_PIN_IRQ) != 0) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  uint16_t z1 = xpt2046_read_cmd(0xB1); // Pressão Z1
  if (z1 < 150 || z1 > 4000) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  // Faz média de 3 leituras para estabilidade
  uint32_t sum_x = 0;
  uint32_t sum_y = 0;
  for (int i = 0; i < 3; i++) {
    sum_x += xpt2046_read_cmd(0xD0);
    sum_y += xpt2046_read_cmd(0x90);
  }
  uint16_t raw_x = sum_x / 3;
  uint16_t raw_y = sum_y / 3;

  // Calibração padrão para ESP32-2432S028R em modo Landscape (320x240)
  const int32_t cal_x_min = 240;
  const int32_t cal_x_max = 3800;
  const int32_t cal_y_min = 240;
  const int32_t cal_y_max = 3800;

  int32_t x = ((int32_t)raw_x - cal_x_min) * CYD_LCD_H_RES /
              (cal_x_max - cal_x_min);
  int32_t y = ((int32_t)raw_y - cal_y_min) * CYD_LCD_V_RES /
              (cal_y_max - cal_y_min);

  if (x < 0)
    x = 0;
  if (x >= CYD_LCD_H_RES)
    x = CYD_LCD_H_RES - 1;
  if (y < 0)
    y = 0;
  if (y >= CYD_LCD_V_RES)
    y = CYD_LCD_V_RES - 1;

  data->point.x = x;
  data->point.y = y;
  data->state = LV_INDEV_STATE_PRESSED;
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(2); }

static void lvgl_task(void *arg) {
  ESP_LOGI(TAG, "Task gráfica LVGL iniciada");
  while (true) {
    if (board_cyd_lvgl_lock(100)) {
      lv_timer_handler();
      board_cyd_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void board_cyd_set_rgb_led(bool r, bool g, bool b) {
  // LED RGB do CYD é ativo em nível BAIXO (0 = aceso, 1 = apagado)
  gpio_set_level((gpio_num_t)CYD_LED_PIN_R, r ? 0 : 1);
  gpio_set_level((gpio_num_t)CYD_LED_PIN_G, g ? 0 : 1);
  gpio_set_level((gpio_num_t)CYD_LED_PIN_B, b ? 0 : 1);
}

bool board_cyd_lvgl_lock(uint32_t timeout_ms) {
  if (!s_lvgl_mutex) {
    return false;
  }
  TickType_t ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(s_lvgl_mutex, ticks) == pdTRUE;
}

void board_cyd_lvgl_unlock(void) {
  if (s_lvgl_mutex) {
    xSemaphoreGive(s_lvgl_mutex);
  }
}

esp_err_t board_cyd_init(void) {
  ESP_LOGI(TAG, "Inicializando hardware ESP32-2432S028R (CYD)...");

  // 1. Configura GPIOs do Backlight e LED RGB
  gpio_config_t out_conf = {};
  out_conf.pin_bit_mask = (1ULL << CYD_LCD_PIN_BL) | (1ULL << CYD_LED_PIN_R) |
                          (1ULL << CYD_LED_PIN_G) | (1ULL << CYD_LED_PIN_B);
  out_conf.mode = GPIO_MODE_OUTPUT;
  out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&out_conf);

  // Desliga o LED RGB inicialmente e liga o Backlight da tela
  board_cyd_set_rgb_led(false, false, false);
  gpio_set_level((gpio_num_t)CYD_LCD_PIN_BL, 1);

  // 2. Inicializa barramento SPI2 para o Display ILI9341
  spi_bus_config_t lcd_buscfg = {};
  lcd_buscfg.sclk_io_num = CYD_LCD_PIN_SCLK;
  lcd_buscfg.mosi_io_num = CYD_LCD_PIN_MOSI;
  lcd_buscfg.miso_io_num = CYD_LCD_PIN_MISO;
  lcd_buscfg.quadwp_io_num = -1;
  lcd_buscfg.quadhd_io_num = -1;
  lcd_buscfg.max_transfer_sz = CYD_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);

  ESP_RETURN_ON_ERROR(
      spi_bus_initialize(CYD_LCD_SPI_HOST, &lcd_buscfg, SPI_DMA_CH_AUTO), TAG,
      "Falha ao inicializar SPI do display");

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = (gpio_num_t)CYD_LCD_PIN_DC;
  io_config.cs_gpio_num = (gpio_num_t)CYD_LCD_PIN_CS;
  io_config.pclk_hz = 40 * 1000 * 1000; // 40 MHz SPI clock
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.spi_mode = 0;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = notify_lvgl_flush_ready;

  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                          (esp_lcd_spi_bus_handle_t)CYD_LCD_SPI_HOST,
                          &io_config, &s_lcd_io),
                      TAG, "Falha ao criar IO SPI do LCD");

  // Envia sequência de inicialização do ILI9341
  for (size_t i = 0;
       i < sizeof(s_ili9341_init_cmds) / sizeof(s_ili9341_init_cmds[0]); i++) {
    esp_lcd_panel_io_tx_param(s_lcd_io, s_ili9341_init_cmds[i].cmd,
                              s_ili9341_init_cmds[i].data,
                              s_ili9341_init_cmds[i].data_bytes);
    if (s_ili9341_init_cmds[i].delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(s_ili9341_init_cmds[i].delay_ms));
    }
  }

  // 3. Inicializa barramento SPI3 para o Touch XPT2046
  gpio_config_t irq_conf = {};
  irq_conf.pin_bit_mask = (1ULL << CYD_TOUCH_PIN_IRQ);
  irq_conf.mode = GPIO_MODE_INPUT;
  irq_conf.pull_up_en = GPIO_PULLUP_DISABLE; // GPIO 36 não possui pull-up interno
  gpio_config(&irq_conf);

  spi_bus_config_t touch_buscfg = {};
  touch_buscfg.sclk_io_num = CYD_TOUCH_PIN_CLK;
  touch_buscfg.mosi_io_num = CYD_TOUCH_PIN_MOSI;
  touch_buscfg.miso_io_num = CYD_TOUCH_PIN_MISO;
  touch_buscfg.quadwp_io_num = -1;
  touch_buscfg.quadhd_io_num = -1;
  touch_buscfg.max_transfer_sz = 32;

  ESP_RETURN_ON_ERROR(
      spi_bus_initialize(CYD_TOUCH_SPI_HOST, &touch_buscfg, SPI_DMA_DISABLED),
      TAG, "Falha ao inicializar SPI do touch");

  spi_device_interface_config_t touch_devcfg = {};
  touch_devcfg.clock_speed_hz = 2 * 1000 * 1000; // 2 MHz para XPT2046
  touch_devcfg.mode = 0;
  touch_devcfg.spics_io_num = CYD_TOUCH_PIN_CS;
  touch_devcfg.queue_size = 1;

  ESP_RETURN_ON_ERROR(
      spi_bus_add_device(CYD_TOUCH_SPI_HOST, &touch_devcfg, &s_touch_spi), TAG,
      "Falha ao adicionar dispositivo XPT2046");

  // 4. Inicializa LVGL 9
  lv_init();
  s_lvgl_mutex = xSemaphoreCreateMutex();

  s_lvgl_disp = lv_display_create(CYD_LCD_H_RES, CYD_LCD_V_RES);
  lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);

  size_t buf_size = CYD_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
  void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(s_lvgl_disp, buf1, buf2, buf_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

  // Timer de tick de 2ms para o LVGL
  const esp_timer_create_args_t tick_timer_args = {
      .callback = &lvgl_tick_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lvgl_tick",
      .skip_unhandled_events = true,
  };
  esp_timer_handle_t tick_timer = NULL;
  ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG,
                      "Falha ao criar timer do LVGL");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 2000), TAG,
                      "Falha ao iniciar timer do LVGL");

  xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 8192, NULL, 5, NULL, 1);

  ESP_LOGI(TAG, "Hardware CYD + LVGL inicializados com sucesso!");
  return ESP_OK;
}
