#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolução do display ESP32-2432S028R (Modo Paisagem / Landscape)
#define CYD_LCD_H_RES 320
#define CYD_LCD_V_RES 240

// Pinagem do Display TFT ILI9341 (SPI2 / HSPI)
#define CYD_LCD_SPI_HOST SPI2_HOST
#define CYD_LCD_PIN_SCLK 14
#define CYD_LCD_PIN_MOSI 13
#define CYD_LCD_PIN_MISO 12
#define CYD_LCD_PIN_CS   15
#define CYD_LCD_PIN_DC   2
#define CYD_LCD_PIN_RST  -1
#define CYD_LCD_PIN_BL   21

// Pinagem do Touch Resistivo XPT2046 (SPI3 / VSPI)
#define CYD_TOUCH_SPI_HOST SPI3_HOST
#define CYD_TOUCH_PIN_CLK  25
#define CYD_TOUCH_PIN_MOSI 32
#define CYD_TOUCH_PIN_MISO 39
#define CYD_TOUCH_PIN_CS   33
#define CYD_TOUCH_PIN_IRQ  36

// Pinagem do LED RGB integrado (Ativo em nível BAIXO)
#define CYD_LED_PIN_R 4
#define CYD_LED_PIN_G 16
#define CYD_LED_PIN_B 17

/**
 * @brief Inicializa o hardware da placa ESP32-2432S028R (Display ILI9341, Touch XPT2046, Backlight, LED RGB e LVGL)
 */
esp_err_t board_cyd_init(void);

/**
 * @brief Adquire o mutex do LVGL antes de chamar funções da UI fora da task do LVGL
 */
bool board_cyd_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief Libera o mutex do LVGL
 */
void board_cyd_lvgl_unlock(void);

/**
 * @brief Define a cor do LED RGB traseiro da placa (true = aceso, false = apagado)
 */
void board_cyd_set_rgb_led(bool r, bool g, bool b);

#ifdef __cplusplus
}
#endif
