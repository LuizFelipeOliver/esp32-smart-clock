#include "geo_time_service.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "geo_time";

static geo_location_t s_location = {
    .valid = false,
    .city = "Sao Paulo",
    .region = "SP",
    .country = "Brazil",
    .timezone = "America/Sao_Paulo",
    .latitude = -23.5505,
    .longitude = -46.6333,
    .utc_offset_sec = -10800,
};

static bool s_sntp_started = false;

// Converte caracteres UTF-8 acentuados comuns em equivalentes ASCII para renderizar
// perfeitamente nas fontes padrão do LVGL
static void normalize_utf8_ascii(char *str) {
  if (!str)
    return;
  uint8_t *src = (uint8_t *)str;
  uint8_t *dst = (uint8_t *)str;
  while (*src) {
    if (*src == 0xC3 && *(src + 1)) {
      uint8_t c = *(src + 1);
      char rep = 0;
      if (c >= 0xA0 && c <= 0xA5)
        rep = 'a';
      else if (c >= 0x80 && c <= 0x85)
        rep = 'A';
      else if (c >= 0xA8 && c <= 0xAB)
        rep = 'e';
      else if (c >= 0x88 && c <= 0x8B)
        rep = 'E';
      else if (c >= 0xAC && c <= 0xAF)
        rep = 'i';
      else if (c >= 0x8C && c <= 0x8F)
        rep = 'I';
      else if (c >= 0xB2 && c <= 0xB6)
        rep = 'o';
      else if (c >= 0x92 && c <= 0x96)
        rep = 'O';
      else if (c >= 0xB9 && c <= 0xBC)
        rep = 'u';
      else if (c >= 0x99 && c <= 0x9C)
        rep = 'U';
      else if (c == 0xA7)
        rep = 'c';
      else if (c == 0x87)
        rep = 'C';
      else if (c == 0xB1)
        rep = 'n';
      else if (c == 0x91)
        rep = 'N';

      if (rep) {
        *dst++ = (uint8_t)rep;
        src += 2;
        continue;
      }
    }
    *dst++ = *src++;
  }
  *dst = '\0';
}

static void apply_utc_offset(int32_t offset_sec) {
  // No padrão POSIX TZ, o sinal é invertido em relação ao ISO 8601:
  // UTC-3 (Brasil, offset = -10800) corresponde a "UTC+3" no POSIX TZ.
  char sign = (offset_sec <= 0) ? '+' : '-';
  int32_t abs_sec = abs(offset_sec);
  int hours = abs_sec / 3600;
  int mins = (abs_sec % 3600) / 60;

  char tz_str[32];
  if (mins > 0) {
    snprintf(tz_str, sizeof(tz_str), "UTC%c%d:%02d", sign, hours, mins);
  } else {
    snprintf(tz_str, sizeof(tz_str), "UTC%c%d", sign, hours);
  }
  setenv("TZ", tz_str, 1);
  tzset();
  ESP_LOGI(TAG, "Fuso horário configurado para %s (offset %lds)", tz_str,
           (long)offset_sec);
}

void geo_time_init(void) {
  // Aplica fuso horário padrão (UTC-3) até que o GeoIP detecte a região real
  apply_utc_offset(s_location.utc_offset_sec);

  if (!s_sntp_started) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    s_sntp_started = true;
    ESP_LOGI(TAG, "Serviço SNTP iniciado (pool.ntp.org)");
  }
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} http_resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && rb && rb->buf) {
    size_t copy_len = evt->data_len;
    if (rb->len + copy_len >= rb->cap) {
      copy_len = (rb->cap - 1) - rb->len;
    }
    if (copy_len > 0) {
      memcpy(rb->buf + rb->len, evt->data, copy_len);
      rb->len += copy_len;
      rb->buf[rb->len] = '\0';
    }
  }
  return ESP_OK;
}

esp_err_t geo_time_fetch_location(geo_location_t *out_loc) {
  char *resp_buf = (char *)malloc(1024);
  if (!resp_buf) {
    return ESP_ERR_NO_MEM;
  }
  resp_buf[0] = '\0';

  http_resp_buf_t rb = {.buf = resp_buf, .len = 0, .cap = 1024};

  esp_http_client_config_t cfg = {};
  cfg.url = "http://ip-api.com/json/"
            "?fields=status,country,regionName,city,lat,lon,timezone,offset";
  cfg.timeout_ms = 8000;
  cfg.event_handler = http_event_handler;
  cfg.user_data = &rb;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  esp_err_t err = esp_http_client_perform(client);
  int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status_code != 200 || rb.len == 0) {
    ESP_LOGW(TAG, "Falha ao consultar ip-api.com (err=%s, HTTP=%d)",
             esp_err_to_name(err), status_code);
    free(resp_buf);
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(resp_buf);
  free(resp_buf);
  if (!root) {
    ESP_LOGW(TAG, "Erro ao fazer parse do JSON de geolocalização");
    return ESP_FAIL;
  }

  cJSON *status = cJSON_GetObjectItem(root, "status");
  if (!cJSON_IsString(status) || strcmp(status->valuestring, "success") != 0) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  cJSON *city = cJSON_GetObjectItem(root, "city");
  cJSON *region = cJSON_GetObjectItem(root, "regionName");
  cJSON *country = cJSON_GetObjectItem(root, "country");
  cJSON *tz = cJSON_GetObjectItem(root, "timezone");
  cJSON *lat = cJSON_GetObjectItem(root, "lat");
  cJSON *lon = cJSON_GetObjectItem(root, "lon");
  cJSON *offset = cJSON_GetObjectItem(root, "offset");

  if (cJSON_IsString(city)) {
    strncpy(s_location.city, city->valuestring, sizeof(s_location.city) - 1);
    normalize_utf8_ascii(s_location.city);
  }
  if (cJSON_IsString(region)) {
    strncpy(s_location.region, region->valuestring,
            sizeof(s_location.region) - 1);
    normalize_utf8_ascii(s_location.region);
  }
  if (cJSON_IsString(country)) {
    strncpy(s_location.country, country->valuestring,
            sizeof(s_location.country) - 1);
    normalize_utf8_ascii(s_location.country);
  }
  if (cJSON_IsString(tz)) {
    strncpy(s_location.timezone, tz->valuestring,
            sizeof(s_location.timezone) - 1);
  }
  if (cJSON_IsNumber(lat)) {
    s_location.latitude = lat->valuedouble;
  }
  if (cJSON_IsNumber(lon)) {
    s_location.longitude = lon->valuedouble;
  }
  if (cJSON_IsNumber(offset)) {
    s_location.utc_offset_sec = offset->valueint;
    apply_utc_offset(s_location.utc_offset_sec);
  }

  s_location.valid = true;
  cJSON_Delete(root);

  ESP_LOGI(TAG, "Região detectada: %s, %s (%s) | Lat: %.4f, Lon: %.4f | TZ: %s",
           s_location.city, s_location.region, s_location.country,
           s_location.latitude, s_location.longitude, s_location.timezone);

  if (out_loc) {
    *out_loc = s_location;
  }
  return ESP_OK;
}

const geo_location_t *geo_time_get_location(void) { return &s_location; }

bool geo_time_is_synced(void) {
  time_t now = 0;
  struct tm timeinfo = {};
  time(&now);
  localtime_r(&now, &timeinfo);
  return (timeinfo.tm_year >= (2025 - 1900));
}

void geo_time_get_formatted(char *out_time_str, size_t time_len,
                            char *out_date_str, size_t date_len) {
  static const char *DAYS_PT[] = {"Dom", "Seg", "Ter", "Qua",
                                  "Qui", "Sex", "Sab"};
  static const char *MONTHS_PT[] = {"Jan", "Fev", "Mar", "Abr", "Mai", "Jun",
                                    "Jul", "Ago", "Set", "Out", "Nov", "Dez"};

  time_t now = 0;
  struct tm ti = {};
  time(&now);
  localtime_r(&now, &ti);

  if (out_time_str && time_len > 0) {
    if (ti.tm_year >= (2025 - 1900)) {
      snprintf(out_time_str, time_len, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min,
               ti.tm_sec);
    } else {
      snprintf(out_time_str, time_len, "--:--:--");
    }
  }

  if (out_date_str && date_len > 0) {
    if (ti.tm_year >= (2025 - 1900)) {
      const char *day_name = DAYS_PT[ti.tm_wday % 7];
      const char *month_name = MONTHS_PT[ti.tm_mon % 12];
      snprintf(out_date_str, date_len, "%s, %02d %s %04d", day_name, ti.tm_mday,
               month_name, ti.tm_year + 1900);
    } else {
      snprintf(out_date_str, date_len, "Sincronizando NTP...");
    }
  }
}
