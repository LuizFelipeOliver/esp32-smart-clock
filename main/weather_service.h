#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_FORECAST_DAYS 7

typedef struct {
  char date[12];       // "YYYY-MM-DD"
  char weekday[6];     // "Seg", "Ter", "Qua", etc.
  float temp_min;
  float temp_max;
  int rain_prob;       // 0..100 %
  int weather_code;    // Código WMO
  const char *short_desc; // "Sol", "Nublado", "Chuva", etc.
} weather_day_t;

typedef struct {
  bool valid;
  float current_temp;
  int current_humidity;
  float current_wind_kmh;
  int current_code;
  const char *current_desc;
  weather_day_t days[WEATHER_FORECAST_DAYS];
} weather_data_t;

/**
 * @brief Consulta a previsão de 7 dias e clima atual na API Open-Meteo (gratuita, sem chave).
 */
esp_err_t weather_service_fetch(double latitude, double longitude,
                                weather_data_t *out_data);

/**
 * @brief Retorna os últimos dados meteorológicos obtidos.
 */
const weather_data_t *weather_service_get_data(void);

/**
 * @brief Retorna a descrição completa em português de um código WMO.
 */
const char *weather_code_to_desc(int code);

/**
 * @brief Retorna a descrição curta (para os cards da semana) de um código WMO.
 */
const char *weather_code_to_short(int code);

#ifdef __cplusplus
}
#endif
