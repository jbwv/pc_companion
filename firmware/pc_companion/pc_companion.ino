#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "secrets.h"

LV_FONT_DECLARE(weather_icons_24);
LV_FONT_DECLARE(calendar_icon_24);

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;

#define DIRECT_RENDER_MODE

#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "esp_lcd_touch_axs15231b.h"
#include <Wire.h>

#define GFX_BL 6

#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK  5
#define LCD_QSPI_D0   1
#define LCD_QSPI_D1   2
#define LCD_QSPI_D2   3
#define LCD_QSPI_D3   4

#define I2C_SDA       8
#define I2C_SCL       7

#define LCD_ROTATION 0

TCA9554 TCA(0x20);

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *gfx = new Arduino_AXS15231B(bus, -1, LCD_ROTATION, false, 320, 480);

uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;
lv_display_t *disp;
lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;

#if LV_USE_LOG != 0
void my_print(lv_log_level_t level, const char *buf)
{
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}
#endif

uint32_t millis_cb(void) { return millis(); }

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  touch_data_t touch_data;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&touch_data)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touch_data.coords[0].x;
    data->point.y = touch_data.coords[0].y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ---- Button grid config: 2 cols x 4 rows = 8 per page ----
#define GRID_COLS 2
#define GRID_ROWS 4
#define BTNS_PER_PAGE (GRID_COLS * GRID_ROWS)
#define NUM_PAGES 3
#define BTN_GAP 10
#define HEADER_H 30

#include "page1_buttons.h"

// ---- Page 2: media controls (untouched) ----
const char PAGE2_KEYS[BTNS_PER_PAGE] = {'p','a','v','n','d','u','m','x'};
const char* PAGE2_LABELS[BTNS_PER_PAGE] = {
  LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE,
  LV_SYMBOL_PREV, LV_SYMBOL_NEXT,
  LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX,
  LV_SYMBOL_MUTE, LV_SYMBOL_CLOSE
};

// ---- Page 1 button handler: generic, driven by PAGE1_BUTTONS ----
static void page1_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  const Page1Button *b = &PAGE1_BUTTONS[idx];

  if (b->modifier1 != 0) Keyboard.press(b->modifier1);
  if (b->modifier2 != 0) Keyboard.press(b->modifier2);
  Keyboard.press(b->key);
  delay(50);
  Keyboard.releaseAll();
}

// ---- Page 2 button handler: unchanged ----
static void page2_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  char key = (char)(intptr_t)lv_obj_get_user_data(btn);

  switch (key) {
    case 'p': ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE); delay(50); ConsumerControl.release(); break;
    case 'a': ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE); delay(50); ConsumerControl.release(); break;
    case 'n': ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT); delay(50); ConsumerControl.release(); break;
    case 'v': ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS); delay(50); ConsumerControl.release(); break;
    case 'd': ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT); delay(50); ConsumerControl.release(); break;
    case 'u': ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT); delay(50); ConsumerControl.release(); break;
    case 'm': ConsumerControl.press(CONSUMER_CONTROL_MUTE); delay(50); ConsumerControl.release(); break;
    case 'x': // Close app (Alt+F4)
      Keyboard.press(KEY_LEFT_ALT); Keyboard.press(KEY_F4);
      delay(50); Keyboard.releaseAll();
      break;
    default:
      Keyboard.press(key); delay(50); Keyboard.release(key);
      break;
  }
}

void build_page1(lv_obj_t *tile)
{
  uint32_t tileW = screenWidth;
  uint32_t tileH = screenHeight - HEADER_H;
  uint32_t btnW = (tileW - (BTN_GAP * (GRID_COLS + 1))) / GRID_COLS;
  uint32_t btnH = (tileH - (BTN_GAP * (GRID_ROWS + 1))) / GRID_ROWS;

  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      int idx = row * GRID_COLS + col;

      lv_obj_t *btn = lv_button_create(tile);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_pos(btn,
        BTN_GAP + col * (btnW + BTN_GAP),
        HEADER_H + BTN_GAP + row * (btnH + BTN_GAP));

      lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
      lv_obj_add_event_cb(btn, page1_btn_event_cb, LV_EVENT_CLICKED, NULL);

      lv_obj_t *label = lv_label_create(btn);
      lv_label_set_text(label, PAGE1_BUTTONS[idx].label);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(label, btnW - 10);
      lv_obj_center(label);
    }
  }
}

void build_page2(lv_obj_t *tile, const char *keys, const char **labels)
{
  uint32_t tileW = screenWidth;
  uint32_t tileH = screenHeight - HEADER_H;
  uint32_t btnW = (tileW - (BTN_GAP * (GRID_COLS + 1))) / GRID_COLS;
  uint32_t btnH = (tileH - (BTN_GAP * (GRID_ROWS + 1))) / GRID_ROWS;

  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      int idx = row * GRID_COLS + col;

      lv_obj_t *btn = lv_button_create(tile);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_pos(btn,
        BTN_GAP + col * (btnW + BTN_GAP),
        HEADER_H + BTN_GAP + row * (btnH + BTN_GAP));

      lv_obj_set_user_data(btn, (void *)(intptr_t)keys[idx]);
      lv_obj_add_event_cb(btn, page2_btn_event_cb, LV_EVENT_CLICKED, NULL);

      lv_obj_t *label = lv_label_create(btn);
      lv_label_set_text(label, labels[idx]);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(label, btnW - 10);
      lv_obj_center(label);
    }
  }
}

// ================= Page 3: fancy info display (untouched) =================
lv_obj_t *timeLabel;
lv_obj_t *dateLabel;
lv_obj_t *timeCard;
lv_obj_t *dateIcon;
lv_obj_t *weatherContainer;
lv_obj_t *weatherIcon;
lv_obj_t *weatherLabel;
lv_obj_t *weatherSubLabel;
lv_obj_t *wifiLabel;
lv_obj_t *wifiSubLabel;
lv_obj_t *tileviewGlobal;
lv_obj_t *tile3Global;

String cachedTimezone = "UTC0";
float latitude = 0.0;
float longitude = 0.0;
bool geoLookupDone = false;

unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;

#define COLOR_BLUE_BG   lv_color_hex(0x0C447C)
#define COLOR_BLUE_TXT  lv_color_hex(0xE6F1FB)
#define COLOR_BLUE_SUB  lv_color_hex(0xB5D4F4)

#define COLOR_AMBER_BG  lv_color_hex(0x854F0B)
#define COLOR_AMBER_TXT lv_color_hex(0xFAEEDA)
#define COLOR_AMBER_SUB lv_color_hex(0xFAC775)

#define COLOR_TEAL_BG   lv_color_hex(0x085041)
#define COLOR_TEAL_TXT  lv_color_hex(0xE1F5EE)
#define COLOR_TEAL_SUB  lv_color_hex(0x9FE1CB)

#define WI_SUN_CLEAR "\xEF\x80\x8D"
#define WI_SUN_CLOUD "\xEF\x80\x82"
#define WI_CLOUD     "\xEF\x81\x81"
#define WI_FOG       "\xEF\x80\x94"
#define WI_RAIN      "\xEF\x80\x99"
#define WI_SNOW      "\xEF\x80\x9B"
#define WI_THUNDER   "\xEF\x80\x9E"

#define CAL_ICON "\xEF\x81\xB3"

void set_weather_border(bool on)
{
  if (on) {
    lv_obj_set_style_border_width(weatherContainer, 3, 0);
    lv_obj_set_style_border_color(weatherContainer, lv_color_hex(0xEF9F27), 0);
  } else {
    lv_obj_set_style_border_width(weatherContainer, 0, 0);
  }
}

const char* weathercode_to_text(int code)
{
  if (code == 0) return "Clear";
  if (code <= 3) return "Partly Cloudy";
  if (code <= 48) return "Foggy";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 99) return "Thunderstorm";
  return "Unknown";
}

const char* weathercode_to_icon(int code)
{
  if (code == 0) return WI_SUN_CLEAR;
  if (code <= 3) return WI_SUN_CLOUD;
  if (code <= 48) return WI_FOG;
  if (code <= 67) return WI_RAIN;
  if (code <= 77) return WI_SNOW;
  if (code <= 82) return WI_RAIN;
  if (code <= 99) return WI_THUNDER;
  return WI_CLOUD;
}

void fetch_weather()
{
  if (WiFi.status() != WL_CONNECTED || !geoLookupDone) {
    lv_label_set_text(weatherLabel, "--");
    lv_label_set_text(weatherSubLabel, "No connection");
    return;
  }

  set_weather_border(true);

  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current_weather=true&temperature_unit=fahrenheit";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    float temp = doc["current_weather"]["temperature"];
    int code = doc["current_weather"]["weathercode"];

    char tempBuf[16];
    snprintf(tempBuf, sizeof(tempBuf), "%.0f F", temp);
    lv_label_set_text(weatherLabel, tempBuf);
    lv_label_set_text(weatherSubLabel, weathercode_to_text(code));
    lv_label_set_text(weatherIcon, weathercode_to_icon(code));

    lastWeatherFetch = millis();
  } else {
    lv_label_set_text(weatherLabel, "--");
    lv_label_set_text(weatherSubLabel, "Fetch failed");
  }

  http.end();
  set_weather_border(false);
}

void maybe_refresh_weather()
{
  if (millis() - lastWeatherFetch >= WEATHER_REFRESH_MS) {
    fetch_weather();
  }
}

void update_wifi_label()
{
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(wifiLabel, WiFi.SSID().c_str());
    char sub[64];
    snprintf(sub, sizeof(sub), "%d dBm | %s", WiFi.RSSI(), WiFi.localIP().toString().c_str());
    lv_label_set_text(wifiSubLabel, sub);
  } else {
    lv_label_set_text(wifiLabel, "Not Connected");
    lv_label_set_text(wifiSubLabel, "");
  }
}

void update_time_label()
{
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char timeBuf[16];
    char dateBuf[16];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", &timeinfo);
    lv_label_set_text(timeLabel, timeBuf);
    lv_label_set_text(dateLabel, dateBuf);
  } else {
    lv_label_set_text(timeLabel, "--:--:--");
    lv_label_set_text(dateLabel, "Syncing...");
  }
}

static void tileview_event_cb(lv_event_t *e)
{
  lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t *active = lv_tileview_get_tile_act(tv);

  if (active == tile3Global) {
    update_wifi_label();
    maybe_refresh_weather();
  }
}

lv_obj_t* make_info_card(lv_obj_t *parent, uint32_t w, uint32_t h, uint32_t y,
                          lv_color_t bg, lv_color_t txtColor, lv_color_t subColor,
                          const lv_font_t *iconFont, const char *iconSymbol,
                          lv_obj_t **outIcon,
                          lv_obj_t **outMainLabel, lv_obj_t **outSubLabel,
                          const char *initialMain, const char *initialSub)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, w, h);
  lv_obj_set_pos(card, BTN_GAP, y);
  lv_obj_set_style_bg_color(card, bg, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *icon = NULL;
  if (iconSymbol != NULL) {
    icon = lv_label_create(card);
    lv_label_set_text(icon, iconSymbol);
    lv_obj_set_style_text_color(icon, txtColor, 0);
    if (iconFont != NULL) {
      lv_obj_set_style_text_font(icon, iconFont, 0);
    }
    lv_obj_set_style_pad_left(icon, 10, 0);
    lv_obj_set_style_pad_right(icon, 6, 0);
  }
  if (outIcon != NULL) *outIcon = icon;

  lv_obj_t *textCol = lv_obj_create(card);
  lv_obj_remove_style_all(textCol);
  lv_obj_set_height(textCol, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(textCol, 1);
  lv_obj_set_flex_flow(textCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(textCol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *mainLbl = lv_label_create(textCol);
  lv_label_set_text(mainLbl, initialMain);
  lv_obj_set_style_text_color(mainLbl, txtColor, 0);
  lv_obj_set_style_text_font(mainLbl, &lv_font_montserrat_24, 0);

  lv_obj_t *subLbl = lv_label_create(textCol);
  lv_label_set_text(subLbl, initialSub);
  lv_obj_set_style_text_color(subLbl, subColor, 0);
  lv_obj_set_style_text_align(subLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(subLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(subLbl, LV_PCT(100));

  *outMainLabel = mainLbl;
  *outSubLabel = subLbl;

  return card;
}

void build_info_page(lv_obj_t *tile)
{
  uint32_t rowH = (screenHeight - HEADER_H - (BTN_GAP * 4)) / 3;
  uint32_t rowW = screenWidth - (BTN_GAP * 2);

  timeCard = make_info_card(tile, rowW, rowH, HEADER_H + BTN_GAP,
                             COLOR_BLUE_BG, COLOR_BLUE_TXT, COLOR_BLUE_SUB,
                             &calendar_icon_24, CAL_ICON, &dateIcon,
                             &timeLabel, &dateLabel,
                             "Syncing...", "Local time");

  weatherContainer = make_info_card(tile, rowW, rowH, HEADER_H + BTN_GAP * 2 + rowH,
                                     COLOR_AMBER_BG, COLOR_AMBER_TXT, COLOR_AMBER_SUB,
                                     &weather_icons_24, WI_CLOUD, &weatherIcon,
                                     &weatherLabel, &weatherSubLabel,
                                     "--", "Loading...");
  lv_obj_set_style_border_width(weatherContainer, 0, 0);

  lv_obj_t *wifiCard = make_info_card(tile, rowW, rowH, HEADER_H + BTN_GAP * 3 + rowH * 2,
                                       COLOR_TEAL_BG, COLOR_TEAL_TXT, COLOR_TEAL_SUB,
                                       NULL, LV_SYMBOL_WIFI, NULL,
                                       &wifiLabel, &wifiSubLabel,
                                       "Loading...", "");
}

void fetch_geo_and_time()
{
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin("http://ip-api.com/json/");
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    if (doc["status"] == "success") {
      cachedTimezone = doc["timezone"].as<String>();
      latitude = doc["lat"];
      longitude = doc["lon"];
      geoLookupDone = true;
      Serial.println("Geo lookup OK: " + cachedTimezone);
    }
  }
  http.end();

  configTzTime("EST5EDT,M3.2.0,M11.1.0/2", "pool.ntp.org");

  struct tm timeinfo;
  uint8_t attempts = 0;
  while (!getLocalTime(&timeinfo, 500) && attempts < 10) {
    attempts++;
  }
}

void connect_wifi()
{
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi not connected - continuing without it");
  }
}

void setup()
{
#ifdef DEV_DEVICE_INIT
  DEV_DEVICE_INIT();
#endif
  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);

  bsp_touch_init(&Wire, -1, LCD_ROTATION, 320, 480);

  Serial.begin(115200);
  delay(1500);

  USB.begin();
  Keyboard.begin();
  ConsumerControl.begin();

  connect_wifi();
  fetch_geo_and_time();

  Serial.println("Page 1 modular config + Page 2/3 unchanged");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  lv_init();
  lv_tick_set_cb(millis_cb);

#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  screenWidth = gfx->width();
  screenHeight = gfx->height();

#ifdef DIRECT_RENDER_MODE
  bufSize = screenWidth * screenHeight;
  disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  bufSize = screenWidth * 40;
  disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif

  if (!disp_draw_buf1 || !disp_draw_buf2) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  } else {
    disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp, my_disp_flush);
#ifdef DIRECT_RENDER_MODE
    lv_display_set_buffers(disp, disp_draw_buf1, disp_draw_buf2, bufSize * 2, LV_DISPLAY_RENDER_MODE_FULL);
#else
    lv_display_set_buffers(disp, disp_draw_buf1, disp_draw_buf2, bufSize * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    lv_obj_t *tv = lv_tileview_create(lv_scr_act());
    tileviewGlobal = tv;
    lv_obj_add_event_cb(tv, tileview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tile1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *tile2 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
    lv_obj_t *tile3 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
    tile3Global = tile3;

    lv_obj_t *h1 = lv_label_create(tile1);
    lv_label_set_text(h1, "Page 1 / 3");
    lv_obj_align(h1, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *h2 = lv_label_create(tile2);
    lv_label_set_text(h2, "Page 2 / 3");
    lv_obj_align(h2, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *h3 = lv_label_create(tile3);
    lv_label_set_text(h3, "Page 3 / 3");
    lv_obj_align(h3, LV_ALIGN_TOP_MID, 0, 5);

    build_page1(tile1);
    build_page2(tile2, PAGE2_KEYS, PAGE2_LABELS);
    build_info_page(tile3);

    update_time_label();
    update_wifi_label();
    fetch_weather();
  }

  Serial.println("Setup done");
}

unsigned long lastClockTick = 0;

void loop()
{
  lv_task_handler();

  if (millis() - lastClockTick >= 1000) {
    lastClockTick = millis();
    update_time_label();
  }

  static unsigned long lastWeatherCheck = 0;
  if (millis() - lastWeatherCheck >= 5000) {
    lastWeatherCheck = millis();
    maybe_refresh_weather();
  }

  delay(5);
}