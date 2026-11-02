# ESP32 Smart Clock & Clima (ESP32-2432S028R / CYD)

Relógio inteligente e estação de previsão do tempo de 7 dias desenvolvido com **ESP-IDF (C/C++)** e **LVGL 9** para a placa **ESP32-2432S028R ("Cheap Yellow Display")**.

## Funcionalidades

- **Interface Gráfica Touchscreen (LVGL 9 - 320x240)**:
  - **Dashboard Principal**: Exibe cidade/região atual, relógio digital sincronizado, data por extenso em português, temperatura/umidade/vento atuais e cards roláveis com a previsão dos **7 dias da semana**.
  - **Configuração de Wi-Fi na Tela**: Scanner de redes Wi-Fi próximas e teclado virtual touchscreen (`lv_keyboard`) para digitar a senha e salvar na memória Flash (`NVS`).
- **Descoberta Automática de Região e Fuso Horário**:
  - Detecta automaticamente a cidade, latitude/longitude e fuso horário via GeoIP (`ip-api.com`) e sincroniza o relógio interno via **SNTP (`pool.ntp.org`)**.
- **Previsão do Tempo da Semana (Open-Meteo API)**:
  - Consulta via HTTPS (`esp_http_client` + `esp_crt_bundle`) a API gratuita **Open-Meteo** (sem necessidade de chave de API) para obter o clima atual e a previsão de 7 dias.
- **Console Interativo via Serial (UART)**:
  - Permite escanear redes (`scan`), configurar o Wi-Fi (`wifi <SSID> <SENHA>`) e visualizar o painel completo (`status`) pelo terminal USB mesmo sem o display conectado.

## Pinagem (ESP32-2432S028R)

| Periférico | Barramento / Pinos |
| --- | --- |
| **Display TFT 2.8" (ILI9341)** | `SPI2_HOST` — `MOSI=13`, `MISO=12`, `SCLK=14`, `CS=15`, `DC=2`, `BL=21` |
| **Touchscreen (XPT2046)** | `SPI3_HOST` — `CLK=25`, `MOSI=32`, `MISO=39`, `CS=33`, `IRQ=36` |
| **LED RGB** | `R=4`, `G=16`, `B=17` (Ativo em nível baixo) |

## Como Compilar e Gravar

```bash
# Ativar o ambiente do ESP-IDF
source ~/.espressif/tools/activate_idf_v6.1.sh

# Compilar o projeto
idf.py build

# Gravar e abrir o monitor serial
idf.py -p /dev/ttyACM0 flash monitor
```
