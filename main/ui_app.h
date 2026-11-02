#pragma once

#include "geo_time_service.h"
#include "weather_service.h"
#include "wifi_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cria as telas LVGL (Dashboard de Relógio/Clima + Configuração de Wi-Fi com Teclado Touch).
 */
void ui_app_init(void);

/**
 * @brief Atualiza o relógio, data e status de conexão na tela (chamado a cada 1s).
 */
void ui_app_update_clock_and_status(void);

/**
 * @brief Atualiza os dados de localização e previsão de 7 dias na tela.
 */
void ui_app_update_weather(const geo_location_t *loc,
                           const weather_data_t *weather);

/**
 * @brief Abre diretamente a tela de configuração de Wi-Fi.
 */
void ui_app_show_wifi_config(void);

#ifdef __cplusplus
}
#endif
