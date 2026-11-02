#include "ui_app.h"
#include "board_cyd.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_app";

// Telas principais
static lv_obj_t *s_scr_dash = NULL;
static lv_obj_t *s_scr_wifi = NULL;
static lv_obj_t *s_scr_pass = NULL;

// Elementos do Dashboard
static lv_obj_t *s_lbl_location = NULL;
static lv_obj_t *s_lbl_wifi_btn = NULL;
static lv_obj_t *s_lbl_clock = NULL;
static lv_obj_t *s_lbl_date = NULL;
static lv_obj_t *s_lbl_curr_temp = NULL;
static lv_obj_t *s_lbl_curr_desc = NULL;
static lv_obj_t *s_lbl_curr_meta = NULL;

// Elementos dos 7 cards da semana
static lv_obj_t *s_day_wday[WEATHER_FORECAST_DAYS] = {};
static lv_obj_t *s_day_desc[WEATHER_FORECAST_DAYS] = {};
static lv_obj_t *s_day_temp[WEATHER_FORECAST_DAYS] = {};
static lv_obj_t *s_day_rain[WEATHER_FORECAST_DAYS] = {};

// Elementos da tela de Wi-Fi
static lv_obj_t *s_wifi_list = NULL;
static lv_obj_t *s_lbl_wifi_status = NULL;
static bool s_scan_running = false;

// Elementos da tela de Senha (Teclado Touch)
static lv_obj_t *s_lbl_pass_ssid = NULL;
static lv_obj_t *s_ta_password = NULL;
static char s_selected_ssid[33] = {0};

static void open_pass_screen(const char *ssid) {
  strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
  s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';

  char title[64];
  snprintf(title, sizeof(title), "Rede: %s", s_selected_ssid);
  lv_label_set_text(s_lbl_pass_ssid, title);
  lv_textarea_set_text(s_ta_password, "");
  lv_screen_load(s_scr_pass);
}

static void wifi_ap_btn_event_cb(lv_event_t *e) {
  const char *ssid = (const char *)lv_event_get_user_data(e);
  if (ssid && strlen(ssid) > 0) {
    open_pass_screen(ssid);
  }
}

static wifi_ap_item_t s_scan_results[WIFI_MAX_SCAN_RESULTS];
static size_t s_scan_count = 0;

static void wifi_scan_worker_task(void *arg) {
  esp_err_t err =
      wifi_manager_scan(s_scan_results, WIFI_MAX_SCAN_RESULTS, &s_scan_count);

  if (board_cyd_lvgl_lock(1000)) {
    lv_obj_clean(s_wifi_list);

    if (err != ESP_OK || s_scan_count == 0) {
      lv_label_set_text(s_lbl_wifi_status,
                        "Nenhuma rede encontrada. Tente novamente.");
    } else {
      char status_txt[80];
      snprintf(status_txt, sizeof(status_txt),
               "%u redes encontradas (toque para conectar):",
               (unsigned)s_scan_count);
      lv_label_set_text(s_lbl_wifi_status, status_txt);

      for (size_t i = 0; i < s_scan_count; i++) {
        char item_txt[64];
        snprintf(item_txt, sizeof(item_txt), "%s (%d dBm)%s",
                 s_scan_results[i].ssid, s_scan_results[i].rssi,
                 s_scan_results[i].secure ? " [Senha]" : " [Aberta]");

        lv_obj_t *btn =
            lv_list_add_button(s_wifi_list, LV_SYMBOL_WIFI, item_txt);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xF8FAFC), 0);
        lv_obj_add_event_cb(btn, wifi_ap_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)s_scan_results[i].ssid);
      }
    }
    s_scan_running = false;
    board_cyd_lvgl_unlock();
  }
  vTaskDelete(NULL);
}

static void start_wifi_scan_ui(void) {
  if (s_scan_running) {
    return;
  }
  s_scan_running = true;
  lv_label_set_text(s_lbl_wifi_status, "Escaneando redes Wi-Fi proximas...");
  lv_obj_clean(s_wifi_list);
  xTaskCreate(wifi_scan_worker_task, "wifi_scan_ui", 4096, NULL, 4, NULL);
}

static void btn_open_wifi_cb(lv_event_t *e) {
  lv_screen_load(s_scr_wifi);
  start_wifi_scan_ui();
}

static void btn_back_dash_cb(lv_event_t *e) { lv_screen_load(s_scr_dash); }

static void btn_rescan_cb(lv_event_t *e) { start_wifi_scan_ui(); }

static void do_connect_from_ui(void) {
  const char *pass = lv_textarea_get_text(s_ta_password);
  ESP_LOGI(TAG, "Conectando via UI na rede '%s'...", s_selected_ssid);
  wifi_manager_connect(s_selected_ssid, pass, true);
  lv_label_set_text(s_lbl_date, "Conectando ao Wi-Fi...");
  lv_screen_load(s_scr_dash);
}

static void btn_connect_pass_cb(lv_event_t *e) { do_connect_from_ui(); }

static void btn_cancel_pass_cb(lv_event_t *e) { lv_screen_load(s_scr_wifi); }

static void kb_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    do_connect_from_ui();
  } else if (code == LV_EVENT_CANCEL) {
    lv_screen_load(s_scr_wifi);
  }
}

static void create_dashboard_screen(void) {
  s_scr_dash = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr_dash, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_text_color(s_scr_dash, lv_color_hex(0xF8FAFC), 0);
  lv_obj_clear_flag(s_scr_dash, LV_OBJ_FLAG_SCROLLABLE);

  // 1. Barra superior (Localização + Botão Wi-Fi)
  lv_obj_t *top_bar = lv_obj_create(s_scr_dash);
  lv_obj_set_size(top_bar, 312, 32);
  lv_obj_set_pos(top_bar, 4, 4);
  lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_width(top_bar, 0, 0);
  lv_obj_set_style_pad_all(top_bar, 4, 0);
  lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

  s_lbl_location = lv_label_create(top_bar);
  lv_label_set_text(s_lbl_location, LV_SYMBOL_GPS " Detectando regiao...");
  lv_obj_set_style_text_color(s_lbl_location, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(s_lbl_location, LV_ALIGN_LEFT_MID, 4, 0);

  lv_obj_t *btn_wifi = lv_button_create(top_bar);
  lv_obj_set_size(btn_wifi, 78, 24);
  lv_obj_align(btn_wifi, LV_ALIGN_RIGHT_MID, -2, 0);
  lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x334155), 0);
  lv_obj_add_event_cb(btn_wifi, btn_open_wifi_cb, LV_EVENT_CLICKED, NULL);

  s_lbl_wifi_btn = lv_label_create(btn_wifi);
  lv_label_set_text(s_lbl_wifi_btn, LV_SYMBOL_WIFI " Config");
  lv_obj_center(s_lbl_wifi_btn);

  // 2. Card Esquerdo: Relógio e Data
  lv_obj_t *clock_card = lv_obj_create(s_scr_dash);
  lv_obj_set_size(clock_card, 154, 88);
  lv_obj_set_pos(clock_card, 4, 40);
  lv_obj_set_style_bg_color(clock_card, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_width(clock_card, 1, 0);
  lv_obj_set_style_border_color(clock_card, lv_color_hex(0x334155), 0);
  lv_obj_set_style_pad_all(clock_card, 6, 0);
  lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl_clock_title = lv_label_create(clock_card);
  lv_label_set_text(lbl_clock_title, "HORA LOCAL");
  lv_obj_set_style_text_color(lbl_clock_title, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(lbl_clock_title, LV_ALIGN_TOP_MID, 0, 0);

  s_lbl_clock = lv_label_create(clock_card);
  lv_label_set_text(s_lbl_clock, "--:--:--");
  lv_obj_set_style_text_color(s_lbl_clock, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(s_lbl_clock, LV_ALIGN_CENTER, 0, 2);

  s_lbl_date = lv_label_create(clock_card);
  lv_label_set_text(s_lbl_date, "Sem Wi-Fi");
  lv_obj_set_style_text_color(s_lbl_date, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(s_lbl_date, LV_ALIGN_BOTTOM_MID, 0, 0);

  // 3. Card Direito: Clima Atual
  lv_obj_t *weather_card = lv_obj_create(s_scr_dash);
  lv_obj_set_size(weather_card, 154, 88);
  lv_obj_set_pos(weather_card, 162, 40);
  lv_obj_set_style_bg_color(weather_card, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_width(weather_card, 1, 0);
  lv_obj_set_style_border_color(weather_card, lv_color_hex(0x334155), 0);
  lv_obj_set_style_pad_all(weather_card, 6, 0);
  lv_obj_clear_flag(weather_card, LV_OBJ_FLAG_SCROLLABLE);

  s_lbl_curr_temp = lv_label_create(weather_card);
  lv_label_set_text(s_lbl_curr_temp, "--.- C");
  lv_obj_set_style_text_color(s_lbl_curr_temp, lv_color_hex(0xFACC15), 0);
  lv_obj_align(s_lbl_curr_temp, LV_ALIGN_TOP_MID, 0, 2);

  s_lbl_curr_desc = lv_label_create(weather_card);
  lv_label_set_text(s_lbl_curr_desc, "Aguardando dados");
  lv_obj_set_style_text_color(s_lbl_curr_desc, lv_color_hex(0xE2E8F0), 0);
  lv_obj_align(s_lbl_curr_desc, LV_ALIGN_CENTER, 0, 2);

  s_lbl_curr_meta = lv_label_create(weather_card);
  lv_label_set_text(s_lbl_curr_meta, "Umid: --% | -- km/h");
  lv_obj_set_style_text_color(s_lbl_curr_meta, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(s_lbl_curr_meta, LV_ALIGN_BOTTOM_MID, 0, 0);

  // 4. Painel Inferior: Previsão dos 7 Dias da Semana (Rolável no Touch)
  lv_obj_t *week_cont = lv_obj_create(s_scr_dash);
  lv_obj_set_size(week_cont, 312, 104);
  lv_obj_set_pos(week_cont, 4, 132);
  lv_obj_set_style_bg_color(week_cont, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_border_width(week_cont, 0, 0);
  lv_obj_set_style_pad_all(week_cont, 2, 0);
  lv_obj_set_style_pad_column(week_cont, 6, 0);
  lv_obj_set_flex_flow(week_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(week_cont, LV_DIR_HOR);

  for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
    lv_obj_t *card = lv_obj_create(week_cont);
    lv_obj_set_size(card, 72, 94);
    lv_obj_set_style_bg_color(
        card, (i == 0) ? lv_color_hex(0x1E3A8A) : lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x334155), 0);
    lv_obj_set_style_pad_all(card, 3, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_day_wday[i] = lv_label_create(card);
    lv_label_set_text(s_day_wday[i], (i == 0) ? "Hoje" : "---");
    lv_obj_set_style_text_color(s_day_wday[i], lv_color_hex(0x38BDF8), 0);
    lv_obj_align(s_day_wday[i], LV_ALIGN_TOP_MID, 0, 0);

    s_day_desc[i] = lv_label_create(card);
    lv_label_set_text(s_day_desc[i], "---");
    lv_obj_set_style_text_color(s_day_desc[i], lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(s_day_desc[i], LV_ALIGN_CENTER, 0, -8);

    s_day_temp[i] = lv_label_create(card);
    lv_label_set_text(s_day_temp[i], "--/--");
    lv_obj_set_style_text_color(s_day_temp[i], lv_color_hex(0xFACC15), 0);
    lv_obj_align(s_day_temp[i], LV_ALIGN_CENTER, 0, 12);

    s_day_rain[i] = lv_label_create(card);
    lv_label_set_text(s_day_rain[i], "Chuva --%");
    lv_obj_set_style_text_color(s_day_rain[i], lv_color_hex(0x94A3B8), 0);
    lv_obj_align(s_day_rain[i], LV_ALIGN_BOTTOM_MID, 0, 0);
  }
}

static void create_wifi_screen(void) {
  s_scr_wifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr_wifi, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_text_color(s_scr_wifi, lv_color_hex(0xF8FAFC), 0);
  lv_obj_clear_flag(s_scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

  // Topo
  lv_obj_t *btn_back = lv_button_create(s_scr_wifi);
  lv_obj_set_size(btn_back, 70, 28);
  lv_obj_set_pos(btn_back, 6, 6);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x334155), 0);
  lv_obj_add_event_cb(btn_back, btn_back_dash_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_back = lv_label_create(btn_back);
  lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Voltar");
  lv_obj_center(lbl_back);

  lv_obj_t *lbl_title = lv_label_create(s_scr_wifi);
  lv_label_set_text(lbl_title, "Configurar Wi-Fi");
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t *btn_scan = lv_button_create(s_scr_wifi);
  lv_obj_set_size(btn_scan, 76, 28);
  lv_obj_set_pos(btn_scan, 238, 6);
  lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x0284C7), 0);
  lv_obj_add_event_cb(btn_scan, btn_rescan_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_scan = lv_label_create(btn_scan);
  lv_label_set_text(lbl_scan, LV_SYMBOL_REFRESH " Scan");
  lv_obj_center(lbl_scan);

  s_lbl_wifi_status = lv_label_create(s_scr_wifi);
  lv_label_set_text(s_lbl_wifi_status,
                    "Toque em Scan para buscar redes Wi-Fi:");
  lv_obj_set_style_text_color(s_lbl_wifi_status, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_pos(s_lbl_wifi_status, 8, 40);

  s_wifi_list = lv_list_create(s_scr_wifi);
  lv_obj_set_size(s_wifi_list, 308, 174);
  lv_obj_set_pos(s_wifi_list, 6, 60);
  lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(s_wifi_list, lv_color_hex(0x334155), 0);
}

static void create_password_screen(void) {
  s_scr_pass = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr_pass, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_text_color(s_scr_pass, lv_color_hex(0xF8FAFC), 0);
  lv_obj_clear_flag(s_scr_pass, LV_OBJ_FLAG_SCROLLABLE);

  s_lbl_pass_ssid = lv_label_create(s_scr_pass);
  lv_label_set_text(s_lbl_pass_ssid, "Rede: ---");
  lv_obj_set_style_text_color(s_lbl_pass_ssid, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_pos(s_lbl_pass_ssid, 8, 6);

  lv_obj_t *btn_cancel = lv_button_create(s_scr_pass);
  lv_obj_set_size(btn_cancel, 70, 26);
  lv_obj_set_pos(btn_cancel, 164, 4);
  lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x475569), 0);
  lv_obj_add_event_cb(btn_cancel, btn_cancel_pass_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
  lv_label_set_text(lbl_cancel, "Voltar");
  lv_obj_center(lbl_cancel);

  lv_obj_t *btn_conn = lv_button_create(s_scr_pass);
  lv_obj_set_size(btn_conn, 76, 26);
  lv_obj_set_pos(btn_conn, 238, 4);
  lv_obj_set_style_bg_color(btn_conn, lv_color_hex(0x16A34A), 0);
  lv_obj_add_event_cb(btn_conn, btn_connect_pass_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_conn = lv_label_create(btn_conn);
  lv_label_set_text(lbl_conn, "Conectar");
  lv_obj_center(lbl_conn);

  s_ta_password = lv_textarea_create(s_scr_pass);
  lv_textarea_set_one_line(s_ta_password, true);
  lv_textarea_set_placeholder_text(s_ta_password, "Digite a senha do Wi-Fi...");
  lv_obj_set_size(s_ta_password, 308, 36);
  lv_obj_set_pos(s_ta_password, 6, 34);
  lv_obj_set_style_bg_color(s_ta_password, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_text_color(s_ta_password, lv_color_hex(0xFFFFFF), 0);

  // Teclado Virtual Touchscreen ocupando a metade inferior da tela 320x240
  lv_obj_t *kb = lv_keyboard_create(s_scr_pass);
  lv_obj_set_size(kb, 320, 160);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, s_ta_password);
  lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

void ui_app_init(void) {
  if (!board_cyd_lvgl_lock(1000)) {
    return;
  }

  create_dashboard_screen();
  create_wifi_screen();
  create_password_screen();

  // Se ainda não houver Wi-Fi salvo no NVS, abre diretamente a tela de
  // configuração de Wi-Fi!
  if (wifi_manager_has_saved_credentials(NULL, 0)) {
    lv_screen_load(s_scr_dash);
  } else {
    lv_screen_load(s_scr_wifi);
    start_wifi_scan_ui();
  }

  board_cyd_lvgl_unlock();
}

void ui_app_show_wifi_config(void) {
  if (board_cyd_lvgl_lock(500)) {
    lv_screen_load(s_scr_wifi);
    start_wifi_scan_ui();
    board_cyd_lvgl_unlock();
  }
}

void ui_app_update_clock_and_status(void) {
  char time_str[16];
  char date_str[32];
  geo_time_get_formatted(time_str, sizeof(time_str), date_str,
                         sizeof(date_str));

  wifi_conn_state_t st = wifi_manager_get_state();

  if (board_cyd_lvgl_lock(200)) {
    lv_label_set_text(s_lbl_clock, time_str);

    if (st == WIFI_STATE_CONNECTED) {
      lv_label_set_text(s_lbl_date, date_str);
      lv_label_set_text(s_lbl_wifi_btn, LV_SYMBOL_WIFI " OK");
    } else if (st == WIFI_STATE_CONNECTING) {
      lv_label_set_text(s_lbl_date, "Conectando Wi-Fi...");
      lv_label_set_text(s_lbl_wifi_btn, LV_SYMBOL_WIFI " ...");
    } else {
      lv_label_set_text(s_lbl_date, "Toque em Wi-Fi");
      lv_label_set_text(s_lbl_wifi_btn, LV_SYMBOL_WIFI " Config");
    }

    board_cyd_lvgl_unlock();
  }
}

void ui_app_update_weather(const geo_location_t *loc,
                           const weather_data_t *weather) {
  if (!board_cyd_lvgl_lock(500)) {
    return;
  }

  if (loc && loc->valid) {
    char loc_txt[128];
    snprintf(loc_txt, sizeof(loc_txt), LV_SYMBOL_GPS " %s, %s", loc->city,
             loc->region);
    lv_label_set_text(s_lbl_location, loc_txt);
  }

  if (weather && weather->valid) {
    char temp_txt[24];
    snprintf(temp_txt, sizeof(temp_txt), "%.1f C", weather->current_temp);
    lv_label_set_text(s_lbl_curr_temp, temp_txt);

    lv_label_set_text(s_lbl_curr_desc, weather->current_desc);

    char meta_txt[48];
    snprintf(meta_txt, sizeof(meta_txt), "Umid: %d%% | %.0f km/h",
             weather->current_humidity, weather->current_wind_kmh);
    lv_label_set_text(s_lbl_curr_meta, meta_txt);

    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
      lv_label_set_text(s_day_wday[i],
                        (i == 0) ? "Hoje" : weather->days[i].weekday);
      lv_label_set_text(s_day_desc[i], weather->days[i].short_desc);

      char dtemp[24];
      snprintf(dtemp, sizeof(dtemp), "%.0f/%.0fC", weather->days[i].temp_max,
               weather->days[i].temp_min);
      lv_label_set_text(s_day_temp[i], dtemp);

      char drain[24];
      snprintf(drain, sizeof(drain), "Chuva %d%%", weather->days[i].rain_prob);
      lv_label_set_text(s_day_rain[i], drain);
    }
  }

  board_cyd_lvgl_unlock();
}
