#include "board_cyd.h"
#include "geo_time_service.h"
#include "ui_app.h"
#include "weather_service.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "smart_clock";

// Imprime um painel completo no Monitor Serial (útil também para testar sem o display conectado)
static void print_serial_dashboard(void) {
  char ssid[33] = {0};
  char ip[20] = {0};
  wifi_manager_get_info(ssid, sizeof(ssid), ip, sizeof(ip));

  char time_str[16];
  char date_str[32];
  geo_time_get_formatted(time_str, sizeof(time_str), date_str,
                         sizeof(date_str));

  const geo_location_t *loc = geo_time_get_location();
  const weather_data_t *w = weather_service_get_data();

  printf("\n========================================================\n");
  printf("  SMART CLOCK & CLIMA - ESP32-2432S028R (CYD)\n");
  printf("========================================================\n");
  printf("  Wi-Fi   : %s (IP: %s)\n",
         (wifi_manager_get_state() == WIFI_STATE_CONNECTED) ? ssid
                                                            : "Desconectado",
         (strlen(ip) > 0) ? ip : "---");
  printf("  Regiao  : %s, %s - %s (TZ: %s)\n", loc->city, loc->region,
         loc->country, loc->timezone);
  printf("  Horario : %s | %s\n", time_str, date_str);

  if (w->valid) {
    printf("--------------------------------------------------------\n");
    printf("  Agora   : %.1f C - %s | Umid: %d%% | Vento: %.1f km/h\n",
           w->current_temp, w->current_desc, w->current_humidity,
           w->current_wind_kmh);
    printf("--------------------------------------------------------\n");
    printf("  PREVISAO DA SEMANA (7 DIAS):\n");
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
      printf("    %-4s (%s) : Max %4.1f C / Min %4.1f C | Chuva %3d%% | %s\n",
             (i == 0) ? "Hoje" : w->days[i].weekday, w->days[i].date,
             w->days[i].temp_max, w->days[i].temp_min, w->days[i].rain_prob,
             w->days[i].short_desc);
    }
  } else {
    printf("  Clima   : Aguardando sincronizacao com Open-Meteo...\n");
  }
  printf("========================================================\n");
  printf("Comandos Serial: 'scan' | 'wifi <SSID> <SENHA>' | 'status'\n\n");
}

// Task de console via Serial (permite configurar Wi-Fi também pelo terminal USB)
static void serial_cli_task(void *arg) {
  char line[128];
  size_t pos = 0;

  while (true) {
    int c = fgetc(stdin);
    if (c == EOF) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (pos > 0) {
        line[pos] = '\0';
        pos = 0;

        if (strcmp(line, "scan") == 0) {
          wifi_ap_item_t list[WIFI_MAX_SCAN_RESULTS];
          size_t count = 0;
          printf("\nEscaneando redes Wi-Fi...\n");
          if (wifi_manager_scan(list, WIFI_MAX_SCAN_RESULTS, &count) ==
              ESP_OK) {
            for (size_t i = 0; i < count; i++) {
              printf("  [%u] %-32s (%d dBm) %s\n", (unsigned)(i + 1),
                     list[i].ssid, list[i].rssi,
                     list[i].secure ? "[ WPA ]" : "[Aberta]");
            }
          }
        } else if (strncmp(line, "wifi ", 5) == 0) {
          char *rest = line + 5;
          char *space = strchr(rest, ' ');
          char ssid[33] = {0};
          char pass[65] = {0};
          if (space) {
            size_t ssid_len = space - rest;
            if (ssid_len >= sizeof(ssid))
              ssid_len = sizeof(ssid) - 1;
            memcpy(ssid, rest, ssid_len);
            strncpy(pass, space + 1, sizeof(pass) - 1);
          } else {
            strncpy(ssid, rest, sizeof(ssid) - 1);
          }
          printf("\nConectando na rede '%s'...\n", ssid);
          wifi_manager_connect(ssid, pass, true);
        } else if (strcmp(line, "status") == 0) {
          print_serial_dashboard();
        } else {
          printf("Comando desconhecido: '%s'. Use: scan | wifi <ssid> <senha> "
                 "| status\n",
                 line);
        }
      }
    } else if (pos < sizeof(line) - 1) {
      line[pos++] = (char)c;
    }
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Iniciando Smart Clock & Clima (ESP32-2432S028R)...");

  // 1. Inicializa Wi-Fi + NVS e serviço de Hora/SNTP
  ESP_ERROR_CHECK(wifi_manager_init());
  geo_time_init();

  // 2. Inicializa o Display ILI9341 + Touch XPT2046 + LVGL da placa CYD
  ESP_ERROR_CHECK(board_cyd_init());
  ui_app_init();

  // 3. Inicia também o console interativo via Serial para testes via USB
  xTaskCreate(serial_cli_task, "serial_cli", 4096, NULL, 3, NULL);

  bool location_synced = false;
  uint32_t weather_cooldown_sec = 0;

  while (true) {
    wifi_conn_state_t wifi_st = wifi_manager_get_state();

    if (wifi_st == WIFI_STATE_CONNECTED) {
      // LED Verde suave ou apagado quando conectado
      board_cyd_set_rgb_led(false, false, false);

      if (!location_synced) {
        board_cyd_set_rgb_led(false, false, true); // Azul: sincronizando dados
        geo_location_t loc = {};
        if (geo_time_fetch_location(&loc) == ESP_OK) {
          location_synced = true;
          weather_cooldown_sec = 0; // Força atualização imediata do clima
        }
        board_cyd_set_rgb_led(false, false, false);
      }

      if (weather_cooldown_sec == 0) {
        const geo_location_t *loc = geo_time_get_location();
        weather_data_t weather = {};
        if (weather_service_fetch(loc->latitude, loc->longitude, &weather) ==
            ESP_OK) {
          ui_app_update_weather(loc, &weather);
          print_serial_dashboard();
          weather_cooldown_sec = 1800; // Atualiza o clima a cada 30 minutos
        } else {
          weather_cooldown_sec = 30; // Tenta novamente em 30s se falhar
        }
      } else {
        weather_cooldown_sec--;
      }
    } else if (wifi_st == WIFI_STATE_CONNECTING) {
      // Pisca LED Azul enquanto conecta
      board_cyd_set_rgb_led(false, false, true);
      location_synced = false;
    } else {
      // LED Vermelho discreto se desconectado
      board_cyd_set_rgb_led(true, false, false);
      location_synced = false;
    }

    // Atualiza o relógio na tela a cada 1 segundo
    ui_app_update_clock_and_status();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
