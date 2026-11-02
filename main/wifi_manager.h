#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MAX_SCAN_RESULTS 16

typedef enum {
  WIFI_STATE_DISCONNECTED = 0,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_ERROR
} wifi_conn_state_t;

typedef struct {
  char ssid[33];
  int8_t rssi;
  bool secure;
} wifi_ap_item_t;

/**
 * @brief Inicializa o NVS e o subsistema Wi-Fi em modo Station.
 *        Se houver credenciais salvas no NVS, tenta conectar automaticamente.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Verifica se existem credenciais salvas no NVS.
 */
bool wifi_manager_has_saved_credentials(char *out_ssid, size_t max_len);

/**
 * @brief Conecta a uma rede Wi-Fi e salva o SSID/senha no NVS.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password,
                               bool save_to_nvs);

/**
 * @brief Realiza um scan síncrono das redes Wi-Fi próximas.
 * @param out_list Array para receber as redes encontradas
 * @param max_items Tamanho máximo do array
 * @param out_count Quantidade de redes preenchidas
 */
esp_err_t wifi_manager_scan(wifi_ap_item_t *out_list, size_t max_items,
                            size_t *out_count);

/**
 * @brief Retorna o estado atual da conexão Wi-Fi.
 */
wifi_conn_state_t wifi_manager_get_state(void);

/**
 * @brief Obtém o SSID conectado atualmente e o endereço IP em formato string.
 */
void wifi_manager_get_info(char *out_ssid, size_t ssid_len, char *out_ip,
                           size_t ip_len);

#ifdef __cplusplus
}
#endif
