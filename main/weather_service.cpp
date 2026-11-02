#include "weather_service.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "weather_service";

static weather_data_t s_weather = {};

const char *weather_code_to_desc(int code) {
  if (code == 0)
    return "Ceu Limpo";
  if (code == 1 || code == 2)
    return "Parc. Nublado";
  if (code == 3)
    return "Encoberto";
  if (code == 45 || code == 48)
    return "Neblina";
  if (code >= 51 && code <= 57)
    return "Garoa";
  if (code >= 61 && code <= 67)
    return "Chuva";
  if (code >= 71 && code <= 77)
    return "Neve";
  if (code >= 80 && code <= 82)
    return "Pancadas";
  if (code >= 95)
    return "Tempestade";
  return "Variavel";
}

const char *weather_code_to_short(int code) {
  if (code == 0)
    return "Sol";
  if (code == 1 || code == 2)
    return "Sol/Nuv";
  if (code == 3)
    return "Nublado";
  if (code == 45 || code == 48)
    return "Neblina";
  if (code >= 51 && code <= 57)
    return "Garoa";
  if (code >= 61 && code <= 82)
    return "Chuva";
  if (code >= 95)
    return "Tempest.";
  return "Nuvens";
}

// Calcula o dia da semana (0=Dom..6=Sab) a partir de "YYYY-MM-DD"
static void date_to_weekday_pt(const char *date_str, char *out_wday,
                               size_t max_len) {
  static const char *WDAYS[] = {"Dom", "Seg", "Ter", "Qua",
                                "Qui", "Sex", "Sab"};
  int y = 0, m = 0, d = 0;
  if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 ||
      d < 1 || d > 31) {
    strncpy(out_wday, "---", max_len - 1);
    return;
  }
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) {
    y -= 1;
  }
  int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
  strncpy(out_wday, WDAYS[w], max_len - 1);
  out_wday[max_len - 1] = '\0';
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} weather_http_buf_t;

static esp_err_t weather_http_evt(esp_http_client_event_t *evt) {
  weather_http_buf_t *rb = (weather_http_buf_t *)evt->user_data;
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

esp_err_t weather_service_fetch(double latitude, double longitude,
                                weather_data_t *out_data) {
  char url[384];
  snprintf(
      url, sizeof(url),
      "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
      "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
      "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
      "precipitation_probability_max&timezone=auto",
      latitude, longitude);

  size_t buf_cap = 4096;
  char *resp_buf = (char *)malloc(buf_cap);
  if (!resp_buf) {
    return ESP_ERR_NO_MEM;
  }
  resp_buf[0] = '\0';

  weather_http_buf_t rb = {.buf = resp_buf, .len = 0, .cap = buf_cap};

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 10000;
  cfg.event_handler = weather_http_evt;
  cfg.user_data = &rb;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  ESP_LOGI(TAG, "Buscando clima da semana na Open-Meteo (lat=%.2f, lon=%.2f)...",
           latitude, longitude);

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  esp_err_t err = esp_http_client_perform(client);
  int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status_code != 200 || rb.len == 0) {
    ESP_LOGW(TAG, "Falha ao consultar Open-Meteo (err=%s, HTTP=%d)",
             esp_err_to_name(err), status_code);
    free(resp_buf);
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(resp_buf);
  free(resp_buf);
  if (!root) {
    ESP_LOGW(TAG, "Erro no parse do JSON da Open-Meteo");
    return ESP_FAIL;
  }

  // 1. Clima atual
  cJSON *current = cJSON_GetObjectItem(root, "current");
  if (cJSON_IsObject(current)) {
    cJSON *t = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *h = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *w = cJSON_GetObjectItem(current, "wind_speed_10m");
    cJSON *c = cJSON_GetObjectItem(current, "weather_code");

    if (cJSON_IsNumber(t))
      s_weather.current_temp = (float)t->valuedouble;
    if (cJSON_IsNumber(h))
      s_weather.current_humidity = h->valueint;
    if (cJSON_IsNumber(w))
      s_weather.current_wind_kmh = (float)w->valuedouble;
    if (cJSON_IsNumber(c))
      s_weather.current_code = c->valueint;
    s_weather.current_desc = weather_code_to_desc(s_weather.current_code);
  }

  // 2. Previsão dos 7 dias da semana
  cJSON *daily = cJSON_GetObjectItem(root, "daily");
  if (cJSON_IsObject(daily)) {
    cJSON *times = cJSON_GetObjectItem(daily, "time");
    cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *rain = cJSON_GetObjectItem(daily, "precipitation_probability_max");

    int count = cJSON_GetArraySize(times);
    if (count > WEATHER_FORECAST_DAYS) {
      count = WEATHER_FORECAST_DAYS;
    }

    for (int i = 0; i < count; i++) {
      cJSON *ji_time = cJSON_GetArrayItem(times, i);
      cJSON *ji_code = cJSON_GetArrayItem(codes, i);
      cJSON *ji_max = cJSON_GetArrayItem(tmax, i);
      cJSON *ji_min = cJSON_GetArrayItem(tmin, i);
      cJSON *ji_rain = rain ? cJSON_GetArrayItem(rain, i) : NULL;

      if (cJSON_IsString(ji_time)) {
        strncpy(s_weather.days[i].date, ji_time->valuestring,
                sizeof(s_weather.days[i].date) - 1);
        date_to_weekday_pt(s_weather.days[i].date, s_weather.days[i].weekday,
                           sizeof(s_weather.days[i].weekday));
      }
      if (cJSON_IsNumber(ji_code)) {
        s_weather.days[i].weather_code = ji_code->valueint;
        s_weather.days[i].short_desc =
            weather_code_to_short(ji_code->valueint);
      } else {
        s_weather.days[i].short_desc = "---";
      }
      if (cJSON_IsNumber(ji_max)) {
        s_weather.days[i].temp_max = (float)ji_max->valuedouble;
      }
      if (cJSON_IsNumber(ji_min)) {
        s_weather.days[i].temp_min = (float)ji_min->valuedouble;
      }
      if (ji_rain && cJSON_IsNumber(ji_rain)) {
        s_weather.days[i].rain_prob = ji_rain->valueint;
      } else {
        s_weather.days[i].rain_prob = 0;
      }
    }
  }

  s_weather.valid = true;
  cJSON_Delete(root);

  ESP_LOGI(TAG, "Clima atualizado: %.1f C (%s), Umidade: %d%%, Vento: %.1f km/h",
           s_weather.current_temp, s_weather.current_desc,
           s_weather.current_humidity, s_weather.current_wind_kmh);

  if (out_data) {
    *out_data = s_weather;
  }
  return ESP_OK;
}

const weather_data_t *weather_service_get_data(void) { return &s_weather; }
