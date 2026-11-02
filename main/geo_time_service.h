#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool valid;
  char city[48];
  char region[48];
  char country[32];
  char timezone[48];
  double latitude;
  double longitude;
  int32_t utc_offset_sec;
} geo_location_t;

/**
 * @brief Inicializa o cliente SNTP para sincronização automática de relógio.
 */
void geo_time_init(void);

/**
 * @brief Detecta a localização geográfica e fuso horário pelo IP público (ip-api.com)
 *        e aplica o fuso horário automaticamente ao relógio do sistema.
 */
esp_err_t geo_time_fetch_location(geo_location_t *out_loc);

/**
 * @brief Retorna a última localização detectada.
 */
const geo_location_t *geo_time_get_location(void);

/**
 * @brief Verifica se o relógio já foi sincronizado via NTP (ano >= 2025).
 */
bool geo_time_is_synced(void);

/**
 * @brief Formata a hora atual ("HH:MM:SS") e data por extenso em português ("Seg, 02 Nov 2026").
 */
void geo_time_get_formatted(char *out_time_str, size_t time_len,
                            char *out_date_str, size_t date_len);

#ifdef __cplusplus
}
#endif
