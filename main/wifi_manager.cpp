#include "wifi_manager.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_manager";
static const char *NVS_NS = "wifi_cfg";

static volatile wifi_conn_state_t s_wifi_state = WIFI_STATE_DISCONNECTED;
static char s_current_ssid[33] = {0};
static char s_current_ip[20] = {0};
static int s_retry_count = 0;
static bool s_scanning = false;
static SemaphoreHandle_t s_wifi_lock = NULL;

#define MAX_RETRY_ATTEMPTS 5

static esp_err_t save_credentials_nvs(const char *ssid, const char *password) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  nvs_set_str(handle, "ssid", ssid);
  nvs_set_str(handle, "pass", password ? password : "");
  err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static bool load_credentials_nvs(char *ssid, size_t ssid_len, char *pass,
                                 size_t pass_len) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t err1 = nvs_get_str(handle, "ssid", ssid, &ssid_len);
  esp_err_t err2 = nvs_get_str(handle, "pass", pass, &pass_len);
  nvs_close(handle);
  return (err1 == ESP_OK && err2 == ESP_OK && strlen(ssid) > 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "Wi-Fi STA iniciado");
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_current_ip[0] = '\0';
    if (!s_scanning && s_retry_count < MAX_RETRY_ATTEMPTS &&
        strlen(s_current_ssid) > 0) {
      s_retry_count++;
      s_wifi_state = WIFI_STATE_CONNECTING;
      ESP_LOGW(TAG, "Reconectando ao Wi-Fi (%d/%d)...", s_retry_count,
               MAX_RETRY_ATTEMPTS);
      esp_wifi_connect();
    } else if (!s_scanning) {
      s_wifi_state =
          (s_retry_count >= MAX_RETRY_ATTEMPTS) ? WIFI_STATE_ERROR : WIFI_STATE_DISCONNECTED;
      ESP_LOGW(TAG, "Desconectado do Wi-Fi");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_current_ip, sizeof(s_current_ip), IPSTR,
             IP2STR(&event->ip_info.ip));
    s_retry_count = 0;
    s_wifi_state = WIFI_STATE_CONNECTED;
    ESP_LOGI(TAG, "Conectado! SSID: %s | IP: %s", s_current_ssid, s_current_ip);
  }
}

esp_err_t wifi_manager_init(void) {
  s_wifi_lock = xSemaphoreCreateMutex();

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(ret, TAG, "Erro ao inicializar NVS");

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Erro em esp_netif_init");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                      "Erro ao criar event loop");
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Erro em esp_wifi_init");

  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                          NULL, NULL),
                      TAG, "Erro ao registrar WIFI_EVENT");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler,
                          NULL, NULL),
                      TAG, "Erro ao registrar IP_EVENT");

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                      "Erro ao definir modo STA");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Erro ao iniciar Wi-Fi");

  // Verifica se há Wi-Fi salvo no NVS para conectar automaticamente
  char saved_ssid[33] = {0};
  char saved_pass[65] = {0};
  if (load_credentials_nvs(saved_ssid, sizeof(saved_ssid), saved_pass,
                           sizeof(saved_pass))) {
    ESP_LOGI(TAG, "Credenciais salvas encontradas para '%s', conectando...",
             saved_ssid);
    wifi_manager_connect(saved_ssid, saved_pass, false);
  } else {
    ESP_LOGI(TAG, "Nenhuma rede Wi-Fi salva no NVS. Aguardando configuração na UI.");
  }

  return ESP_OK;
}

bool wifi_manager_has_saved_credentials(char *out_ssid, size_t max_len) {
  char ssid[33] = {0};
  char pass[65] = {0};
  if (load_credentials_nvs(ssid, sizeof(ssid), pass, sizeof(pass))) {
    if (out_ssid && max_len > 0) {
      strncpy(out_ssid, ssid, max_len - 1);
      out_ssid[max_len - 1] = '\0';
    }
    return true;
  }
  return false;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password,
                               bool save_to_nvs) {
  if (!ssid || strlen(ssid) == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_wifi_lock, portMAX_DELAY);

  if (save_to_nvs) {
    save_credentials_nvs(ssid, password);
  }

  strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
  s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
  s_retry_count = 0;
  s_wifi_state = WIFI_STATE_CONNECTING;

  esp_wifi_disconnect();

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
  if (password && strlen(password) > 0) {
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);
  }
  wifi_config.sta.threshold.authmode =
      (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err == ESP_OK) {
    err = esp_wifi_connect();
  }

  xSemaphoreGive(s_wifi_lock);
  return err;
}

esp_err_t wifi_manager_scan(wifi_ap_item_t *out_list, size_t max_items,
                            size_t *out_count) {
  if (!out_list || !out_count || max_items == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_count = 0;

  xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
  s_scanning = true;

  // Se estava tentando conectar em loop, pausa antes do scan
  if (s_wifi_state == WIFI_STATE_CONNECTING) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  wifi_scan_config_t scan_cfg = {};
  scan_cfg.show_hidden = false;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

  ESP_LOGI(TAG, "Iniciando scan de redes Wi-Fi...");
  esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Falha no scan Wi-Fi: %s", esp_err_to_name(err));
    s_scanning = false;
    xSemaphoreGive(s_wifi_lock);
    return err;
  }

  uint16_t ap_num = WIFI_MAX_SCAN_RESULTS;
  wifi_ap_record_t ap_records[WIFI_MAX_SCAN_RESULTS];
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_records));

  size_t count = 0;
  for (uint16_t i = 0; i < ap_num && count < max_items; i++) {
    if (strlen((const char *)ap_records[i].ssid) == 0) {
      continue;
    }
    // Evita SSIDs duplicados (repetidores na mesma rede)
    bool duplicate = false;
    for (size_t j = 0; j < count; j++) {
      if (strcmp(out_list[j].ssid, (const char *)ap_records[i].ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    strncpy(out_list[count].ssid, (const char *)ap_records[i].ssid,
            sizeof(out_list[count].ssid) - 1);
    out_list[count].ssid[sizeof(out_list[count].ssid) - 1] = '\0';
    out_list[count].rssi = ap_records[i].rssi;
    out_list[count].secure = (ap_records[i].authmode != WIFI_AUTH_OPEN);
    count++;
  }

  *out_count = count;
  s_scanning = false;
  ESP_LOGI(TAG, "Scan finalizado: %u redes únicas encontradas", (unsigned)count);

  xSemaphoreGive(s_wifi_lock);
  return ESP_OK;
}

wifi_conn_state_t wifi_manager_get_state(void) { return s_wifi_state; }

void wifi_manager_get_info(char *out_ssid, size_t ssid_len, char *out_ip,
                           size_t ip_len) {
  if (out_ssid && ssid_len > 0) {
    strncpy(out_ssid, s_current_ssid, ssid_len - 1);
    out_ssid[ssid_len - 1] = '\0';
  }
  if (out_ip && ip_len > 0) {
    strncpy(out_ip, s_current_ip, ip_len - 1);
    out_ip[ip_len - 1] = '\0';
  }
}
