#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include "time.h"
#include "esp_system.h"

// ---- microSD card (1-bit SD_MMC mode -- matches Waveshare's own
// 07_sd_test demo, and needs no chip-select toggling, unlike 4-wire SPI
// mode). Pin numbers are NOT in Waveshare's docs -- cross-referenced
// from the board's schematic (SD_SCLK/SD_MOSI/SD_MISO net names) since
// the SD card and the external 2x16 pin header share this same SPI-ish
// bus. Used for local caching (weather/geo -- see save_last_known_cache())
// and a button-config backup (see backup_button_config_to_sd()). Entirely
// optional: if no card is inserted or the mount fails, sdReady stays
// false and everything else keeps working over WiFi exactly as before.
#define SD_MMC_CLK 11
#define SD_MMC_CMD 10
#define SD_MMC_D0  9
#define SD_CACHE_DIR "/pc_companion"
#define SD_CACHE_PATH SD_CACHE_DIR "/last_known.json"
#define SD_BUTTON_BACKUP_PATH SD_CACHE_DIR "/button_backup.json"
#define SD_HA_BACKUP_PATH SD_CACHE_DIR "/ha_backup.json"
bool sdReady = false;

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include "secrets.h"

#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "es8311.h"
#include "esp_check.h"

LV_FONT_DECLARE(weather_icons_24);
LV_FONT_DECLARE(calendar_icon_24);

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDMouse Mouse;

// Trackpad transport: 1 = board sends real USB HID mouse events directly
// (no PC-side software required for the trackpad itself). Set to 0 to
// fall back to the serial+pyautogui approach instead (matches
// pc_companion_telemetry.py's MOVE/CLICK/RIGHT_CLICK listener) if the
// direct-HID route ever misbehaves on your PC. Only this flag needs to
// change -- the touch/drag detection code is identical either way.
#define TRACKPAD_USE_HID_MOUSE 1

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

// ---- Jarvis / ESP-SR: I2S microphone pins (confirmed from Waveshare's own demos) ----
#define I2S_MCK_PIN   44
#define I2S_BCK_PIN   13
#define I2S_LRCK_PIN  15
#define I2S_DOUT_PIN  16
#define I2S_DIN_PIN   14

#define SR_SAMPLE_RATE 16000
#define SR_DATA_WIDTH  I2S_DATA_BIT_WIDTH_16BIT
#define SR_MCLK_MULTIPLE (256)
#define SR_MCLK_FREQ_HZ (SR_SAMPLE_RATE * SR_MCLK_MULTIPLE)
#define SR_VOICE_VOLUME (70)

#define SR_INPUT_FORMAT     "M"
#define SR_INPUT_CHANNELS   SR_CHANNELS_MONO
#define SR_OUTPUT_CHANNELS  I2S_SLOT_MODE_MONO

static const char *TAG = "pc_companion";

I2SClass srI2s;

// Declared early (rather than down by the display/gfx setup, where the
// original single-page version had them) because build_media_page()
// and the other build_* page functions need these before setup() runs.
uint32_t screenWidth;
uint32_t screenHeight;

// ---- Button grid config: 2 cols x 4 rows = 8 per page ----
#define GRID_COLS 2
#define GRID_ROWS 4
#define BTNS_PER_PAGE (GRID_COLS * GRID_ROWS)
#define BTN_GAP 10
#define HEADER_H 30

#include "page1_buttons.h"
#include "page1b_buttons.h"
#include "jarvis_ai_screensaver.h"

// ---- Wireless button config editor: mutable, RAM-backed copies of
// PAGE1_BUTTONS / PAGE1B_BUTTONS ----
// PAGE1_BUTTONS / PAGE1B_BUTTONS (from page1_buttons.h / page1b_buttons.h)
// stay exactly as-is -- they're the compiled-in factory defaults, used to
// seed these editable copies on a never-configured board. Everything that
// actually fires a button (fire_page1_action/fire_page1b_action) or draws
// one (build_button_grid) reads from THESE arrays instead, so a button
// renamed from http://<board-ip>/ takes effect immediately, no reflash.
// Saved edits persist in NVS flash (Preferences, namespace "pcconfig") and
// are reloaded by load_button_config() on every boot.
//
// NOTE: renaming a Hotkeys button here does NOT change what you say to
// Jarvis to trigger it -- the voice command phrases (SR_COMMAND_PHRASES
// below) are compiled into the wake-word model and can't be changed
// without reflashing. Only the on-screen label/sub-label/key combo is
// live-editable.
struct EditableButton {
  char label[24];
  char subLabel[24];
  uint8_t modifier1;
  uint8_t modifier2;
  char key;
  // What this button's combo should DO on the PC side -- a program
  // path, script path, or command line, e.g.
  // "C:\pc_companion\scripts\kairos.bat" or "notepad.exe". Empty means
  // "no AHK action" -- either unmapped, or handled by a native Windows
  // shortcut (Win+L for lock needs no AHK line at all). This is what
  // pc_companion_telemetry.py reads (via GET /config) to auto-generate
  // the matching hotkeys.ahk lines -- see build_ahk_block() there.
  char action[80];
};

EditableButton livePage1[BTNS_PER_PAGE];
EditableButton livePage1b[BTNS_PER_PAGE];

// ---- HA page: separate button type, separate storage from the two
// above -- see build_ha_grid()/fire_ha_action() and load_ha_config().
// No modifier/key fields (nothing here sends a USB keypress); instead
// each button calls one Home Assistant service on one entity. Saved
// edits live in their own NVS namespace ("haconfig") and their own SD
// backup file (ha_backup.json) -- entirely separate from the
// Hotkeys/Hotkeys 2 pipeline, so HA buttons are never written into
// hotkeys.ahk and never touched by pc_companion_telemetry.py's sync.
struct HAButton {
  char label[24];
  char subLabel[24];
  char domain[16];  // e.g. "light", "switch", "scene"
  char service[16]; // e.g. "toggle", "turn_on", "turn_off"
  char entity[48];  // e.g. "light.living_room"
};

HAButton liveHA[BTNS_PER_PAGE];

// Compiled-in default actions, index-aligned with PAGE1_BUTTONS /
// PAGE1B_BUTTONS, seeded into livePage1/livePage1b on a never-configured
// board (before any NVS override). These match what was already
// hand-written in hotkeys.ahk at the time this feature was added --
// kept here (not in page1_buttons.h) since they're a PC-side path, not
// a board-side label/key concern. "" = no default, set one from the
// web editor.
//
// NOTE: the Toolbox/Mithril paths below are THE ORIGINAL AUTHOR'S OWN
// MACHINE PATHS, kept as a real-world example of the format -- THESE
// TWO BUTTONS WILL NOT WORK ON YOUR PC. Toolbox and Mithril are
// separate personal projects not released on GitHub yet. See
// docs/SETUP.md section 5, and replace these two lines with your own
// script paths (or leave them "" and set an Action from the web editor
// instead).
const char* PAGE1_DEFAULT_ACTIONS[BTNS_PER_PAGE] = {
  "C:\\pc_companion\\scripts\\kairos.bat",   // Kairos
  "C:\\pc_companion\\scripts\\telos.bat",    // Telos
  "D:\\toolbox_py\\toolbox_py\\toolbox.pyw", // Toolbox -- NOT INCLUDED, see NOTE above
  "C:\\pc_companion\\scripts\\mithril.bat",  // Mithril -- NOT INCLUDED, see NOTE above
  "",                                        // Snip
  "powershell.exe",                          // PowerShell
  "notepad.exe",                             // Notepad
  "",                                        // Lock (Win+L is native, no AHK line needed)
};
const char* PAGE1B_DEFAULT_ACTIONS[BTNS_PER_PAGE] = {
  "", "", "", "", "", "", "", "",
};

// Label widgets, stashed here when build_button_grid() creates them, so
// a saved web edit can update on-screen text immediately without
// rebuilding the grid.
lv_obj_t *page1MainLbl[BTNS_PER_PAGE];
lv_obj_t *page1SubLbl[BTNS_PER_PAGE];
lv_obj_t *page1bMainLbl[BTNS_PER_PAGE];
lv_obj_t *page1bSubLbl[BTNS_PER_PAGE];
lv_obj_t *haMainLbl[BTNS_PER_PAGE];
lv_obj_t *haSubLbl[BTNS_PER_PAGE];

Preferences pcPrefs;
WebServer server(80);

// ---- WiFi/HA live credentials (NVS-backed; see load_network_config() /
// connect_wifi() further down) -- declared up here, ahead of
// call_ha_service(), which reads liveHaHost/liveHaPort/liveHaToken.
#define WIFI_PLACEHOLDER_SSID "PUT_SSID_HERE"

// WIFI_SSID/WIFI_PASSWORD/HA_HOST/HA_PORT/HA_TOKEN are all optional in
// secrets.h -- not everyone flashing this will have WiFi/HA ready to go
// at build time, and a required-field dependency shouldn't be able to
// break the build. These #ifndefs let the sketch compile whether
// secrets.h defines all five, some, or none of them.
//
// IMPORTANT if you DO define one of these yourself: it needs an actual
// value, even if that value is an empty string -- `#define HA_PORT`
// with nothing after it is NOT the same as leaving it out, and will
// still fail to compile (the macro exists but expands to nothing,
// which breaks every place that macro is used as a function argument).
// Safe patterns are: don't write the line at all (these defaults take
// over), or write it with a real value like `#define HA_PORT 8123` or
// `#define HA_TOKEN ""`.
#ifndef WIFI_SSID
#define WIFI_SSID WIFI_PLACEHOLDER_SSID
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef HA_HOST
#define HA_HOST ""
#endif
#ifndef HA_PORT
#define HA_PORT 8123
#endif
#ifndef HA_TOKEN
#define HA_TOKEN ""
#endif

String liveWifiSsid;
String liveWifiPassword;
String liveHaHost;
int liveHaPort = 8123;
String liveHaToken;

// ---- Voice command phrases -> Page 1 (Hotkeys) button index ----
// Each entry's command_id is the index into PAGE1_BUTTONS.
// Multiple phrases can map to the same index for natural variation.
//
// NOTE: Hotkeys 2 (PAGE1B_BUTTONS, the new second bank under Hotkeys)
// is touch-only for now -- it is NOT wired into voice commands. Once
// you decide what its 8 buttons actually are, add phrase entries here
// with command_id 8-15 (index into PAGE1B_BUTTONS) the same way the
// entries below map into PAGE1_BUTTONS, and extend fire-by-voice
// dispatch in onSrEvent() to check the id range and call
// fire_page1b_action() for ids >= 8.
static const sr_cmd_t sr_commands[] = {
  {0, "KfRbS"},
  {1, "TfLbS"},
  {2, "ToLBeKS"},
  {2, "gT ToLBeKS"},
  {3, "MgvRcL"},
  {4, "SNgP"},
  {5, "PtkscL"},
  {6, "NbTgPaD"},
  {7, "LnK"},
};

// ================= Tile identity =================
// 2x3 grid: top row is the original three pages, bottom row is the
// new second-row pages added under each.
//
//   Hotkeys <--> Media    <--> Info
//      |            |             |
//   Hotkeys2 <--> Trackpad <--> System
enum {
  TILE_HOTKEYS = 0,
  TILE_MEDIA,
  TILE_INFO,
  TILE_HOTKEYS2,
  TILE_HA,
  TILE_TRACKPAD,
  TILE_SYSTEM,
  TILE_COUNT
};

static const char *TILE_NAMES[TILE_COUNT] = {
  "Hotkeys", "Media", "Info", "Hotkeys 2", "HA", "Trackpad", "System"
};

lv_obj_t *tileObjs[TILE_COUNT];
lv_obj_t *tileviewGlobal; // the tileview widget itself, needed to programmatically change tiles
lv_obj_t *tileInfoGlobal;   // triggers a weather refresh on tile-switch (see tileview_event_cb)
lv_obj_t *tileSystemGlobal; // triggers the SD-status/network card refresh on tile-switch

// ---- Header widgets (per tile): page name, Jarvis status pill, nav chevrons ----
lv_obj_t *pageLabel[TILE_COUNT];
lv_obj_t *statusPill[TILE_COUNT];
lv_obj_t *chevLabel[TILE_COUNT];

#define JARVIS_IDLE_BORDER   JSS_COLOR_CYAN_MID
#define JARVIS_IDLE_TEXT     JSS_COLOR_CYAN
// Listening state stays a distinct green -- a real state change is
// worth a color of its own rather than folding it into the cyan
// theme, same reasoning as keeping the mute button red (see
// MUTE_BTN_BG_MUTED above).
#define JARVIS_LISTEN_BORDER lv_color_hex(0x1D9E75)
#define JARVIS_LISTEN_TEXT   lv_color_hex(0x5DCAA5)


// Forward-declared: definition lives with the button-grid builders
// further down, but create_media_btn() (used by the Media page, which
// comes first in the file) needs it too.
static void style_hud_button(lv_obj_t *btn);

static void style_status_pill(lv_obj_t *label)
{
  lv_obj_set_style_border_width(label, 1, 0);
  lv_obj_set_style_border_color(label, JARVIS_IDLE_BORDER, 0);
  lv_obj_set_style_text_color(label, JARVIS_IDLE_TEXT, 0);
  lv_obj_set_style_radius(label, 20, 0);
  lv_obj_set_style_pad_hor(label, 12, 0);
  lv_obj_set_style_pad_ver(label, 4, 0);
}

static void update_jarvis_headers(bool listening)
{
  lv_color_t border = listening ? JARVIS_LISTEN_BORDER : JARVIS_IDLE_BORDER;
  lv_color_t textColor = listening ? JARVIS_LISTEN_TEXT : JARVIS_IDLE_TEXT;
  const char *stateText = listening ? "Listening..." : "Say Jarvis";

  for (int i = 0; i < TILE_COUNT; i++) {
    if (statusPill[i] != NULL) {
      lv_obj_set_style_border_color(statusPill[i], border, 0);
      lv_obj_set_style_text_color(statusPill[i], textColor, 0);
      lv_label_set_text(statusPill[i], stateText);
    }
  }
}

// Builds the standard header row for one tile: page name (left),
// "Say Jarvis" / "Listening..." pill (mid-right), and swipe-direction
// chevrons (far right) showing which edges of this tile you can swipe
// off of. Pass only the directions this tile itself allows leaving in
// (matches the LV_DIR_* flags given to lv_tileview_add_tile for it).
static void build_tile_header(int idx, lv_obj_t *tile, bool left, bool right, bool up, bool down)
{
  pageLabel[idx] = lv_label_create(tile);
  lv_label_set_text(pageLabel[idx], TILE_NAMES[idx]);
  lv_obj_align(pageLabel[idx], LV_ALIGN_TOP_LEFT, BTN_GAP + 18, 9); // nudged ~3mm right

  statusPill[idx] = lv_label_create(tile);
  style_status_pill(statusPill[idx]);
  lv_obj_align(statusPill[idx], LV_ALIGN_TOP_MID, 10, 4);

  chevLabel[idx] = lv_label_create(tile);
  char buf[32] = "";
  bool haveOne = false;
  if (left)  { if (haveOne) strcat(buf, "  "); strcat(buf, LV_SYMBOL_LEFT);  haveOne = true; }
  if (right) { if (haveOne) strcat(buf, "  "); strcat(buf, LV_SYMBOL_RIGHT); haveOne = true; }
  if (up)    { if (haveOne) strcat(buf, "  "); strcat(buf, LV_SYMBOL_UP);    haveOne = true; }
  if (down)  { if (haveOne) strcat(buf, "  "); strcat(buf, LV_SYMBOL_DOWN);  haveOne = true; }
  lv_label_set_text(chevLabel[idx], buf);
  lv_obj_align(chevLabel[idx], LV_ALIGN_TOP_RIGHT, -BTN_GAP, 9);
}

// ---- Shared action-firing logic for Hotkeys (Page 1): used by BOTH touch and voice ----
static void fire_page1_action(int idx)
{
  if (idx < 0 || idx >= BTNS_PER_PAGE) return;
  const EditableButton *b = &livePage1[idx]; // live (web-editable) copy, not the compiled default

  if (b->modifier1 != 0) Keyboard.press(b->modifier1);
  if (b->modifier2 != 0) Keyboard.press(b->modifier2);
  Keyboard.press(b->key);
  delay(50);
  Keyboard.releaseAll();
}

// ---- Same idea for Hotkeys 2 -- touch only for now, see sr_commands note above ----
static void fire_page1b_action(int idx)
{
  if (idx < 0 || idx >= BTNS_PER_PAGE) return;
  const EditableButton *b = &livePage1b[idx]; // live (web-editable) copy, not the compiled default

  if (b->modifier1 != 0) Keyboard.press(b->modifier1);
  if (b->modifier2 != 0) Keyboard.press(b->modifier2);
  Keyboard.press(b->key);
  delay(50);
  Keyboard.releaseAll();
}

// ---- Generic button-grid touch handlers ----
static void page1_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  fire_page1_action(idx);
}

static void page1b_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  fire_page1b_action(idx);
}

// ---- HA page: calls one Home Assistant service on one entity ----
// POST http://<liveHaHost>:<liveHaPort>/api/services/<domain>/<service>
// body {"entity_id": "<entity>"}, auth via long-lived token. liveHaHost/
// liveHaPort/liveHaToken are NVS-backed (see load_network_config()) so
// these can be changed via the WiFi/HA setup portal without a reflash --
// secrets.h's HA_HOST/HA_PORT/HA_TOKEN only seed NVS on a first-ever boot.
// Fire-and-forget: this board has no state to reconcile against (see
// the "could it pull a full HA profile" discussion -- not worth it for
// this screen), so it doesn't bother reading the response body.
static void call_ha_service(const char *domain, const char *service, const char *entity)
{
  if (WiFi.status() != WL_CONNECTED) return;
  if (domain[0] == '\0' || service[0] == '\0' || entity[0] == '\0') return; // unconfigured button

  HTTPClient http;
  String url = "http://" + liveHaHost + ":" + String(liveHaPort) +
               "/api/services/" + domain + "/" + service;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + liveHaToken);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["entity_id"] = entity;
  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  if (httpCode <= 0) {
    Serial.printf("[HA] call failed: %s\n", http.errorToString(httpCode).c_str());
  } else if (httpCode >= 400) {
    Serial.printf("[HA] %s/%s on %s -> HTTP %d\n", domain, service, entity, httpCode);
  }
  http.end();
}

// ---- Shared action-firing logic for the HA page: used by BOTH touch and voice ----
static void fire_ha_action(int idx)
{
  if (idx < 0 || idx >= BTNS_PER_PAGE) return;
  const HAButton *b = &liveHA[idx];
  call_ha_service(b->domain, b->service, b->entity);
}

static void ha_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  fire_ha_action(idx);
}

// ================= Media page (was Page 2) =================
// play/pause and mute/unmute are now single combined-icon buttons.
// Close App is gone. The bottom row is a Now Playing panel instead of
// two more buttons. Icon/color state is a local best-guess toggle
// updated on every press, and gets corrected by real state the moment
// telemetry from pc_companion_telemetry.py starts arriving over
// serial (see handle_telemetry_line() below).
lv_obj_t *playPauseBtn, *playPauseIcon;
lv_obj_t *muteBtn, *muteIcon;
lv_obj_t *nowPlayingTrack, *nowPlayingArtist;
bool guessIsPlaying = false;
bool guessIsMuted = false;

#define MUTE_BTN_BG_NORMAL lv_color_hex(0x0a151f) // matches style_hud_button()'s panel bg
#define MUTE_BTN_BG_MUTED  lv_color_hex(0xB33A3A) // kept as a clear red alert -- semantic, not thematic

static void update_playpause_visual(bool playing)
{
  lv_label_set_text(playPauseIcon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void update_mute_visual(bool muted)
{
  // Always show the mute glyph (speaker+slash) so this button reads as
  // "the mute button" at a glance -- it used to swap to
  // LV_SYMBOL_VOLUME_MAX when unmuted, which is the exact same icon as
  // the real volume-up button elsewhere on this page and made the two
  // impossible to tell apart. State is communicated by color instead:
  // gray = not muted (tap to mute), red = muted (tap to unmute).
  lv_label_set_text(muteIcon, LV_SYMBOL_MUTE);
  lv_obj_set_style_bg_color(muteBtn, muted ? MUTE_BTN_BG_MUTED : MUTE_BTN_BG_NORMAL, 0);
}

static void update_now_playing(const String &track, const String &artist)
{
  lv_label_set_text(nowPlayingTrack, track.length() ? track.c_str() : "Not Playing");
  lv_label_set_text(nowPlayingArtist, artist.c_str());
}

// ---- Media page button handler ----
static void page2_btn_event_cb(lv_event_t *e)
{
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  char key = (char)(intptr_t)lv_obj_get_user_data(btn);

  switch (key) {
    case 'p': // combined play/pause
      ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE); delay(50); ConsumerControl.release();
      guessIsPlaying = !guessIsPlaying;
      update_playpause_visual(guessIsPlaying);
      break;
    case 'm': // combined mute/unmute
      ConsumerControl.press(CONSUMER_CONTROL_MUTE); delay(50); ConsumerControl.release();
      guessIsMuted = !guessIsMuted;
      update_mute_visual(guessIsMuted);
      break;
    case 'n': ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT); delay(50); ConsumerControl.release(); break;
    case 'v': ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS); delay(50); ConsumerControl.release(); break;
    case 'd': ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT); delay(50); ConsumerControl.release(); break;
    case 'u': ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT); delay(50); ConsumerControl.release(); break;
    default:
      Keyboard.press(key); delay(50); Keyboard.release(key);
      break;
  }
}

static lv_obj_t* create_media_btn(lv_obj_t *tile, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   char key, const char *symbol)
{
  lv_obj_t *btn = lv_button_create(tile);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_user_data(btn, (void *)(intptr_t)key);
  lv_obj_add_event_cb(btn, page2_btn_event_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(btn);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, symbol);
  lv_obj_center(lbl);
  return btn;
}

void build_media_page(lv_obj_t *tile)
{
  uint32_t tileW = screenWidth;
  uint32_t tileH = screenHeight - HEADER_H;
  uint32_t btnW = (tileW - (BTN_GAP * (GRID_COLS + 1))) / GRID_COLS;
  uint32_t btnH = (tileH - (BTN_GAP * (GRID_ROWS + 1))) / GRID_ROWS;

  uint32_t row0Y = HEADER_H + BTN_GAP;
  uint32_t row1Y = row0Y + btnH + BTN_GAP;
  uint32_t row2Y = row1Y + btnH + BTN_GAP;
  uint32_t row3Y = row2Y + btnH + BTN_GAP;
  uint32_t col0X = BTN_GAP;
  uint32_t col1X = BTN_GAP + btnW + BTN_GAP;

  // Row 0: combined play/pause, combined mute/unmute
  playPauseBtn = lv_button_create(tile);
  lv_obj_set_size(playPauseBtn, btnW, btnH);
  lv_obj_set_pos(playPauseBtn, col0X, row0Y);
  lv_obj_set_user_data(playPauseBtn, (void *)(intptr_t)'p');
  lv_obj_add_event_cb(playPauseBtn, page2_btn_event_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(playPauseBtn);
  playPauseIcon = lv_label_create(playPauseBtn);
  lv_label_set_text(playPauseIcon, LV_SYMBOL_PLAY);
  lv_obj_center(playPauseIcon);

  muteBtn = lv_button_create(tile);
  lv_obj_set_size(muteBtn, btnW, btnH);
  lv_obj_set_pos(muteBtn, col1X, row0Y);
  lv_obj_set_user_data(muteBtn, (void *)(intptr_t)'m');
  lv_obj_add_event_cb(muteBtn, page2_btn_event_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(muteBtn);
  muteIcon = lv_label_create(muteBtn);
  lv_label_set_text(muteIcon, LV_SYMBOL_MUTE); // see update_mute_visual() -- icon is fixed, color shows state
  lv_obj_center(muteIcon);

  // Row 1: previous / next -- unchanged
  create_media_btn(tile, col0X, row1Y, btnW, btnH, 'v', LV_SYMBOL_PREV);
  create_media_btn(tile, col1X, row1Y, btnW, btnH, 'n', LV_SYMBOL_NEXT);

  // Row 2: vol- / vol+ -- unchanged
  create_media_btn(tile, col0X, row2Y, btnW, btnH, 'd', LV_SYMBOL_VOLUME_MID);
  create_media_btn(tile, col1X, row2Y, btnW, btnH, 'u', LV_SYMBOL_VOLUME_MAX);

  // Row 3: Now Playing panel, spans both columns (replaces the old
  // separate Pause button + Close App button)
  lv_obj_t *npPanel = lv_obj_create(tile);
  lv_obj_set_size(npPanel, tileW - BTN_GAP * 2, btnH);
  lv_obj_set_pos(npPanel, BTN_GAP, row3Y);
  lv_obj_set_style_radius(npPanel, 10, 0);
  lv_obj_set_style_bg_color(npPanel, lv_color_hex(0x0a151f), 0);
  lv_obj_set_style_bg_opa(npPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(npPanel, JSS_COLOR_CYAN_DIM, 0);
  lv_obj_set_style_border_width(npPanel, 1, 0);
  lv_obj_set_style_pad_all(npPanel, 4, 0);
  lv_obj_clear_flag(npPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(npPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(npPanel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  nowPlayingTrack = lv_label_create(npPanel);
  lv_label_set_text(nowPlayingTrack, "Not Playing");
  lv_obj_set_style_text_color(nowPlayingTrack, JSS_COLOR_CORE, 0);

  nowPlayingArtist = lv_label_create(npPanel);
  lv_label_set_text(nowPlayingArtist, "");
  lv_obj_set_style_text_color(nowPlayingArtist, JSS_COLOR_CYAN_MID, 0);
}

// ---- Wake-word -> screensaver hand-off flags ----
// onSrEvent() below fires from the ESP_SR library's own task, not from
// loop()/LVGL's task, so it must NOT call jarvis_screensaver_wake_show()
// or jarvis_screensaver_wake_dismiss() directly -- those call into LVGL,
// which isn't thread-safe, and jss_apply_state() is heavy enough
// (animations + a full-screen invalidate) that doing it from the wrong
// task risks the same class of crash the button-styling bug caused, just
// from a different mechanism (a cross-task race instead of a memory-pool
// exhaustion). Plain bools are enough here (no mutex/queue needed) since
// each is only ever set true by onSrEvent() and cleared by loop() --
// a torn read in the worst case just delays the pop-open/dismiss by one
// more loop() pass, never corrupts anything.
static volatile bool jssWakeShowPending = false;
static volatile bool jssWakeDismissPending = false;

// update_jarvis_headers() was being called directly from onSrEvent()
// below (the SR task, not loop()/LVGL's thread) on the theory that it
// was "already safe" -- it had run this way for a while with no visible
// problem, unlike the much heavier jss_apply_state() work. That
// assumption doesn't hold: it doesn't just set style colors, it also
// calls lv_label_set_text() on every status pill across every page,
// switching between "Listening..." and "Say Jarvis" -- different
// lengths, so LVGL actually reallocates that label's text buffer out of
// its memory pool each time. That's a real cross-thread allocation, now
// happening on every wake/timeout event instead of rarely, and a freeze
// persisted even after removing the one forced redraw we'd already
// found and fixed -- pointing at this as the remaining culprit. Same
// treatment as the screensaver calls: onSrEvent() only records what the
// headers should say, loop() is what actually touches LVGL.
static volatile bool jssHeaderUpdatePending = false;
static volatile bool jssHeaderListening = false;

// ---- Jarvis event handler ----
static void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
      Serial.println("Jarvis: WakeWord Detected!");
      jssHeaderListening = true;
      jssHeaderUpdatePending = true;
      jssWakeShowPending = true;
      if (strlen(SR_INPUT_FORMAT) == 1) {
        ESP_SR.setMode(SR_MODE_COMMAND);
      }
      break;
    case SR_EVENT_WAKEWORD_CHANNEL:
      Serial.printf("Jarvis: WakeWord Channel %d Verified!\n", command_id);
      jssHeaderListening = true;
      jssHeaderUpdatePending = true;
      jssWakeShowPending = true;
      ESP_SR.setMode(SR_MODE_COMMAND);
      break;
    case SR_EVENT_TIMEOUT:
      Serial.println("Jarvis: Timeout, back to listening for wake word");
      jssHeaderListening = false;
      jssHeaderUpdatePending = true;
      jssWakeDismissPending = true;
      ESP_SR.setMode(SR_MODE_WAKEWORD);
      break;
    case SR_EVENT_COMMAND:
      Serial.printf("Jarvis: Command ID %d Detected -> %s\n", command_id, PAGE1_BUTTONS[command_id].label);
      fire_page1_action(command_id);
      ESP_SR.setMode(SR_MODE_COMMAND);
      break;
    default:
      Serial.println("Jarvis: Unknown Event!");
      break;
  }
}

static esp_err_t es8311_codec_init(void) {
  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SR_MCLK_FREQ_HZ,
    .sample_frequency = SR_SAMPLE_RATE
  };
  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, SR_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
  return ESP_OK;
}

void setup_jarvis()
{
  // Note: Wire.begin(I2C_SDA, I2C_SCL) is already called earlier in
  // setup() for the touch controller / TCA9554 -- do not call it again
  esp_err_t codecResult = es8311_codec_init();
  if (codecResult != ESP_OK) {
    Serial.println("Jarvis: ES8311 CODEC INIT FAILED");
    return;
  }
  Serial.println("Jarvis: ES8311 codec initialized OK");

  srI2s.setTimeout(1000);
  srI2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, -1, I2S_DIN_PIN, I2S_MCK_PIN);

  bool i2sOk = srI2s.begin(I2S_MODE_STD, SR_SAMPLE_RATE, SR_DATA_WIDTH, SR_OUTPUT_CHANNELS, I2S_STD_SLOT_LEFT);
  if (!i2sOk) {
    Serial.println("Jarvis: I2S FAILED TO START");
    return;
  }
  Serial.println("Jarvis: I2S started OK");

  ESP_SR.onEvent(onSrEvent);
  bool srOk = ESP_SR.begin(srI2s, sr_commands, sizeof(sr_commands) / sizeof(sr_cmd_t), SR_INPUT_CHANNELS, SR_MODE_WAKEWORD, SR_INPUT_FORMAT);
  if (!srOk) {
    Serial.println("Jarvis: ESP_SR FAILED TO START");
    return;
  }
  Serial.println("Jarvis: listening for wake word");
}

TCA9554 TCA(0x20);

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *gfx = new Arduino_AXS15231B(bus, -1, LCD_ROTATION, false, 320, 480);

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

// ---- Shared HUD styling -- REBUILT as shared lv_style_t objects instead
// of per-object local styles. History, for whoever reads this next:
//   1. Original version called lv_obj_set_style_*() directly on every
//      button/card (~20 objects, 6-7 properties each). That's LVGL's
//      "local style" path -- each call allocates its own property storage
//      out of LV_MEM_SIZE (this build's fixed 64KB pool), permanently,
//      per object. Looked fine on the bench.
//   2. First real symptom: pressing BOOT to force the screensaver open
//      reliably panicked the board (Reset reason: PANIC (exception/abort)),
//      immediately inside lv_task_handler()'s render pass. First theory
//      (shadow styles fragmenting the ESP32 heap) was DISPROVEN by
//      logging heap_caps_get_largest_free_block(): it went from ~12KB to
//      24-31KB after removing shadows, with the identical crash still
//      happening. That ruled out ESP-heap exhaustion.
//   3. A bisection build (skip all local button/card styling, and
//      separately flip LV_THEME_DEFAULT_DARK back to 0) made the crash
//      disappear entirely. Re-enabling just the theme flip (dark theme
//      ON, styling still off) did NOT bring the crash back -- confirming
//      the ~20 local-style objects were the actual trigger, not the
//      theme. Almost certainly: LV_MEM_SIZE is a SEPARATE fixed pool from
//      the ESP32 heap the diagnostic print was watching -- lv_style
//      property storage, lv_anim_t nodes (jss_apply_state() starts/stops
//      four animations on every BOOT press) and this board's own display
//      draw buffers all come out of that same 64KB. ~20 objects x 6-7
//      properties is enough local-style storage, on top of everything
//      else already living in that pool, to leave too little headroom for
//      the render pass's own allocations -- explaining why it was
//      deterministic and independent of ESP-heap headroom the whole time.
//   4. Fix: lv_obj_add_style() with a small number of SHARED lv_style_t
//      objects, initialized once. Every object referencing the same
//      style costs one pointer, not its own copy of the property list --
//      this is the difference between ~20 local-style allocations and 3
//      total, for the identical visual result.
static lv_style_t style_hud_btn_main;     // bg + border + radius + text, default state
static lv_style_t style_hud_btn_pressed;  // border brightens on press
static lv_style_t style_hud_card_border;  // border + radius only -- cards set their own bg_color per call (varies)
static bool hud_styles_inited = false;

static void ensure_hud_styles_inited()
{
  if (hud_styles_inited) return;
  hud_styles_inited = true;

  lv_style_init(&style_hud_btn_main);
  lv_style_set_bg_color(&style_hud_btn_main, lv_color_hex(0x0a151f));
  lv_style_set_bg_opa(&style_hud_btn_main, LV_OPA_COVER);
  lv_style_set_border_color(&style_hud_btn_main, JSS_COLOR_CYAN_DIM);
  lv_style_set_border_width(&style_hud_btn_main, 1);
  lv_style_set_radius(&style_hud_btn_main, 9);
  lv_style_set_text_color(&style_hud_btn_main, JSS_COLOR_CORE);

  // Pressed feedback: brighten the border instead of the theme's default
  // darken-tint, which read muddy on top of a dark card.
  lv_style_init(&style_hud_btn_pressed);
  lv_style_set_border_color(&style_hud_btn_pressed, JSS_COLOR_CYAN);

  lv_style_init(&style_hud_card_border);
  lv_style_set_border_color(&style_hud_card_border, JSS_COLOR_CYAN_DIM);
  lv_style_set_border_width(&style_hud_card_border, 1);
  lv_style_set_radius(&style_hud_card_border, 14);
}

static void style_hud_button(lv_obj_t *btn)
{
  ensure_hud_styles_inited();
  lv_obj_add_style(btn, &style_hud_btn_main, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_style(btn, &style_hud_btn_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
}

// Generic button-grid builder -- used for both Hotkeys and Hotkeys 2,
// parameterized on which (mutable, EditableButton) array and which touch
// handler to use. outMainLbl/outSubLbl (each BTNS_PER_PAGE long) receive
// the created label widgets so a later web-based edit can update their
// text live -- see apply_button_edit() below. The sub-label widget is
// always created (even for an empty string) and just hidden when blank,
// so a button that starts with no sub-label can still gain one later
// without needing a rebuild/reboot.
void build_button_grid(lv_obj_t *tile, const EditableButton *buttons, lv_event_cb_t cb,
                        lv_obj_t **outMainLbl, lv_obj_t **outSubLbl)
{
  uint32_t tileW = screenWidth;
  uint32_t tileH = screenHeight - HEADER_H;
  uint32_t btnW = (tileW - (BTN_GAP * (GRID_COLS + 1))) / GRID_COLS;
  uint32_t btnH = (tileH - (BTN_GAP * (GRID_ROWS + 1))) / GRID_ROWS;

  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      int idx = row * GRID_COLS + col;
      const EditableButton *b = &buttons[idx];

      lv_obj_t *btn = lv_button_create(tile);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_pos(btn,
        BTN_GAP + col * (btnW + BTN_GAP),
        HEADER_H + BTN_GAP + row * (btnH + BTN_GAP));

      lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
      lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
      style_hud_button(btn);

      // Flex layout applied directly to the button (not a wrapper
      // object) so touch detection matches Page 2's proven-working
      // structure exactly -- big label on top, faint sub-label
      // underneath (only if provided)
      lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      lv_obj_t *mainLbl = lv_label_create(btn);
      lv_label_set_text(mainLbl, b->label);
      lv_obj_set_style_text_align(mainLbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(mainLbl, btnW - 10);

      lv_obj_t *subLbl = lv_label_create(btn);
      lv_label_set_text(subLbl, b->subLabel);
      lv_obj_set_style_text_align(subLbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_opa(subLbl, LV_OPA_80, 0);
      lv_obj_set_style_text_color(subLbl, JSS_COLOR_CYAN_MID, 0);
      lv_obj_set_width(subLbl, btnW - 10);
      if (strlen(b->subLabel) == 0) {
        lv_obj_add_flag(subLbl, LV_OBJ_FLAG_HIDDEN);
      }

      if (outMainLbl) outMainLbl[idx] = mainLbl;
      if (outSubLbl)  outSubLbl[idx]  = subLbl;
    }
  }
}

// Same layout as build_button_grid() above, but typed to HAButton --
// kept as a separate function rather than templated/reused because
// HAButton and EditableButton aren't related types (no domain/service/
// entity vs modifier/key overlap), so a shared function can't take
// either without a cast.
void build_ha_grid(lv_obj_t *tile, const HAButton *buttons, lv_event_cb_t cb,
                    lv_obj_t **outMainLbl, lv_obj_t **outSubLbl)
{
  uint32_t tileW = screenWidth;
  uint32_t tileH = screenHeight - HEADER_H;
  uint32_t btnW = (tileW - (BTN_GAP * (GRID_COLS + 1))) / GRID_COLS;
  uint32_t btnH = (tileH - (BTN_GAP * (GRID_ROWS + 1))) / GRID_ROWS;

  for (int row = 0; row < GRID_ROWS; row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      int idx = row * GRID_COLS + col;
      const HAButton *b = &buttons[idx];

      lv_obj_t *btn = lv_button_create(tile);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_pos(btn,
        BTN_GAP + col * (btnW + BTN_GAP),
        HEADER_H + BTN_GAP + row * (btnH + BTN_GAP));

      lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
      lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
      style_hud_button(btn);

      lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      lv_obj_t *mainLbl = lv_label_create(btn);
      lv_label_set_text(mainLbl, b->label);
      lv_obj_set_style_text_align(mainLbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(mainLbl, btnW - 10);

      lv_obj_t *subLbl = lv_label_create(btn);
      lv_label_set_text(subLbl, b->subLabel);
      lv_obj_set_style_text_align(subLbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_opa(subLbl, LV_OPA_80, 0);
      lv_obj_set_style_text_color(subLbl, JSS_COLOR_CYAN_MID, 0);
      lv_obj_set_width(subLbl, btnW - 10);
      if (strlen(b->subLabel) == 0) {
        lv_obj_add_flag(subLbl, LV_OBJ_FLAG_HIDDEN);
      }

      if (outMainLbl) outMainLbl[idx] = mainLbl;
      if (outSubLbl)  outSubLbl[idx]  = subLbl;
    }
  }
}

// ================= Trackpad page =================
// Full-tile touch-drag area feeding USBHIDMouse (or, if
// TRACKPAD_USE_HID_MOUSE is set to 0, serial MOVE/CLICK/RIGHT_CLICK
// lines for pc_companion_telemetry.py's pyautogui listener to pick
// up). This tile deliberately has NO tileview swipe directions
// enabled (see the dir flags where it's added in setup()) -- any
// drag gesture here is mouse movement, so it can't also mean "swipe
// to the next page" the way every other tile's gestures do. Getting
// back out uses the three nav buttons in the header area instead
// (left -> Hotkeys 2, up -> Media, right -> System -- this tile's
// three neighbors in the 2x3 grid). This tile also skips the shared
// build_tile_header() page-name label entirely -- three 40px buttons
// plus the Jarvis status pill don't leave room for it on a 320px-wide
// header, so build_trackpad_page() builds its own header below
// instead of calling build_tile_header().
static int tpPrevX = 0, tpPrevY = 0;
static bool tpDragged = false;

static void trackpad_send_move(int dx, int dy)
{
#if TRACKPAD_USE_HID_MOUSE
  Mouse.move(dx, dy);
#else
  Serial.printf("MOVE,%d,%d\n", dx, dy);
#endif
}

static void trackpad_send_click(bool rightClick)
{
#if TRACKPAD_USE_HID_MOUSE
  if (rightClick) Mouse.click(MOUSE_RIGHT);
  else Mouse.click(MOUSE_LEFT);
#else
  Serial.println(rightClick ? "RIGHT_CLICK" : "CLICK");
#endif
}

static void trackpad_area_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (indev == NULL) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  if (code == LV_EVENT_PRESSED) {
    tpPrevX = p.x;
    tpPrevY = p.y;
    tpDragged = false;
  } else if (code == LV_EVENT_PRESSING) {
    int dx = (p.x - tpPrevX) * 2; // sensitivity multiplier, tune to taste
    int dy = (p.y - tpPrevY) * 2;
    if (dx != 0 || dy != 0) {
      trackpad_send_move(dx, dy);
      tpPrevX = p.x;
      tpPrevY = p.y;
      tpDragged = true;
    }
  } else if (code == LV_EVENT_RELEASED) {
    if (!tpDragged) {
      trackpad_send_click(false); // quick tap = left click
    }
  }
}

static void trackpad_left_btn_cb(lv_event_t *e) { trackpad_send_click(false); }
static void trackpad_right_btn_cb(lv_event_t *e) { trackpad_send_click(true); }

// Header nav buttons -- named "nav" to keep them distinct from the two
// mouse-click callbacks just above (trackpad_left_btn_cb /
// trackpad_right_btn_cb), which are unrelated: those fire a mouse
// click, these change tiles. lv_obj_scroll_to_view() does NOT change
// the tileview's active tile -- that needs the tileview's own
// navigation call, lv_obj_set_tile(), pointed at the tileview widget
// itself (not the tile object).
static void trackpad_nav_left_cb(lv_event_t *e)
{
  lv_obj_set_tile(tileviewGlobal, tileObjs[TILE_HOTKEYS2], LV_ANIM_ON);
}
static void trackpad_nav_up_cb(lv_event_t *e)
{
  lv_obj_set_tile(tileviewGlobal, tileObjs[TILE_MEDIA], LV_ANIM_ON);
}
static void trackpad_nav_right_cb(lv_event_t *e)
{
  lv_obj_set_tile(tileviewGlobal, tileObjs[TILE_SYSTEM], LV_ANIM_ON);
}

void build_trackpad_page(lv_obj_t *tile)
{
  // No page-name label on this tile -- see the comment above this
  // function for why. Just the Jarvis status pill (top-left, where
  // the label would normally sit) and three nav buttons (top-right):
  // left -> Hotkeys 2, up -> Media, right -> System.
  statusPill[TILE_TRACKPAD] = lv_label_create(tile);
  style_status_pill(statusPill[TILE_TRACKPAD]);
  lv_label_set_text(statusPill[TILE_TRACKPAD], "Say Jarvis");
  lv_obj_align(statusPill[TILE_TRACKPAD], LV_ALIGN_TOP_LEFT, BTN_GAP, 4);

  lv_obj_t *navLeftBtn = lv_button_create(tile);
  lv_obj_set_size(navLeftBtn, 40, HEADER_H - 4);
  lv_obj_align(navLeftBtn, LV_ALIGN_TOP_RIGHT, -98, 2);
  lv_obj_add_event_cb(navLeftBtn, trackpad_nav_left_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(navLeftBtn);
  lv_obj_t *navLeftIcon = lv_label_create(navLeftBtn);
  lv_label_set_text(navLeftIcon, LV_SYMBOL_LEFT);
  lv_obj_center(navLeftIcon);

  lv_obj_t *navUpBtn = lv_button_create(tile);
  lv_obj_set_size(navUpBtn, 40, HEADER_H - 4);
  lv_obj_align(navUpBtn, LV_ALIGN_TOP_RIGHT, -54, 2);
  lv_obj_add_event_cb(navUpBtn, trackpad_nav_up_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(navUpBtn);
  lv_obj_t *navUpIcon = lv_label_create(navUpBtn);
  lv_label_set_text(navUpIcon, LV_SYMBOL_UP);
  lv_obj_center(navUpIcon);

  lv_obj_t *navRightBtn = lv_button_create(tile);
  lv_obj_set_size(navRightBtn, 40, HEADER_H - 4);
  lv_obj_align(navRightBtn, LV_ALIGN_TOP_RIGHT, -BTN_GAP, 2);
  lv_obj_add_event_cb(navRightBtn, trackpad_nav_right_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(navRightBtn);
  lv_obj_t *navRightIcon = lv_label_create(navRightBtn);
  lv_label_set_text(navRightIcon, LV_SYMBOL_RIGHT);
  lv_obj_center(navRightIcon);

  uint32_t bottomBtnH = 50;
  uint32_t dragH = screenHeight - HEADER_H - BTN_GAP * 3 - bottomBtnH;

  lv_obj_t *dragArea = lv_obj_create(tile);
  lv_obj_set_size(dragArea, screenWidth - BTN_GAP * 2, dragH);
  lv_obj_set_pos(dragArea, BTN_GAP, HEADER_H + BTN_GAP);
  lv_obj_add_flag(dragArea, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(dragArea, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(dragArea, trackpad_area_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_bg_color(dragArea, lv_color_hex(0x0a151f), 0);
  lv_obj_set_style_bg_opa(dragArea, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(dragArea, JSS_COLOR_CYAN_DIM, 0);
  lv_obj_set_style_border_width(dragArea, 1, 0);
  lv_obj_set_style_radius(dragArea, 10, 0);

  lv_obj_t *hint = lv_label_create(dragArea);
  lv_label_set_text(hint, "Drag to move");
  lv_obj_center(hint);
  lv_obj_set_style_text_color(hint, JSS_COLOR_CYAN_MID, 0);
  lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);

  uint32_t btnY = HEADER_H + BTN_GAP * 2 + dragH;
  uint32_t btnW = (screenWidth - BTN_GAP * 3) / 2;

  lv_obj_t *leftBtn = lv_button_create(tile);
  lv_obj_set_size(leftBtn, btnW, bottomBtnH);
  lv_obj_set_pos(leftBtn, BTN_GAP, btnY);
  lv_obj_add_event_cb(leftBtn, trackpad_left_btn_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(leftBtn);
  lv_obj_t *leftLbl = lv_label_create(leftBtn);
  lv_label_set_text(leftLbl, "LEFT");
  lv_obj_center(leftLbl);

  lv_obj_t *rightBtn = lv_button_create(tile);
  lv_obj_set_size(rightBtn, btnW, bottomBtnH);
  lv_obj_set_pos(rightBtn, BTN_GAP * 2 + btnW, btnY);
  lv_obj_add_event_cb(rightBtn, trackpad_right_btn_cb, LV_EVENT_CLICKED, NULL);
  style_hud_button(rightBtn);
  lv_obj_t *rightLbl = lv_label_create(rightBtn);
  lv_label_set_text(rightLbl, "RIGHT");
  lv_obj_center(rightLbl);
}

// ================= Info page (Page 3) =================
// Blue card: time+date on one line, plus a 7-day week strip underneath
// with today highlighted -- see build_blue_card()/update_week_strip().
lv_obj_t *timeDateLabel;
lv_obj_t *weekCells[7];     // one background "chip" per day -- today gets filled in
lv_obj_t *weekDayLabels[7]; // "S" "M" "T" "W" "T" "F" "S"
lv_obj_t *weekNumLabels[7]; // day-of-month under each letter

// Orange card: today's conditions plus tomorrow's outlook underneath --
// see build_weather_card()/fetch_weather().
lv_obj_t *weatherIcon;
lv_obj_t *weatherLabel;         // "75 F"
lv_obj_t *weatherSubLabel;      // "Clear  |  H:79 L:61  |  Rain 10%"
lv_obj_t *weatherTomorrowIcon;
lv_obj_t *weatherTomorrowLabel; // "Tomorrow   H:81 L:64"

// Green card: US timezones (ET/CT/MT/PT), zone name stacked over a big
// time, four columns separated by thin dividers -- see
// build_timezone_card()/update_timezone_labels(). The board's own
// clock is always kept on Eastern time (see fetch_geo_and_time()'s
// configTzTime call), so the other three zones are just fixed-hour
// offsets from it: all four share the same DST start/end dates, so no
// separate per-zone DST handling is needed.
lv_obj_t *tzZoneLabels[4];
lv_obj_t *tzTimeLabels[4];
const char *TZ_NAMES[4] = {"ET", "CT", "MT", "PT"};
const int TZ_HOUR_OFFSETS[4] = {0, -1, -2, -3};

String cachedTimezone = "UTC0";
float latitude = 0.0;
float longitude = 0.0;
bool geoLookupDone = false;

unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL; // 10 minutes

// Last successfully fetched weather -- mirrored to SD (see
// save_last_known_cache()) so a reboot before WiFi/the weather API
// responds can show "last known" instead of a blank/"--" display.
float lastWeatherTemp = NAN;
int lastWeatherCode = -1;
float lastTodayHigh = NAN;
float lastTodayLow = NAN;
int lastRainPct = -1;
int lastTomorrowCode = -1;
float lastTomorrowHigh = NAN;
float lastTomorrowLow = NAN;

// "Last saved" timestamps for the two SD-backed items, shown on the
// System page's Orange card (see update_sd_status_card()). Set at save
// time and also restored from the JSON files themselves at boot (see
// load_last_known_cache()/restore_button_config_from_sd()) so the card
// is accurate even before anything saves again this session.
String sdWeatherSavedAt = "";
String sdButtonSavedAt = "";
String sdHaSavedAt = "";

// "9/03 2:14p" style -- used for the two SD "last saved" timestamps
// above. Returns "" if the clock hasn't synced yet.
String current_timestamp_str()
{
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return "";
  int h12 = ti.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  const char *ap = (ti.tm_hour < 12) ? "a" : "p";
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%02d %d:%02d%s", ti.tm_mon + 1, ti.tm_mday, h12, ti.tm_min, ap);
  return String(buf);
}

// ================= microSD: bring-up + local caching =================
// Mounts the card in 1-bit SD_MMC mode (matches Waveshare's own demo --
// see the SD_MMC_CLK/CMD/D0 comment near the includes). Non-fatal if it
// fails: sdReady just stays false and every caller below no-ops.
void setup_sd_card()
{
  if (!SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)) {
    Serial.println("[SD] setPins() failed");
    return;
  }
  if (!SD_MMC.begin("/sdcard", true)) { // true = 1-bit mode
    Serial.println("[SD] No card detected / mount failed -- caching and config "
                    "backup are disabled for this boot, everything else is unaffected.");
    return;
  }
  sdReady = true;
  Serial.printf("[SD] Mounted OK (%llu MB)\n", (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
  if (!SD_MMC.exists(SD_CACHE_DIR)) {
    SD_MMC.mkdir(SD_CACHE_DIR);
  }
}

// Mirrors the last successfully fetched geo lookup + weather to SD.
// Called after each successful fetch_geo_and_time()/fetch_weather() --
// see those functions. Cheap (one small JSON file), so no throttling.
void save_last_known_cache()
{
  if (!sdReady) return;
  JsonDocument doc;
  doc["lat"] = latitude;
  doc["lon"] = longitude;
  doc["timezone"] = cachedTimezone;
  doc["weather_temp"] = lastWeatherTemp;
  doc["weather_code"] = lastWeatherCode;
  doc["today_high"] = lastTodayHigh;
  doc["today_low"] = lastTodayLow;
  doc["rain_pct"] = lastRainPct;
  doc["tomorrow_code"] = lastTomorrowCode;
  doc["tomorrow_high"] = lastTomorrowHigh;
  doc["tomorrow_low"] = lastTomorrowLow;
  sdWeatherSavedAt = current_timestamp_str();
  doc["saved_at"] = sdWeatherSavedAt;

  File f = SD_MMC.open(SD_CACHE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] Could not open last_known.json for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
}

// Loads whatever was last cached and shows it immediately -- so the
// Info page shows "last known" weather instead of a blank "--" while
// waiting on WiFi/the weather API this boot. Call AFTER the weather
// widgets exist (build_info_page()) but before/around the first real
// fetch_weather() call in setup(). A later successful fetch_weather()
// overwrites this with live data as normal.
void load_last_known_cache()
{
  if (!sdReady || !SD_MMC.exists(SD_CACHE_PATH)) return;
  File f = SD_MMC.open(SD_CACHE_PATH, FILE_READ);
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("[SD] last_known.json failed to parse -- ignoring cache");
    return;
  }

  sdWeatherSavedAt = doc["saved_at"] | "";

  float cachedLat = doc["lat"] | 0.0f;
  float cachedLon = doc["lon"] | 0.0f;
  if (cachedLat != 0.0f || cachedLon != 0.0f) {
    latitude = cachedLat;
    longitude = cachedLon;
    cachedTimezone = doc["timezone"] | "UTC0";
    // Lets fetch_weather() proceed the moment WiFi connects, using last
    // known coordinates, instead of waiting on a fresh geo lookup too.
    geoLookupDone = true;
  }

  if (doc["weather_temp"].is<float>()) {
    lastWeatherTemp = doc["weather_temp"];
    lastWeatherCode = doc["weather_code"] | -1;
    lastTodayHigh = doc["today_high"] | NAN;
    lastTodayLow = doc["today_low"] | NAN;
    lastRainPct = doc["rain_pct"] | -1;
    lastTomorrowCode = doc["tomorrow_code"] | -1;
    lastTomorrowHigh = doc["tomorrow_high"] | NAN;
    lastTomorrowLow = doc["tomorrow_low"] | NAN;
    apply_weather_display(lastWeatherTemp, lastWeatherCode, lastTodayHigh, lastTodayLow, lastRainPct,
                           lastTomorrowCode, lastTomorrowHigh, lastTomorrowLow);
    Serial.println("[SD] Showing cached weather until a fresh fetch succeeds");
  }
}

// ---- 16-button config backup/restore ----
// Mirrors the exact same data already persisted to NVS (Preferences) to
// a JSON file on SD, so a factory reset / erased flash / bad NVS write
// isn't a total loss. Called once per Save All (see handle_save()), not
// once per button -- 16 individual SD writes per save is unnecessary.
void backup_button_config_to_sd()
{
  if (!sdReady) return;
  JsonDocument doc;
  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (int page = 1; page <= 2; page++) {
    EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
    for (int i = 0; i < BTNS_PER_PAGE; i++) {
      JsonObject b = buttons.add<JsonObject>();
      b["page"] = page;
      b["idx"] = i;
      b["lbl"] = arr[i].label;
      b["sub"] = arr[i].subLabel;
      b["m1"] = code_from_mod(arr[i].modifier1);
      b["m2"] = code_from_mod(arr[i].modifier2);
      b["key"] = String(arr[i].key);
      b["action"] = arr[i].action;
    }
  }
  sdButtonSavedAt = current_timestamp_str();
  doc["saved_at"] = sdButtonSavedAt;

  File f = SD_MMC.open(SD_BUTTON_BACKUP_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] Could not open button_backup.json for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[SD] Button config backed up");
}

// Restores button config from the SD backup -- only used as a fallback
// for a genuinely blank NVS (factory reset / erased flash), checked
// from load_button_config() before it falls back to compiled defaults.
// Returns true if it restored something (and re-persists it to NVS so
// future boots read from NVS directly, same as any other edit).
bool restore_button_config_from_sd()
{
  if (!sdReady || !SD_MMC.exists(SD_BUTTON_BACKUP_PATH)) return false;
  File f = SD_MMC.open(SD_BUTTON_BACKUP_PATH, FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("[SD] button_backup.json failed to parse -- ignoring");
    return false;
  }

  JsonArray buttons = doc["buttons"].as<JsonArray>();
  if (buttons.isNull()) return false;

  sdButtonSavedAt = doc["saved_at"] | "";

  for (JsonObject b : buttons) {
    int page = b["page"] | 1;
    int idx = b["idx"] | -1;
    if (idx < 0 || idx >= BTNS_PER_PAGE) continue;
    EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
    strlcpy(arr[idx].label, b["lbl"] | "", sizeof(arr[idx].label));
    strlcpy(arr[idx].subLabel, b["sub"] | "", sizeof(arr[idx].subLabel));
    arr[idx].modifier1 = mod_from_code(b["m1"] | "none");
    arr[idx].modifier2 = mod_from_code(b["m2"] | "none");
    const char *keyStr = b["key"] | "";
    if (keyStr[0] != '\0') arr[idx].key = keyStr[0];
    strlcpy(arr[idx].action, b["action"] | "", sizeof(arr[idx].action));
  }
  Serial.println("[SD] Button config restored from SD backup (NVS was empty)");
  return true;
}

// ---- HA button config backup/restore -- own file, deliberately kept
// separate from button_backup.json above. Mirrors liveHA to SD the same
// way, called once per HA Save All (see handle_save_ha()). This data
// never flows into hotkeys.ahk -- see the HAButton struct comment.
void backup_ha_config_to_sd()
{
  if (!sdReady) return;
  JsonDocument doc;
  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (int i = 0; i < BTNS_PER_PAGE; i++) {
    JsonObject b = buttons.add<JsonObject>();
    b["idx"] = i;
    b["lbl"] = liveHA[i].label;
    b["sub"] = liveHA[i].subLabel;
    b["dom"] = liveHA[i].domain;
    b["svc"] = liveHA[i].service;
    b["ent"] = liveHA[i].entity;
  }
  sdHaSavedAt = current_timestamp_str();
  doc["saved_at"] = sdHaSavedAt;

  File f = SD_MMC.open(SD_HA_BACKUP_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] Could not open ha_backup.json for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[SD] HA config backed up");
}

// Restores HA config from the SD backup -- same fallback role as
// restore_button_config_from_sd(), checked from load_ha_config() before
// falling back to blank defaults.
bool restore_ha_config_from_sd()
{
  if (!sdReady || !SD_MMC.exists(SD_HA_BACKUP_PATH)) return false;
  File f = SD_MMC.open(SD_HA_BACKUP_PATH, FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("[SD] ha_backup.json failed to parse -- ignoring");
    return false;
  }

  JsonArray buttons = doc["buttons"].as<JsonArray>();
  if (buttons.isNull()) return false;

  sdHaSavedAt = doc["saved_at"] | "";

  for (JsonObject b : buttons) {
    int idx = b["idx"] | -1;
    if (idx < 0 || idx >= BTNS_PER_PAGE) continue;
    strlcpy(liveHA[idx].label, b["lbl"] | "", sizeof(liveHA[idx].label));
    strlcpy(liveHA[idx].subLabel, b["sub"] | "", sizeof(liveHA[idx].subLabel));
    strlcpy(liveHA[idx].domain, b["dom"] | "", sizeof(liveHA[idx].domain));
    strlcpy(liveHA[idx].service, b["svc"] | "", sizeof(liveHA[idx].service));
    strlcpy(liveHA[idx].entity, b["ent"] | "", sizeof(liveHA[idx].entity));
  }
  Serial.println("[SD] HA config restored from SD backup (NVS was empty)");
  return true;
}

// All three Info/System card groups now share one reactor-HUD palette
// instead of separate blue/amber/teal fills -- a HUD reads as one
// instrument panel, not three unrelated colored widgets. Kept as three
// named macro groups (rather than collapsed to one) so every call site
// below didn't need touching, only these definitions.
#define COLOR_BLUE_BG   lv_color_hex(0x0a151f)
#define COLOR_BLUE_TXT  JSS_COLOR_CYAN
#define COLOR_BLUE_SUB  JSS_COLOR_CYAN_MID

#define COLOR_AMBER_BG  lv_color_hex(0x0a151f)
#define COLOR_AMBER_TXT JSS_COLOR_CYAN
#define COLOR_AMBER_SUB JSS_COLOR_CYAN_MID

#define COLOR_TEAL_BG   lv_color_hex(0x0a151f)
#define COLOR_TEAL_TXT  JSS_COLOR_CYAN
#define COLOR_TEAL_SUB  JSS_COLOR_CYAN_MID

#define WI_SUN_CLEAR "\xEF\x80\x8D"
#define WI_SUN_CLOUD "\xEF\x80\x82"
#define WI_CLOUD     "\xEF\x81\x81"
#define WI_FOG       "\xEF\x80\x94"
#define WI_RAIN      "\xEF\x80\x99"
#define WI_SNOW      "\xEF\x80\x9B"
#define WI_THUNDER   "\xEF\x80\x9E"

#define CAL_ICON "\xEF\x81\xB3"

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

// Fills in the Orange card's two rows (today + tomorrow) from a set of
// already-fetched-or-cached values. Shared by fetch_weather() (live)
// and load_last_known_cache() (last known, at boot) so the two never
// drift out of sync with each other's formatting.
void apply_weather_display(float temp, int code, float hi, float lo, int rainPct,
                            int tmrwCode, float tmrwHi, float tmrwLo)
{
  char tempBuf[16];
  snprintf(tempBuf, sizeof(tempBuf), "%.0f F", temp);
  lv_label_set_text(weatherLabel, tempBuf);
  lv_label_set_text(weatherIcon, weathercode_to_icon(code));

  char subBuf[64];
  snprintf(subBuf, sizeof(subBuf), "%s  |  H:%.0f L:%.0f  |  Rain %d%%",
           weathercode_to_text(code), hi, lo, rainPct);
  lv_label_set_text(weatherSubLabel, subBuf);

  if (tmrwCode < 0) {
    lv_label_set_text(weatherTomorrowIcon, WI_CLOUD);
    lv_label_set_text(weatherTomorrowLabel, "Tomorrow  --");
  } else {
    char tmrwBuf[32];
    snprintf(tmrwBuf, sizeof(tmrwBuf), "Tomorrow   H:%.0f L:%.0f", tmrwHi, tmrwLo);
    lv_label_set_text(weatherTomorrowLabel, tmrwBuf);
    lv_label_set_text(weatherTomorrowIcon, weathercode_to_icon(tmrwCode));
  }
}

void fetch_weather()
{
  if (WiFi.status() != WL_CONNECTED || !geoLookupDone) {
    lv_label_set_text(weatherLabel, "--");
    lv_label_set_text(weatherSubLabel, "No connection");
    lv_label_set_text(weatherTomorrowLabel, "Tomorrow  --");
    return;
  }

  HTTPClient http;
  // "daily" pulls today's high/low + rain chance and tomorrow's
  // high/low/condition in the same request -- timezone=auto lets
  // Open-Meteo figure out day boundaries from lat/lon itself, so
  // "today"/"tomorrow" line up correctly without us tracking a second
  // timezone string. forecast_days=2 is exactly today + tomorrow.
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current_weather=true&temperature_unit=fahrenheit" +
               "&daily=weathercode,temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
               "&timezone=auto&forecast_days=2";
  http.begin(url);
  // Bound how long this can block loop() -- this whole call runs
  // synchronously right in loop(), which also stalls server.handleClient()
  // and therefore incoming telemetry POSTs from pc_companion_telemetry.py.
  // Without an explicit cap, HTTPClient's default timeout (several
  // seconds) let a slow/hung fetch stall long enough that the CPU/RAM/GPU
  // staleness check (see check_telemetry_staleness()) would blank the
  // System page right after -- intermittent, since it only happened when
  // a given fetch ran long. Capping both here keeps the worst case well
  // under that staleness window.
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    float temp = doc["current_weather"]["temperature"];
    int code = doc["current_weather"]["weathercode"];

    JsonArray tmax = doc["daily"]["temperature_2m_max"].as<JsonArray>();
    JsonArray tmin = doc["daily"]["temperature_2m_min"].as<JsonArray>();
    JsonArray rain = doc["daily"]["precipitation_probability_max"].as<JsonArray>();
    JsonArray dcode = doc["daily"]["weathercode"].as<JsonArray>();

    float hi = tmax.size() > 0 ? tmax[0].as<float>() : NAN;
    float lo = tmin.size() > 0 ? tmin[0].as<float>() : NAN;
    int rainPct = rain.size() > 0 ? rain[0].as<int>() : -1;
    int tmrwCode = dcode.size() > 1 ? dcode[1].as<int>() : -1;
    float tmrwHi = tmax.size() > 1 ? tmax[1].as<float>() : NAN;
    float tmrwLo = tmin.size() > 1 ? tmin[1].as<float>() : NAN;

    apply_weather_display(temp, code, hi, lo, rainPct, tmrwCode, tmrwHi, tmrwLo);

    lastWeatherFetch = millis();
    lastWeatherTemp = temp;
    lastWeatherCode = code;
    lastTodayHigh = hi;
    lastTodayLow = lo;
    lastRainPct = rainPct;
    lastTomorrowCode = tmrwCode;
    lastTomorrowHigh = tmrwHi;
    lastTomorrowLow = tmrwLo;
    save_last_known_cache(); // mirror to SD -- see load_last_known_cache() for the reboot fallback
  } else {
    lv_label_set_text(weatherLabel, "--");
    lv_label_set_text(weatherSubLabel, "Fetch failed");
    lv_label_set_text(weatherTomorrowLabel, "Tomorrow  --");
  }

  http.end();
}

void maybe_refresh_weather()
{
  if (millis() - lastWeatherFetch >= WEATHER_REFRESH_MS) {
    fetch_weather();
  }
}

// Blue card's top line: "2:14 PM  |  Tuesday, Sep 3" -- time and date
// sharing one line instead of stacking, so the week strip below has
// room. (Plain "|" separator, not a fancy dash/bullet -- the compiled-in
// font doesn't cover those glyphs.)
void update_time_label()
{
  struct tm ti;
  if (getLocalTime(&ti, 0)) {
    int h12 = ti.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    const char *ap = (ti.tm_hour < 12) ? "AM" : "PM";
    char wdayBuf[12], monBuf[6];
    strftime(wdayBuf, sizeof(wdayBuf), "%A", &ti);
    strftime(monBuf, sizeof(monBuf), "%b", &ti);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d:%02d %s  |  %s, %s %d",
             h12, ti.tm_min, ap, wdayBuf, monBuf, ti.tm_mday);
    lv_label_set_text(timeDateLabel, buf);
  } else {
    lv_label_set_text(timeDateLabel, "Syncing...");
  }
  update_week_strip();
  update_timezone_labels();
}

static void tileview_event_cb(lv_event_t *e)
{
  lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t *active = lv_tileview_get_tile_act(tv);

  if (active == tileInfoGlobal) {
    maybe_refresh_weather();
  } else if (active == tileSystemGlobal) {
    update_system_network_card();
    update_sd_status_card();
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
  ensure_hud_styles_inited();
  lv_obj_add_style(card, &style_hud_card_border, LV_PART_MAIN | LV_STATE_DEFAULT);
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

// ---- Generic multi-row card shell -- used by the Info page's timezone
// card and all three System page cards below. Unlike make_info_card()
// above (one icon + one big value), these hold several small rows
// stacked in a column.
lv_obj_t* make_card_shell(lv_obj_t *parent, uint32_t w, uint32_t h, uint32_t y, lv_color_t bg)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, w, h);
  lv_obj_set_pos(card, BTN_GAP, y);
  lv_obj_set_style_bg_color(card, bg, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  ensure_hud_styles_inited();
  lv_obj_add_style(card, &style_hud_card_border, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return card;
}

// One "label ... value" row, e.g. "WiFi   Connected (-52 dBm)". Returns
// the value label so the caller can update it later.
lv_obj_t* add_card_row(lv_obj_t *card, lv_color_t txtColor, lv_color_t valColor, const char *leftText)
{
  lv_obj_t *row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *nameLbl = lv_label_create(row);
  lv_label_set_text(nameLbl, leftText);
  lv_obj_set_style_text_color(nameLbl, txtColor, 0);

  lv_obj_t *valLbl = lv_label_create(row);
  lv_label_set_text(valLbl, "--");
  lv_obj_set_style_text_color(valLbl, valColor, 0);

  return valLbl;
}

// One "CPU  [====----]  42%" row -- a name label, a growable bar, and a
// percent label. Used by the System page's consolidated CPU/RAM/GPU card.
lv_obj_t* make_stat_row(lv_obj_t *card, lv_color_t txtColor, lv_color_t barTrack, lv_obj_t **outPct, const char *name)
{
  lv_obj_t *row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);

  lv_obj_t *nameLbl = lv_label_create(row);
  lv_label_set_text(nameLbl, name);
  lv_obj_set_style_text_color(nameLbl, txtColor, 0);
  lv_obj_set_width(nameLbl, 40);

  lv_obj_t *bar = lv_bar_create(row);
  lv_obj_set_flex_grow(bar, 1);
  lv_obj_set_height(bar, 14);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, barTrack, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, txtColor, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);

  lv_obj_t *pctLbl = lv_label_create(row);
  lv_label_set_text(pctLbl, "--");
  lv_obj_set_style_text_color(pctLbl, txtColor, 0);
  lv_obj_set_width(pctLbl, 44);
  lv_obj_set_style_text_align(pctLbl, LV_TEXT_ALIGN_RIGHT, 0);

  *outPct = pctLbl;
  return bar;
}

// Blue card: time+date on one line, plus a 7-day week strip underneath
// with today's column brighter than the rest. See update_week_strip()
// for the per-second refresh (cheap -- just relabeling 14 labels).
void build_blue_card(lv_obj_t *tile, uint32_t w, uint32_t h, uint32_t y)
{
  lv_obj_t *card = make_card_shell(tile, w, h, y, COLOR_BLUE_BG);

  timeDateLabel = lv_label_create(card);
  lv_label_set_text(timeDateLabel, "Syncing...");
  lv_obj_set_style_text_color(timeDateLabel, COLOR_BLUE_TXT, 0);
  lv_obj_set_style_text_font(timeDateLabel, &lv_font_montserrat_18, 0);

  lv_obj_t *weekRow = lv_obj_create(card);
  lv_obj_remove_style_all(weekRow);
  lv_obj_set_size(weekRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(weekRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(weekRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < 7; i++) {
    lv_obj_t *cell = lv_obj_create(weekRow);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(cell, 6, 0);
    lv_obj_set_style_pad_ver(cell, 3, 0);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0); // today's cell fills this in -- see update_week_strip()
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    weekCells[i] = cell;

    lv_obj_t *dow = lv_label_create(cell);
    lv_obj_set_style_text_color(dow, COLOR_BLUE_SUB, 0);
    weekDayLabels[i] = dow;

    lv_obj_t *num = lv_label_create(cell);
    lv_obj_set_style_text_color(num, COLOR_BLUE_SUB, 0);
    weekNumLabels[i] = num;
  }
}

// Recomputes the week strip's 7 day numbers and highlights today's
// column. Uses mktime() to normalize month/year rollover (e.g. Aug 31
// + 3 days -> Sep 3) -- proper calendar math, unlike the plain
// hour-offset trick update_timezone_labels() uses below (that one gets
// away with skipping it since it never displays a date).
//
// Today gets a solid rounded "chip" (bright fill, dark text) instead of
// just a brighter text color -- much easier to spot at a glance than a
// color shift alone, and matches how a phone/calendar app marks today.
void update_week_strip()
{
  struct tm base;
  if (!getLocalTime(&base, 0)) return;

  static const char *dowLetters[7] = {"S", "M", "T", "W", "T", "F", "S"};
  int todayWday = base.tm_wday; // 0=Sun..6=Sat

  for (int i = 0; i < 7; i++) {
    struct tm cell = base;
    cell.tm_mday += (i - todayWday);
    cell.tm_hour = 12; cell.tm_min = 0; cell.tm_sec = 0; // clear of any midnight/DST edge case
    mktime(&cell); // normalizes month/day rollover

    char buf[4];
    snprintf(buf, sizeof(buf), "%d", cell.tm_mday);
    lv_label_set_text(weekNumLabels[i], buf);
    lv_label_set_text(weekDayLabels[i], dowLetters[i]);

    bool isToday = (i == todayWday);
    if (isToday) {
      lv_obj_set_style_bg_opa(weekCells[i], LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(weekCells[i], COLOR_BLUE_TXT, 0);
      lv_obj_set_style_text_color(weekDayLabels[i], COLOR_BLUE_BG, 0);
      lv_obj_set_style_text_color(weekNumLabels[i], COLOR_BLUE_BG, 0);
    } else {
      lv_obj_set_style_bg_opa(weekCells[i], LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(weekDayLabels[i], COLOR_BLUE_SUB, 0);
      lv_obj_set_style_text_color(weekNumLabels[i], COLOR_BLUE_SUB, 0);
    }
  }
}

// Orange card: today's conditions on top, a thin divider, then
// tomorrow's outlook underneath. See apply_weather_display() (near
// fetch_weather()) for what fills these labels in.
void build_weather_card(lv_obj_t *tile, uint32_t w, uint32_t h, uint32_t y)
{
  lv_obj_t *card = make_card_shell(tile, w, h, y, COLOR_AMBER_BG);

  lv_obj_t *todayRow = lv_obj_create(card);
  lv_obj_remove_style_all(todayRow);
  lv_obj_set_size(todayRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(todayRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(todayRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  weatherIcon = lv_label_create(todayRow);
  lv_label_set_text(weatherIcon, WI_CLOUD);
  lv_obj_set_style_text_color(weatherIcon, COLOR_AMBER_TXT, 0);
  lv_obj_set_style_text_font(weatherIcon, &weather_icons_24, 0);
  lv_obj_set_style_pad_right(weatherIcon, 8, 0);

  lv_obj_t *todayCol = lv_obj_create(todayRow);
  lv_obj_remove_style_all(todayCol);
  lv_obj_set_height(todayCol, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(todayCol, 1);
  lv_obj_set_flex_flow(todayCol, LV_FLEX_FLOW_COLUMN);

  weatherLabel = lv_label_create(todayCol);
  lv_label_set_text(weatherLabel, "--");
  lv_obj_set_style_text_color(weatherLabel, COLOR_AMBER_TXT, 0);
  lv_obj_set_style_text_font(weatherLabel, &lv_font_montserrat_24, 0);

  weatherSubLabel = lv_label_create(todayCol);
  lv_label_set_text(weatherSubLabel, "Loading...");
  lv_obj_set_style_text_color(weatherSubLabel, COLOR_AMBER_SUB, 0);

  lv_obj_t *divider = lv_obj_create(card);
  lv_obj_remove_style_all(divider);
  lv_obj_set_size(divider, LV_PCT(94), 1);
  lv_obj_set_style_bg_color(divider, COLOR_AMBER_SUB, 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);

  lv_obj_t *tmrwRow = lv_obj_create(card);
  lv_obj_remove_style_all(tmrwRow);
  lv_obj_set_size(tmrwRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tmrwRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tmrwRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tmrwRow, 6, 0);

  weatherTomorrowIcon = lv_label_create(tmrwRow);
  lv_label_set_text(weatherTomorrowIcon, WI_CLOUD);
  lv_obj_set_style_text_color(weatherTomorrowIcon, COLOR_AMBER_SUB, 0);
  lv_obj_set_style_text_font(weatherTomorrowIcon, &weather_icons_24, 0);

  weatherTomorrowLabel = lv_label_create(tmrwRow);
  lv_label_set_text(weatherTomorrowLabel, "Tomorrow  --");
  lv_obj_set_style_text_color(weatherTomorrowLabel, COLOR_AMBER_SUB, 0);
}

// Green card on the Info page: US timezones, zone name stacked over a
// big time, four columns separated by thin vertical dividers. See
// update_timezone_labels() for the actual per-second refresh.
void build_timezone_card(lv_obj_t *tile, uint32_t w, uint32_t h, uint32_t y)
{
  lv_obj_t *card = make_card_shell(tile, w, h, y, COLOR_TEAL_BG);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      lv_obj_t *divider = lv_obj_create(card);
      lv_obj_remove_style_all(divider);
      lv_obj_set_size(divider, 1, LV_PCT(70));
      lv_obj_set_style_bg_color(divider, COLOR_TEAL_SUB, 0);
      lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);
    }

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *zoneLbl = lv_label_create(col);
    lv_label_set_text(zoneLbl, TZ_NAMES[i]);
    lv_obj_set_style_text_color(zoneLbl, COLOR_TEAL_SUB, 0);
    tzZoneLabels[i] = zoneLbl;

    lv_obj_t *timeLbl = lv_label_create(col);
    lv_label_set_text(timeLbl, "--:--");
    lv_obj_set_style_text_color(timeLbl, COLOR_TEAL_TXT, 0);
    lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_18, 0);
    tzTimeLabels[i] = timeLbl;
  }
}

// Called once a second (see loop()) -- recomputes all four zone clocks
// from the board's own Eastern-time clock. Only the hour/minute fields
// are touched (no calendar-day rollover math), which is fine since
// these labels only ever show a time, never a date.
void update_timezone_labels()
{
  struct tm etTime;
  if (!getLocalTime(&etTime, 0)) return;

  for (int i = 0; i < 4; i++) {
    int hour = etTime.tm_hour + TZ_HOUR_OFFSETS[i];
    if (hour < 0) hour += 24;
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    const char *ap = (hour < 12) ? "a" : "p";
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d%s", h12, etTime.tm_min, ap);
    lv_label_set_text(tzTimeLabels[i], buf);
  }
}

void build_info_page(lv_obj_t *tile)
{
  uint32_t rowH = (screenHeight - HEADER_H - (BTN_GAP * 4)) / 3;
  uint32_t rowW = screenWidth - (BTN_GAP * 2);

  build_blue_card(tile, rowW, rowH, HEADER_H + BTN_GAP);
  build_weather_card(tile, rowW, rowH, HEADER_H + BTN_GAP * 2 + rowH);
  build_timezone_card(tile, rowW, rowH, HEADER_H + BTN_GAP * 3 + rowH * 2);
}

// ================= System page (CPU/RAM/GPU, SD status, network) =================
// Blue: live CPU/RAM/GPU, fed by the same small PC-side companion
// script (pc_companion_telemetry.py) that also drives Now Playing and
// the mute-state indicator on the Media page.
// Orange: the two SD-backed items' last-saved times, plus SD free space.
// Green: WiFi status + IP (moved here from the Info page -- see
// build_timezone_card() there for what replaced it).
// Same Blue -> Orange -> Green color order as the Info page.
lv_obj_t *cpuBar, *cpuPct;
lv_obj_t *ramBar, *ramPct;
lv_obj_t *gpuBar, *gpuPct;

lv_obj_t *sdWeatherVal, *sdButtonVal, *sdHaVal, *sdSpaceVal;
lv_obj_t *sysWifiVal, *sysSsidVal, *sysIpVal;

void build_system_page(lv_obj_t *tile)
{
  uint32_t rowH = (screenHeight - HEADER_H - (BTN_GAP * 4)) / 3;
  uint32_t rowW = screenWidth - (BTN_GAP * 2);

  lv_obj_t *blueCard = make_card_shell(tile, rowW, rowH, HEADER_H + BTN_GAP, COLOR_BLUE_BG);
  cpuBar = make_stat_row(blueCard, COLOR_BLUE_TXT, COLOR_BLUE_SUB, &cpuPct, "CPU");
  ramBar = make_stat_row(blueCard, COLOR_BLUE_TXT, COLOR_BLUE_SUB, &ramPct, "RAM");
  gpuBar = make_stat_row(blueCard, COLOR_BLUE_TXT, COLOR_BLUE_SUB, &gpuPct, "GPU");

  lv_obj_t *orangeCard = make_card_shell(tile, rowW, rowH, HEADER_H + BTN_GAP * 2 + rowH, COLOR_AMBER_BG);
  sdWeatherVal = add_card_row(orangeCard, COLOR_AMBER_TXT, COLOR_AMBER_SUB, "Weather Cache");
  sdButtonVal  = add_card_row(orangeCard, COLOR_AMBER_TXT, COLOR_AMBER_SUB, "Button Backup");
  sdHaVal      = add_card_row(orangeCard, COLOR_AMBER_TXT, COLOR_AMBER_SUB, "HA Backup");
  sdSpaceVal   = add_card_row(orangeCard, COLOR_AMBER_TXT, COLOR_AMBER_SUB, "SD Card");

  lv_obj_t *greenCard = make_card_shell(tile, rowW, rowH, HEADER_H + BTN_GAP * 3 + rowH * 2, COLOR_TEAL_BG);
  sysWifiVal = add_card_row(greenCard, COLOR_TEAL_TXT, COLOR_TEAL_SUB, "WiFi");
  sysSsidVal = add_card_row(greenCard, COLOR_TEAL_TXT, COLOR_TEAL_SUB, "SSID");
  sysIpVal   = add_card_row(greenCard, COLOR_TEAL_TXT, COLOR_TEAL_SUB, "IP Address");
}

unsigned long lastTelemetryMs = 0;
bool telemetryEverSeen = false;

void update_system_page(int cpu, int gpu, int ram)
{
  char buf[8];
  lv_bar_set_value(cpuBar, cpu, LV_ANIM_OFF);
  snprintf(buf, sizeof(buf), "%d%%", cpu); lv_label_set_text(cpuPct, buf);
  lv_bar_set_value(ramBar, ram, LV_ANIM_OFF);
  snprintf(buf, sizeof(buf), "%d%%", ram); lv_label_set_text(ramPct, buf);
  lv_bar_set_value(gpuBar, gpu, LV_ANIM_OFF);
  snprintf(buf, sizeof(buf), "%d%%", gpu); lv_label_set_text(gpuPct, buf);
  telemetryEverSeen = true;
  lastTelemetryMs = millis();
}

// Threshold has margin above the ~1s PC-side send interval AND above the
// capped worst-case blocking time of fetch_weather()'s HTTPClient call
// (see its setConnectTimeout()/setTimeout() comment) -- that call runs
// synchronously in loop() and briefly starves server.handleClient(), so
// this needs enough slack that one such stall doesn't itself look like a
// dead telemetry feed.
#define TELEMETRY_STALE_MS 8000UL

void check_telemetry_staleness()
{
  if (telemetryEverSeen && millis() - lastTelemetryMs > TELEMETRY_STALE_MS) {
    lv_bar_set_value(cpuBar, 0, LV_ANIM_OFF);
    lv_bar_set_value(ramBar, 0, LV_ANIM_OFF);
    lv_bar_set_value(gpuBar, 0, LV_ANIM_OFF);
    lv_label_set_text(cpuPct, "--");
    lv_label_set_text(ramPct, "--");
    lv_label_set_text(gpuPct, "--");
  }
}

// Orange card -- refreshed on Save All / cache-write (immediate) and on
// every System-tile switch (see tileview_event_cb) so free-space stays
// current without needing its own periodic poll.
void update_sd_status_card()
{
  if (!sdReady) {
    lv_label_set_text(sdWeatherVal, "No card");
    lv_label_set_text(sdButtonVal, "No card");
    lv_label_set_text(sdHaVal, "No card");
    lv_label_set_text(sdSpaceVal, "No card");
    return;
  }

  lv_label_set_text(sdWeatherVal, sdWeatherSavedAt.length() ? sdWeatherSavedAt.c_str() : "Not yet saved");
  lv_label_set_text(sdButtonVal, sdButtonSavedAt.length() ? sdButtonSavedAt.c_str() : "Not yet saved");
  lv_label_set_text(sdHaVal, sdHaSavedAt.length() ? sdHaSavedAt.c_str() : "Not yet saved");

  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  uint64_t freeBytes = (total > used) ? (total - used) : 0;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
           freeBytes / (1024.0 * 1024.0 * 1024.0), total / (1024.0 * 1024.0 * 1024.0));
  lv_label_set_text(sdSpaceVal, buf);
}

// Green card -- same WiFi/IP data that used to live on the Info page's
// Green card, now living here instead.
void update_system_network_card()
{
  if (WiFi.status() == WL_CONNECTED) {
    char sub[32];
    snprintf(sub, sizeof(sub), "Connected (%d dBm)", WiFi.RSSI());
    lv_label_set_text(sysWifiVal, sub);
    lv_label_set_text(sysSsidVal, WiFi.SSID().c_str());
    lv_label_set_text(sysIpVal, WiFi.localIP().toString().c_str());
  } else {
    lv_label_set_text(sysWifiVal, "Not Connected");
    lv_label_set_text(sysSsidVal, "--");
    lv_label_set_text(sysIpVal, "--");
  }
}

// ================= Telemetry serial protocol =================
// Shared by System (CPU/RAM/GPU), Media's Now Playing panel, and the
// mute-state indicator. Lines come from pc_companion_telemetry.py
// over the same USB-CDC serial port used for everything else -- no
// second cable, no second port.
//   DATA,<cpu>,<gpu>,<ram>\n        -- integers 0-100
//   NOWPLAYING,<track>|||<artist>\n -- ||| separator avoids comma clashes
//   MUTE,<0|1>\n                    -- corrects the local guess-toggle
static void handle_telemetry_line(const String &line)
{
  // Debug echo -- ALWAYS prints, for ANY line received, parsed or not.
  // This is the only way to tell "board is receiving nothing" apart
  // from "board is receiving fine but not parsing it right" -- watch
  // for these lines in pc_companion_telemetry.py's terminal (it prints
  // whatever the board sends back as [BOARD] ...).
  Serial.print("[BOARDRX] ");
  Serial.println(line);

  if (line.startsWith("DATA,")) {
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    if (c1 > 0 && c2 > 0 && c3 > 0) {
      int cpu = line.substring(c1 + 1, c2).toInt();
      int gpu = line.substring(c2 + 1, c3).toInt();
      int ram = line.substring(c3 + 1).toInt();
      update_system_page(cpu, gpu, ram);
      Serial.printf("[BOARDRX] parsed DATA ok -> cpu=%d gpu=%d ram=%d\n", cpu, gpu, ram);
    } else {
      Serial.println("[BOARDRX] DATA line did not parse (bad comma count)");
    }
  } else if (line.startsWith("NOWPLAYING,")) {
    String rest = line.substring(strlen("NOWPLAYING,"));
    int sep = rest.indexOf("|||");
    String track = sep >= 0 ? rest.substring(0, sep) : rest;
    String artist = sep >= 0 ? rest.substring(sep + 3) : "";
    update_now_playing(track, artist);
  } else if (line.startsWith("MUTE,")) {
    bool muted = line.substring(5).toInt() != 0;
    guessIsMuted = muted;
    update_mute_visual(muted);
  }
}

void poll_telemetry_serial()
{
  static String buf = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buf.trim();
      if (buf.length() > 0) handle_telemetry_line(buf);
      buf = "";
    } else if (c != '\r') {
      buf += c;
      if (buf.length() > 200) buf = ""; // guard against a runaway line
    }
  }
}

// ================= WiFi/HTTP telemetry receiver + wireless button config editor =================
// The board runs a small WebServer on port 80 for two unrelated jobs that
// happen to share it because both need "the board listening for
// incoming HTTP on the LAN":
//   POST /telemetry   <- pc_companion_telemetry.py: cpu, gpu, ram
//   POST /mute         <- pc_companion_telemetry.py: muted (0/1)
//   POST /nowplaying   <- pc_companion_telemetry.py: track, artist
//   GET  /              -> button config editor page (rename Hotkeys /
//                          Hotkeys 2 buttons, change their key combo)
//   POST /save          <- that editor's form submit
// See the EditableButton comment near PAGE1_BUTTONS' include for why the
// config editor exists as a separate live copy rather than editing
// PAGE1_BUTTONS/PAGE1B_BUTTONS directly (those are const, compiled-in).

// ---- Modifier <-> short string code, for the editor's <select> values ----
static uint8_t mod_from_code(const String &code)
{
  if (code == "ctrl")  return KEY_LEFT_CTRL;
  if (code == "alt")   return KEY_LEFT_ALT;
  if (code == "shift") return KEY_LEFT_SHIFT;
  if (code == "win")   return KEY_LEFT_GUI;
  return 0; // "none" / anything unrecognized
}

static const char* code_from_mod(uint8_t mod)
{
  if (mod == KEY_LEFT_CTRL)  return "ctrl";
  if (mod == KEY_LEFT_ALT)   return "alt";
  if (mod == KEY_LEFT_SHIFT) return "shift";
  if (mod == KEY_LEFT_GUI)   return "win";
  return "none";
}

// One <select> for one modifier slot, with `sel` marked selected.
static String mod_select_html(const String &name, uint8_t current)
{
  const char *cur = code_from_mod(current);
  String html = "<select name='" + name + "'>";
  const char *opts[5] = {"none", "ctrl", "alt", "shift", "win"};
  const char *labels[5] = {"(none)", "Ctrl", "Alt", "Shift", "Win"};
  for (int i = 0; i < 5; i++) {
    html += "<option value='" + String(opts[i]) + "'";
    if (strcmp(cur, opts[i]) == 0) html += " selected";
    html += ">" + String(labels[i]) + "</option>";
  }
  html += "</select>";
  return html;
}

// Loads livePage1/livePage1b from the compiled defaults, then overrides
// any button that has a saved edit in NVS. Call once, at boot, before
// building the Hotkeys/Hotkeys 2 grids.
void load_button_config()
{
  for (int i = 0; i < BTNS_PER_PAGE; i++) {
    strlcpy(livePage1[i].label, PAGE1_BUTTONS[i].label, sizeof(livePage1[i].label));
    strlcpy(livePage1[i].subLabel, PAGE1_BUTTONS[i].subLabel, sizeof(livePage1[i].subLabel));
    livePage1[i].modifier1 = PAGE1_BUTTONS[i].modifier1;
    livePage1[i].modifier2 = PAGE1_BUTTONS[i].modifier2;
    livePage1[i].key = PAGE1_BUTTONS[i].key;
    strlcpy(livePage1[i].action, PAGE1_DEFAULT_ACTIONS[i], sizeof(livePage1[i].action));

    strlcpy(livePage1b[i].label, PAGE1B_BUTTONS[i].label, sizeof(livePage1b[i].label));
    strlcpy(livePage1b[i].subLabel, PAGE1B_BUTTONS[i].subLabel, sizeof(livePage1b[i].subLabel));
    livePage1b[i].modifier1 = PAGE1B_BUTTONS[i].modifier1;
    livePage1b[i].modifier2 = PAGE1B_BUTTONS[i].modifier2;
    livePage1b[i].key = PAGE1B_BUTTONS[i].key;
    strlcpy(livePage1b[i].action, PAGE1B_DEFAULT_ACTIONS[i], sizeof(livePage1b[i].action));
  }

  // If NVS has never been written (factory-fresh flash / an erase),
  // prefer restoring from the SD backup over the compiled defaults --
  // see restore_button_config_from_sd(). Re-persists to NVS so future
  // boots read from NVS directly like any other edit, and skips the
  // per-key override loop below since the restore already has it all.
  pcPrefs.begin("pcconfig", true);
  bool nvsHasButtonData = pcPrefs.isKey("p1_0_lbl");
  pcPrefs.end();

  if (!nvsHasButtonData && restore_button_config_from_sd()) {
    for (int page = 1; page <= 2; page++) {
      EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
      for (int i = 0; i < BTNS_PER_PAGE; i++) {
        apply_button_edit(page, i, arr[i].label, arr[i].subLabel,
                           arr[i].modifier1, arr[i].modifier2, String(arr[i].key), arr[i].action);
      }
    }
    return;
  }

  pcPrefs.begin("pcconfig", true); // read-only
  for (int page = 1; page <= 2; page++) {
    EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
    for (int i = 0; i < BTNS_PER_PAGE; i++) {
      char keyBuf[16];
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_lbl", page, i);
      if (pcPrefs.isKey(keyBuf)) {
        String v = pcPrefs.getString(keyBuf, "");
        strlcpy(arr[i].label, v.c_str(), sizeof(arr[i].label));
      }
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_sub", page, i);
      if (pcPrefs.isKey(keyBuf)) {
        String v = pcPrefs.getString(keyBuf, "");
        strlcpy(arr[i].subLabel, v.c_str(), sizeof(arr[i].subLabel));
      }
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_m1", page, i);
      if (pcPrefs.isKey(keyBuf)) arr[i].modifier1 = pcPrefs.getUChar(keyBuf, arr[i].modifier1);
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_m2", page, i);
      if (pcPrefs.isKey(keyBuf)) arr[i].modifier2 = pcPrefs.getUChar(keyBuf, arr[i].modifier2);
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_key", page, i);
      if (pcPrefs.isKey(keyBuf)) {
        String v = pcPrefs.getString(keyBuf, "");
        if (v.length() > 0) arr[i].key = v[0];
      }
      snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_act", page, i);
      if (pcPrefs.isKey(keyBuf)) {
        String v = pcPrefs.getString(keyBuf, "");
        strlcpy(arr[i].action, v.c_str(), sizeof(arr[i].action));
      }
    }
  }
  pcPrefs.end();
}

// Applies one edited button to the live array, its on-screen labels (if
// already built), and persists it to NVS. label/subLabel/key are taken
// as-is if non-empty; an empty key is ignored (keeps the previous key)
// so a blank form field can't accidentally disable a button.
// putString()/putUChar() return the bytes written, 0 on failure (most
// commonly ESP_ERR_NVS_NOT_ENOUGH_SPACE -- the "nvs" partition is a
// fixed small size, shared by every Preferences namespace on the board
// plus WiFi's own internal state, and it can fill up). Both write loops
// below log any 0-byte result so a "my saved config vanished after a
// power cycle" report can be traced from the Serial Monitor instead of
// guessed at -- a failed write never reached flash, so it was never
// really saved even though the on-screen edit looked like it worked.
static void nvs_put_str_checked(Preferences &prefs, const char *key, const String &val)
{
  size_t n = prefs.putString(key, val);
  if (n == 0 && val.length() > 0) {
    Serial.printf("[NVS] FAILED to write '%s' (%u bytes) -- NVS partition may be full\n", key, val.length());
  }
}

static void apply_button_edit(int page, int idx, const String &label, const String &subLabel,
                               uint8_t mod1, uint8_t mod2, const String &key, const String &action)
{
  if (idx < 0 || idx >= BTNS_PER_PAGE) return;
  EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
  lv_obj_t **mainLbls = (page == 1) ? page1MainLbl : page1bMainLbl;
  lv_obj_t **subLbls  = (page == 1) ? page1SubLbl  : page1bSubLbl;

  strlcpy(arr[idx].label, label.c_str(), sizeof(arr[idx].label));
  strlcpy(arr[idx].subLabel, subLabel.c_str(), sizeof(arr[idx].subLabel));
  arr[idx].modifier1 = mod1;
  arr[idx].modifier2 = mod2;
  if (key.length() > 0) arr[idx].key = key[0];
  strlcpy(arr[idx].action, action.c_str(), sizeof(arr[idx].action));

  if (mainLbls[idx]) lv_label_set_text(mainLbls[idx], arr[idx].label);
  if (subLbls[idx]) {
    lv_label_set_text(subLbls[idx], arr[idx].subLabel);
    if (strlen(arr[idx].subLabel) == 0) lv_obj_add_flag(subLbls[idx], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(subLbls[idx], LV_OBJ_FLAG_HIDDEN);
  }

  char keyBuf[16];
  pcPrefs.begin("pcconfig", false); // read-write
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_lbl", page, idx); nvs_put_str_checked(pcPrefs, keyBuf, arr[idx].label);
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_sub", page, idx); nvs_put_str_checked(pcPrefs, keyBuf, arr[idx].subLabel);
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_m1", page, idx);  pcPrefs.putUChar(keyBuf, arr[idx].modifier1);
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_m2", page, idx);  pcPrefs.putUChar(keyBuf, arr[idx].modifier2);
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_key", page, idx); nvs_put_str_checked(pcPrefs, keyBuf, String(arr[idx].key));
  snprintf(keyBuf, sizeof(keyBuf), "p%d_%d_act", page, idx); nvs_put_str_checked(pcPrefs, keyBuf, arr[idx].action);
  pcPrefs.end();
}

// Loads liveHA -- own NVS namespace ("haconfig"), entirely separate from
// "pcconfig" above. No compiled-default header exists for HA (unlike
// PAGE1_BUTTONS/PAGE1B_BUTTONS) since there's nothing sensible to
// pre-fill with someone else's entities -- factory-fresh buttons start
// blank ("HA 2".."HA 8", no domain/service/entity), except button 0,
// which ships with a worked FAKE EXAMPLE ("TV Bedroom" -> switch.toggle
// on switch.bedroom_tv) purely so a new user can see the expected
// format at a glance. THIS ENTITY DOES NOT EXIST ON YOUR HOME ASSISTANT
// INSTANCE -- pressing this button before editing it will just get a
// harmless 404/error back from your HA server. See docs/SETUP.md
// section 8. The user fills in real entities (including replacing this
// one) from the web editor. Call once, at boot, before build_ha_grid().
void load_ha_config()
{
  for (int i = 0; i < BTNS_PER_PAGE; i++) {
    snprintf(liveHA[i].label, sizeof(liveHA[i].label), "HA %d", i + 1);
    liveHA[i].subLabel[0] = '\0';
    liveHA[i].domain[0]   = '\0';
    liveHA[i].service[0]  = '\0';
    liveHA[i].entity[0]   = '\0';
  }
  // Example button -- see the big comment above. Overwritten immediately
  // by NVS/SD restore below if this board has already been configured.
  strlcpy(liveHA[0].label,   "TV Bedroom",        sizeof(liveHA[0].label));
  strlcpy(liveHA[0].subLabel,"EXAMPLE - edit me", sizeof(liveHA[0].subLabel));
  strlcpy(liveHA[0].domain,  "switch",            sizeof(liveHA[0].domain));
  strlcpy(liveHA[0].service, "toggle",            sizeof(liveHA[0].service));
  strlcpy(liveHA[0].entity,  "switch.bedroom_tv", sizeof(liveHA[0].entity));

  pcPrefs.begin("haconfig", true);
  bool nvsHasHaData = pcPrefs.isKey("ha_0_lbl");
  pcPrefs.end();

  // Factory-fresh NVS -- try the SD backup before settling for the blank
  // defaults above. Mirrors load_button_config()'s same fallback order.
  if (!nvsHasHaData && restore_ha_config_from_sd()) {
    for (int i = 0; i < BTNS_PER_PAGE; i++) {
      apply_ha_button_edit(i, liveHA[i].label, liveHA[i].subLabel,
                            liveHA[i].domain, liveHA[i].service, liveHA[i].entity);
    }
    return;
  }

  pcPrefs.begin("haconfig", true); // read-only
  for (int i = 0; i < BTNS_PER_PAGE; i++) {
    char keyBuf[16];
    snprintf(keyBuf, sizeof(keyBuf), "ha_%d_lbl", i);
    if (pcPrefs.isKey(keyBuf)) strlcpy(liveHA[i].label, pcPrefs.getString(keyBuf, "").c_str(), sizeof(liveHA[i].label));
    snprintf(keyBuf, sizeof(keyBuf), "ha_%d_sub", i);
    if (pcPrefs.isKey(keyBuf)) strlcpy(liveHA[i].subLabel, pcPrefs.getString(keyBuf, "").c_str(), sizeof(liveHA[i].subLabel));
    snprintf(keyBuf, sizeof(keyBuf), "ha_%d_dom", i);
    if (pcPrefs.isKey(keyBuf)) strlcpy(liveHA[i].domain, pcPrefs.getString(keyBuf, "").c_str(), sizeof(liveHA[i].domain));
    snprintf(keyBuf, sizeof(keyBuf), "ha_%d_svc", i);
    if (pcPrefs.isKey(keyBuf)) strlcpy(liveHA[i].service, pcPrefs.getString(keyBuf, "").c_str(), sizeof(liveHA[i].service));
    snprintf(keyBuf, sizeof(keyBuf), "ha_%d_ent", i);
    if (pcPrefs.isKey(keyBuf)) strlcpy(liveHA[i].entity, pcPrefs.getString(keyBuf, "").c_str(), sizeof(liveHA[i].entity));
  }
  pcPrefs.end();
}

// Applies one edited HA button to the live array, its on-screen labels
// (if already built), and persists it to NVS ("haconfig"). Mirrors
// apply_button_edit() above but with domain/service/entity instead of
// modifier/key/action.
static void apply_ha_button_edit(int idx, const String &label, const String &subLabel,
                                  const String &domain, const String &service, const String &entity)
{
  if (idx < 0 || idx >= BTNS_PER_PAGE) return;

  strlcpy(liveHA[idx].label, label.c_str(), sizeof(liveHA[idx].label));
  strlcpy(liveHA[idx].subLabel, subLabel.c_str(), sizeof(liveHA[idx].subLabel));
  strlcpy(liveHA[idx].domain, domain.c_str(), sizeof(liveHA[idx].domain));
  strlcpy(liveHA[idx].service, service.c_str(), sizeof(liveHA[idx].service));
  strlcpy(liveHA[idx].entity, entity.c_str(), sizeof(liveHA[idx].entity));

  if (haMainLbl[idx]) lv_label_set_text(haMainLbl[idx], liveHA[idx].label);
  if (haSubLbl[idx]) {
    lv_label_set_text(haSubLbl[idx], liveHA[idx].subLabel);
    if (strlen(liveHA[idx].subLabel) == 0) lv_obj_add_flag(haSubLbl[idx], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(haSubLbl[idx], LV_OBJ_FLAG_HIDDEN);
  }

  char keyBuf[16];
  pcPrefs.begin("haconfig", false); // read-write
  snprintf(keyBuf, sizeof(keyBuf), "ha_%d_lbl", idx); nvs_put_str_checked(pcPrefs, keyBuf, liveHA[idx].label);
  snprintf(keyBuf, sizeof(keyBuf), "ha_%d_sub", idx); nvs_put_str_checked(pcPrefs, keyBuf, liveHA[idx].subLabel);
  snprintf(keyBuf, sizeof(keyBuf), "ha_%d_dom", idx); nvs_put_str_checked(pcPrefs, keyBuf, liveHA[idx].domain);
  snprintf(keyBuf, sizeof(keyBuf), "ha_%d_svc", idx); nvs_put_str_checked(pcPrefs, keyBuf, liveHA[idx].service);
  snprintf(keyBuf, sizeof(keyBuf), "ha_%d_ent", idx); nvs_put_str_checked(pcPrefs, keyBuf, liveHA[idx].entity);
  pcPrefs.end();
}

// ---- GET / -- the config editor page itself ----
static String button_row_html(int page, int idx, const EditableButton &b)
{
  String p = String("p") + page + "_" + idx + "_";
  String row = "<tr><td>" + String(page == 1 ? "Hotkeys " : "Hotkeys 2 ") + String(idx + 1) + "</td>";
  row += "<td><input name='" + p + "lbl' value='" + String(b.label) + "' maxlength='23'></td>";
  row += "<td><input name='" + p + "sub' value='" + String(b.subLabel) + "' maxlength='23'></td>";
  row += "<td>" + mod_select_html(p + "m1", b.modifier1) + "</td>";
  row += "<td>" + mod_select_html(p + "m2", b.modifier2) + "</td>";
  row += "<td><input name='" + p + "key' value='" + String(b.key) + "' maxlength='1' size='2'></td>";
  row += "<td><input name='" + p + "act' value='" + String(b.action) + "' maxlength='79' placeholder='e.g. C:\\path\\to\\app.exe'></td>";
  row += "</tr>";
  return row;
}

// HA rows use their own field prefix ("ha_<idx>_") and their own form
// (action='/save_ha') so this section posts to handle_save_ha() below --
// entirely separate from /save and the Hotkeys/Hotkeys 2 rows, which is
// what keeps HA config out of hotkeys.ahk and GET /config.
static String ha_button_row_html(int idx, const HAButton &b)
{
  String p = String("ha_") + idx + "_";
  String row = "<tr><td>HA " + String(idx + 1) + "</td>";
  row += "<td><input name='" + p + "lbl' value='" + String(b.label) + "' maxlength='23'></td>";
  row += "<td><input name='" + p + "sub' value='" + String(b.subLabel) + "' maxlength='23'></td>";
  row += "<td><input name='" + p + "dom' value='" + String(b.domain) + "' maxlength='15' placeholder='light'></td>";
  row += "<td><input name='" + p + "svc' value='" + String(b.service) + "' maxlength='15' placeholder='toggle'></td>";
  row += "<td><input name='" + p + "ent" + "' value='" + String(b.entity) + "' maxlength='47' placeholder='light.living_room'></td>";
  row += "</tr>";
  return row;
}

static void handle_root()
{
  String html = "<!DOCTYPE html><html><head><title>PC Companion</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:20px;}"
    "table{border-collapse:collapse;width:100%;} td{padding:6px;border-bottom:1px solid #ddd;}"
    "input{width:90%;} button{padding:10px 20px;font-size:16px;margin-top:16px;}</style>"
    "</head><body>"
    "<h2>PC Companion -- Button Config</h2>"
    "<p>Rename buttons and change their key combo. Changes apply immediately "
    "and persist across reboots. Renaming a Hotkeys button does NOT change "
    "its Jarvis voice phrase -- that's fixed at flash time.</p>"
    "<p>The Action column is what runs on your PC when that combo is "
    "pressed -- a program path, script path, or command. Leave it blank "
    "if this button doesn't need one (e.g. it's already a native Windows "
    "shortcut). Your PC's telemetry service reads this automatically and "
    "keeps hotkeys.ahk in sync -- no manual editing needed.</p>"
    "<form method='POST' action='/save'>"
    "<table><tr><th>Button</th><th>Label</th><th>Sub-label</th><th>Mod 1</th><th>Mod 2</th><th>Key</th><th>Action</th></tr>";

  for (int i = 0; i < BTNS_PER_PAGE; i++) html += button_row_html(1, i, livePage1[i]);
  for (int i = 0; i < BTNS_PER_PAGE; i++) html += button_row_html(2, i, livePage1b[i]);

  html += "</table><button type='submit'>Save All</button></form>";

  html += "<h2>HA</h2>"
    "<p>Each button calls one Home Assistant service on one entity -- "
    "Domain (e.g. <code>light</code>), Service (e.g. <code>toggle</code>, "
    "<code>turn_on</code>, <code>turn_off</code>), Entity ID (e.g. "
    "<code>light.living_room</code>). Leave Entity blank to leave a "
    "button unconfigured -- it just won't do anything when tapped. "
    "These never touch hotkeys.ahk.</p>"
    "<form method='POST' action='/save_ha'>"
    "<table><tr><th>Button</th><th>Label</th><th>Sub-label</th><th>Domain</th><th>Service</th><th>Entity ID</th></tr>";

  for (int i = 0; i < BTNS_PER_PAGE; i++) html += ha_button_row_html(i, liveHA[i]);

  html += "</table><button type='submit'>Save HA</button></form>";

  html += "<h2>Home Assistant connection</h2>"
    "<p>Host/IP, port, and long-lived access token used by every HA "
    "button above. Takes effect immediately, no restart needed. The "
    "token field is always shown blank -- it's write-only here (never "
    "echoed back to the browser); leave it blank to keep the "
    "currently-saved token unchanged.</p>"
    "<form method='POST' action='/save_ha_conn'>"
    "<label>HA Host / IP</label><br>"
    "<input name='ha_host' maxlength='63' value='" + liveHaHost + "'><br>"
    "<label>HA Port</label><br>"
    "<input name='ha_port' maxlength='5' value='" + String(liveHaPort) + "'><br>"
    "<label>HA Long-Lived Access Token</label><br>"
    "<input name='ha_token' maxlength='200' placeholder='"
    + String(liveHaToken.length() > 0 ? "(unchanged -- currently set)" : "(not set)") +
    "'><br>"
    "<button type='submit'>Save HA Connection</button></form>";

  html += "<h2>WiFi network</h2>"
    "<p>Currently joined to: <b>" + liveWifiSsid + "</b>. To switch to a "
    "different WiFi network without Arduino IDE, the board will restart "
    "its own WiFi hotspot (<code>PC_Companion_Setup</code>) so you can enter "
    "new details from your phone or computer -- no reflash needed. (HA "
    "details can now be changed above without doing this.)</p>"
    "<form method='POST' action='/reconfigure_wifi' "
    "onsubmit='return confirm(\"This restarts the board into WiFi setup mode "
    "and it will drop off the network until reconfigured. Continue?\");'>"
    "<button type='submit'>Reconfigure WiFi</button></form>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ---- POST /save -- applies every row from the form above ----
static void handle_save()
{
  for (int page = 1; page <= 2; page++) {
    for (int i = 0; i < BTNS_PER_PAGE; i++) {
      String p = String("p") + page + "_" + i + "_";
      if (!server.hasArg(p + "lbl")) continue; // this button wasn't in the form, skip
      String label  = server.arg(p + "lbl");
      String sub    = server.arg(p + "sub");
      uint8_t m1    = mod_from_code(server.arg(p + "m1"));
      uint8_t m2    = mod_from_code(server.arg(p + "m2"));
      String key    = server.arg(p + "key");
      String action = server.hasArg(p + "act") ? server.arg(p + "act") : "";
      apply_button_edit(page, i, label, sub, m1, m2, key, action);
    }
  }
  backup_button_config_to_sd(); // mirror the just-saved config to SD -- see restore_button_config_from_sd()
  update_sd_status_card();      // reflect the new "last saved" time right away, not just on next tile switch
  server.sendHeader("Location", "/");
  server.send(303); // redirect back to the (now updated) editor page
}

// ---- POST /save_ha -- applies every row from the HA form above ----
// Separate handler/NVS namespace/SD file from handle_save() on purpose --
// see the HAButton struct comment for why.
static void handle_save_ha()
{
  for (int i = 0; i < BTNS_PER_PAGE; i++) {
    String p = String("ha_") + i + "_";
    if (!server.hasArg(p + "lbl")) continue;
    String label   = server.arg(p + "lbl");
    String sub     = server.arg(p + "sub");
    String domain  = server.arg(p + "dom");
    String service = server.arg(p + "svc");
    String entity  = server.arg(p + "ent");
    apply_ha_button_edit(i, label, sub, domain, service, entity);
  }
  backup_ha_config_to_sd(); // mirror to SD -- see restore_ha_config_from_sd(). Never touches AHK.
  update_sd_status_card();  // reflect the new "last saved" time right away, not just on next tile switch
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---- GET /config -- machine-readable version of the button config, for
// pc_companion_telemetry.py to poll and turn into hotkeys.ahk lines. Only
// includes combo + action (what AHK needs) -- label/sub-label are a
// board-only concern. Buttons with an empty action are included too
// (action == "") so the Python side can tell "no action set" apart from
// "button doesn't exist".
static void handle_get_config()
{
  JsonDocument doc;
  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (int page = 1; page <= 2; page++) {
    EditableButton *arr = (page == 1) ? livePage1 : livePage1b;
    for (int i = 0; i < BTNS_PER_PAGE; i++) {
      JsonObject b = buttons.add<JsonObject>();
      b["page"] = page;
      b["idx"] = i;
      b["mod1"] = code_from_mod(arr[i].modifier1);
      b["mod2"] = code_from_mod(arr[i].modifier2);
      b["key"] = String(arr[i].key);
      b["action"] = arr[i].action;
    }
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---- Telemetry POST routes -- pc_companion_telemetry.py's payload ----
static void handle_post_telemetry()
{
  if (server.hasArg("cpu") && server.hasArg("gpu") && server.hasArg("ram")) {
    update_system_page(server.arg("cpu").toInt(), server.arg("gpu").toInt(), server.arg("ram").toInt());
  }
  server.send(200, "text/plain", "OK");
}

static void handle_post_mute()
{
  if (server.hasArg("muted")) {
    bool muted = server.arg("muted").toInt() != 0;
    guessIsMuted = muted;
    update_mute_visual(muted);
  }
  server.send(200, "text/plain", "OK");
}

static void handle_post_nowplaying()
{
  if (server.hasArg("track") && server.hasArg("artist")) {
    update_now_playing(server.arg("track"), server.arg("artist"));
  }
  server.send(200, "text/plain", "OK");
}

// ---- POST /save_ha_conn -- HA host/port/token, editable from normal
// mode (no SoftAP hotspot, no restart). Added because typing a long HA
// long-lived access token into the tiny SoftAP setup form on a phone is
// painful -- once the board is on real WiFi, this same page is reachable
// from a real keyboard/browser instead. Writes straight to the same
// "netcfg" NVS namespace load_network_config() reads on boot, and also
// updates liveHaHost/liveHaPort/liveHaToken in memory immediately, so
// call_ha_service() picks up the change on its very next call -- no
// reboot needed.
//
// The token field is write-only by design: its current value is never
// echoed back into the form (see handle_root()), only a placeholder
// noting whether one is set, and this handler leaves the saved token
// untouched whenever the submitted field is blank -- so re-saving the
// host/port doesn't require retyping the token, and the token itself
// never round-trips through the browser after the first time you type it.
static void handle_save_ha_conn()
{
  if (server.hasArg("ha_host")) {
    liveHaHost = server.arg("ha_host");
    pcPrefs.begin("netcfg", false);
    nvs_put_str_checked(pcPrefs, "ha_host", liveHaHost);
    pcPrefs.end();
  }
  if (server.hasArg("ha_port") && server.arg("ha_port").length() > 0) {
    liveHaPort = server.arg("ha_port").toInt();
    pcPrefs.begin("netcfg", false);
    pcPrefs.putInt("ha_port", liveHaPort);
    pcPrefs.end();
  }
  if (server.hasArg("ha_token") && server.arg("ha_token").length() > 0) {
    liveHaToken = server.arg("ha_token");
    pcPrefs.begin("netcfg", false);
    nvs_put_str_checked(pcPrefs, "ha_token", liveHaToken);
    pcPrefs.end();
  } // blank token field -> leave the saved token exactly as it was

  server.sendHeader("Location", "/");
  server.send(303); // redirect back to the (now updated) editor page
}

// ---- POST /reconfigure_wifi -- sets the NVS flag connect_wifi() checks
// on next boot, then restarts into run_wifi_setup_portal(). This is the
// user-facing trigger for "change my WiFi network without Arduino IDE" --
// see load_network_config()/connect_wifi()/run_wifi_setup_portal() above.
// HA host/port/token no longer need this path at all -- see
// handle_save_ha_conn() above -- so this is WiFi-only now.
// Deliberately NOT the BOOT button (that's reserved for the ESP32's own
// bootloader-entry strap pin at power-on -- see check_boot_button()'s
// comment) -- this lives in the web editor instead, which is already
// where every other config change on this board happens.
static void handle_reconfigure_wifi()
{
  pcPrefs.begin("netcfg", false);
  pcPrefs.putBool("want_setup", true);
  pcPrefs.end();
  server.send(200, "text/html",
    "<html><body><h2>Restarting into WiFi setup mode...</h2>"
    "<p>In a few seconds, connect to the <b>PC_Companion_Setup</b> WiFi "
    "network from your phone or computer, then browse to "
    "<b>http://192.168.4.1/</b> to enter your new WiFi network's "
    "details.</p></body></html>");
  delay(500);
  ESP.restart();
}

void setup_web_server()
{
  server.on("/", HTTP_GET, handle_root);
  server.on("/save", HTTP_POST, handle_save);
  server.on("/save_ha", HTTP_POST, handle_save_ha);
  server.on("/save_ha_conn", HTTP_POST, handle_save_ha_conn);
  server.on("/config", HTTP_GET, handle_get_config);
  server.on("/telemetry", HTTP_POST, handle_post_telemetry);
  server.on("/mute", HTTP_POST, handle_post_mute);
  server.on("/nowplaying", HTTP_POST, handle_post_nowplaying);
  server.on("/reconfigure_wifi", HTTP_POST, handle_reconfigure_wifi);
  server.begin();
  Serial.println("[NET] Web server started on port 80");
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
      save_last_known_cache(); // mirror to SD -- see load_last_known_cache()
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

// ---- WiFi/HA credentials: NVS-backed, secrets.h only seeds first boot ----
// secrets.h's #defines used to be the ONLY source for these -- changing
// networks meant editing that file and reflashing via Arduino IDE, full
// stop. That's a real problem on a computer with no Arduino IDE (see the
// roadmap's "WiFi setup without Arduino IDE" writeup). Now they live in
// NVS (Preferences, namespace "netcfg"), with secrets.h's values used
// only to seed NVS the very first time the board ever boots -- so a
// board flashed once with real credentials in secrets.h still behaves
// exactly as before, but every board also gets a working fallback path
// once those credentials are wrong or missing (see run_wifi_setup_portal()).
// (WIFI_PLACEHOLDER_SSID and the live* globals are declared up near
// pcPrefs/server, ahead of call_ha_service() -- see that comment.)

void load_network_config()
{
  pcPrefs.begin("netcfg", true); // read-only
  bool hasSsid = pcPrefs.isKey("ssid");
  pcPrefs.end();

  if (!hasSsid) {
    // Genuinely first-ever boot for this flash -- seed NVS from
    // secrets.h so every later boot reads from one place (NVS), not two.
    pcPrefs.begin("netcfg", false);
    nvs_put_str_checked(pcPrefs, "ssid", WIFI_SSID);
    nvs_put_str_checked(pcPrefs, "pass", WIFI_PASSWORD);
    nvs_put_str_checked(pcPrefs, "ha_host", HA_HOST);
    pcPrefs.putInt("ha_port", HA_PORT);
    nvs_put_str_checked(pcPrefs, "ha_token", HA_TOKEN);
    pcPrefs.end();
  }

  pcPrefs.begin("netcfg", true);
  liveWifiSsid = pcPrefs.getString("ssid", WIFI_SSID);
  liveWifiPassword = pcPrefs.getString("pass", WIFI_PASSWORD);
  liveHaHost = pcPrefs.getString("ha_host", HA_HOST);
  liveHaPort = pcPrefs.getInt("ha_port", HA_PORT);
  liveHaToken = pcPrefs.getString("ha_token", HA_TOKEN);
  pcPrefs.end();
}

// ---- WiFi setup portal (SoftAP + plain web form) ----
// Fallback used when the board has no working WiFi to join -- either
// genuinely never configured, or the user asked to reconfigure it (see
// handle_reconfigure_wifi()). NEVER RETURNS: saving the form writes to
// NVS and reboots; this function owns the board completely until then.
//
// Deliberately does not touch LVGL, ESP-SR, or HID -- this runs from
// inside connect_wifi(), which setup() calls before any of those are
// initialized (see setup()'s ordering), so there is zero contention
// with LVGL's tight 64KB LV_MEM_SIZE pool or ESP-SR's model RAM, the two
// things that have actually crashed/frozen this board before (see the
// roadmap's "LVGL memory pool lesson"). Reuses the same global `server`
// WebServer instance normal operation uses for /config and /telemetry --
// setup_web_server() is simply never reached on a boot that takes this
// path, so there's no port/route conflict.
//
// Deliberately skips a DNS-hijack "auto-popup" captive portal (the kind
// phones auto-open a login page for) -- extra always-on task/socket,
// inconsistent across phone OSes. Simpler: connect to the hotspot,
// browse to the printed IP (192.168.4.1 by default) by hand.
static void handle_wifi_setup_root()
{
  String html = "<!DOCTYPE html><html><head><title>PC Companion Setup</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:20px;max-width:420px;}"
    "input{width:100%;padding:8px;margin:4px 0 12px 0;box-sizing:border-box;}"
    "button{padding:12px 24px;font-size:16px;}label{font-weight:bold;}"
    "h3{margin-top:28px;}</style>"
    "</head><body>"
    "<h2>PC Companion -- WiFi Setup</h2>"
    "<p>Enter your WiFi network's name and password. The board saves this "
    "and restarts, then joins that network on its own -- no computer or "
    "Arduino IDE needed.</p>"
    "<form method='POST' action='/setup_save'>"
    "<label>WiFi Network Name (SSID)</label>"
    "<input name='ssid' maxlength='32' required>"
    "<label>WiFi Password</label>"
    "<input name='pass' type='password' maxlength='63'>"
    "<h3>Home Assistant (optional)</h3>"
    "<p>Leave blank if you're not using the HA page.</p>"
    "<label>HA Host / IP</label><input name='ha_host' maxlength='63'>"
    "<label>HA Port</label><input name='ha_port' maxlength='5' value='8123'>"
    "<label>HA Long-Lived Access Token</label><input name='ha_token' maxlength='200'>"
    "<button type='submit'>Save and Restart</button>"
    "</form></body></html>";
  server.send(200, "text/html", html);
}

static void handle_wifi_setup_save()
{
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "text/plain", "SSID is required");
    return;
  }
  pcPrefs.begin("netcfg", false);
  nvs_put_str_checked(pcPrefs, "ssid", server.arg("ssid"));
  nvs_put_str_checked(pcPrefs, "pass", server.arg("pass"));
  if (server.hasArg("ha_host"))  nvs_put_str_checked(pcPrefs, "ha_host", server.arg("ha_host"));
  if (server.hasArg("ha_port") && server.arg("ha_port").length() > 0) {
    pcPrefs.putInt("ha_port", server.arg("ha_port").toInt());
  }
  if (server.hasArg("ha_token")) nvs_put_str_checked(pcPrefs, "ha_token", server.arg("ha_token"));
  pcPrefs.putBool("want_setup", false); // clear a pending reconfigure request, if any
  pcPrefs.end();

  server.send(200, "text/html",
    "<html><body><h2>Saved. Restarting...</h2>"
    "<p>The board will join your WiFi network in a few seconds.</p></body></html>");
  delay(500);
  ESP.restart();
}

static void run_wifi_setup_portal()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PC_Companion_Setup");
  IPAddress apIp = WiFi.softAPIP();
  Serial.println("[SETUP] ============================================");
  Serial.println("[SETUP] WiFi setup mode -- connect to WiFi network:");
  Serial.println("[SETUP]   PC_Companion_Setup");
  Serial.println("[SETUP] then browse to:");
  Serial.printf("[SETUP]   http://%s/\n", apIp.toString().c_str());
  Serial.println("[SETUP] ============================================");

  server.on("/", HTTP_GET, handle_wifi_setup_root);
  server.on("/setup_save", HTTP_POST, handle_wifi_setup_save);
  server.begin();

  while (true) {
    server.handleClient();
    delay(2);
  }
}

void connect_wifi()
{
  pcPrefs.begin("netcfg", true);
  bool forceSetup = pcPrefs.getBool("want_setup", false);
  pcPrefs.end();

  if (forceSetup) {
    Serial.println("[SETUP] Reconfigure-WiFi requested from the web editor -- entering setup mode");
    run_wifi_setup_portal(); // never returns
  }

  WiFi.begin(liveWifiSsid.c_str(), liveWifiPassword.c_str());
  Serial.print("Connecting to WiFi");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    // pc_companion_telemetry.py watches serial for exactly this format
    // to auto-discover the board's IP -- do not change without also
    // updating NET_IP_RE in that script. See announce_ip_periodically()
    // in loop() for the repeat-every-30s half of this.
    Serial.printf("[NET] IP=%s\n", WiFi.localIP().toString().c_str());
  } else if (liveWifiSsid == WIFI_PLACEHOLDER_SSID || liveWifiSsid.length() == 0) {
    // Never configured at all -- either secrets.h's placeholder, or an
    // empty WIFI_SSID (some builds leave it "" rather than the
    // placeholder string, e.g. testing without WiFi/HA set up yet) --
    // never overwritten by a real network either at flash time or via
    // the setup portal. Offer the portal instead of silently running
    // with no WiFi forever.
    Serial.println("\nWiFi never configured -- entering setup mode");
    run_wifi_setup_portal(); // never returns
  } else {
    // A real, previously-working network just isn't reachable right now
    // (router reboot, out of range, temporarily down). Don't force the
    // board into an unusable hotspot-only state over what might be a
    // temporary blip -- fall back to today's behavior: keep running,
    // HID/hotkeys/trackpad all still work, weather/HA/telemetry just
    // won't until WiFi comes back (or someone deliberately reconfigures
    // via the web editor's "Reconfigure WiFi" button).
    Serial.println("\nWiFi not connected - continuing without it");
  }
}

// Re-prints the [NET] IP= line every 30s so pc_companion_telemetry.py
// re-syncs if the board's IP ever changes (DHCP renewal, router
// reboot, etc) without needing a reboot/restart on either side.
static void announce_ip_periodically()
{
  static unsigned long lastAnnounce = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastAnnounce < 30000) return;
  lastAnnounce = millis();
  Serial.printf("[NET] IP=%s\n", WiFi.localIP().toString().c_str());
}

// ---- BOOT button (GPIO0) -- force-triggers the Jarvis AI screensaver ----
// GPIO0 doubles as the bootloader-entry strap pin, but that only matters
// at power-on/reset; reading it as a plain button during normal runtime
// (well after setup() has finished) is the standard, safe Arduino-ESP32
// pattern. Wired active-low with the internal pull-up, debounced in
// software. See jarvis_ai_screensaver.h for what the press actually does.
#define BOOT_BUTTON_PIN 0
#define BOOT_BUTTON_DEBOUNCE_MS 50
static int bootButtonLastRaw = HIGH;    // last raw digitalRead(), pre-debounce
static int bootButtonStable = HIGH;     // debounced state we've actually acted on
static unsigned long bootButtonLastChangeMs = 0;

static void check_boot_button()
{
  int reading = digitalRead(BOOT_BUTTON_PIN);

  if (reading != bootButtonLastRaw) {
    bootButtonLastRaw = reading;
    bootButtonLastChangeMs = millis();
  }

  if (millis() - bootButtonLastChangeMs > BOOT_BUTTON_DEBOUNCE_MS && reading != bootButtonStable) {
    bootButtonStable = reading;
    // DIAGNOSTIC: print every debounced state change, not just presses,
    // so we can see from Serial whether the pin is toggling AT ALL.
    Serial.printf("[JSS] BOOT pin debounced change -> %s\n", bootButtonStable == LOW ? "LOW (pressed)" : "HIGH (released)");
    if (bootButtonStable == LOW) { // pressed (active low, internal pull-up)
      jarvis_screensaver_force_show();
    }
  }
}

// ---- PWR button (TCA9554 I/O expander, EXIO6) -- force WiFi setup mode ----
// Unlike BOOT, this button is NOT a plain ESP32 GPIO -- per Waveshare's own
// docs for this board, it's wired through the same TCA9554 I2C expander
// already used for the LCD backlight-enable sequence in setup() (see
// TCA.pinMode1()/TCA.write1() there), read here with TCA.read1(). It's also
// active-HIGH (opposite of BOOT's active-low GPIO0) -- confirmed from
// Waveshare's documentation, not assumed.
//
// The board's own hardware already owns two edges of this button that we
// never touch: a single click powers the board on from fully off, and
// holding it 6+ seconds forces a hardware shutdown. This handler only runs
// during normal operation (loop(), well after boot), and fires on a short
// debounced press -- nowhere near the 6-second shutdown hold -- so it can't
// fight either of those. This is exactly the physical, no-WiFi-required
// trigger the SoftAP setup portal was missing (see connect_wifi()):
// pressing PWR sets the same "want_setup" NVS flag handle_reconfigure_wifi()
// sets from the web editor, then restarts straight into setup mode.
#define PWR_BUTTON_EXIO 6
#define PWR_BUTTON_DEBOUNCE_MS 50
// I2C reads cost more than a GPIO read -- don't hammer the bus every
// loop() pass. 200ms found by testing (Sep 2026): 50ms was frequent
// enough to add noticeable touch-scroll jitter, since this shares the
// same I2C bus the touch controller polls on every drag/scroll gesture.
// A deliberate single button press doesn't need sub-second response --
// worst case with debounce this still reacts within ~0.5s.
#define PWR_BUTTON_POLL_MS 200
static int pwrButtonLastRaw = LOW;   // last debounce-candidate reading (active-high)
static int pwrButtonStable = LOW;    // debounced state we've actually acted on
static unsigned long pwrButtonLastChangeMs = 0;
static unsigned long pwrButtonLastPollMs = 0;

static void check_pwr_button()
{
  if (millis() - pwrButtonLastPollMs < PWR_BUTTON_POLL_MS) return;
  pwrButtonLastPollMs = millis();

  int reading = TCA.read1(PWR_BUTTON_EXIO) ? HIGH : LOW;

  if (reading != pwrButtonLastRaw) {
    pwrButtonLastRaw = reading;
    pwrButtonLastChangeMs = millis();
  }

  if (millis() - pwrButtonLastChangeMs > PWR_BUTTON_DEBOUNCE_MS && reading != pwrButtonStable) {
    pwrButtonStable = reading;
    Serial.printf("[PWR] EXIO6 debounced change -> %s\n", pwrButtonStable == HIGH ? "HIGH (pressed)" : "LOW (released)");
    if (pwrButtonStable == HIGH) { // pressed (active high, per Waveshare docs)
      Serial.println("[PWR] PWR button pressed -- entering WiFi setup mode");
      pcPrefs.begin("netcfg", false);
      pcPrefs.putBool("want_setup", true);
      pcPrefs.end();
      delay(300); // let the Serial line flush before restarting
      ESP.restart();
    }
  }
}

// ---- Diagnostic helper: decode the reason the PREVIOUS boot ended ----
static const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON (normal power-on)";
    case ESP_RST_EXT:        return "EXT (external pin)";
    case ESP_RST_SW:         return "SW (esp_restart())";
    case ESP_RST_PANIC:      return "PANIC (exception/abort)";
    case ESP_RST_INT_WDT:    return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:   return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:        return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT (power/voltage sag)";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "OTHER";
  }
}

void setup()
{
#ifdef DEV_DEVICE_INIT
  DEV_DEVICE_INIT();
#endif
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP); // see check_boot_button() -- forces the Jarvis screensaver on

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
  TCA.pinMode1(PWR_BUTTON_EXIO, INPUT); // see check_pwr_button() -- forces WiFi setup mode

  bsp_touch_init(&Wire, -1, LCD_ROTATION, 320, 480);

  Serial.begin(115200);
  delay(1500);

  // ---- Diagnostic: print WHY the previous boot ended ----
  Serial.printf("Reset reason: %s\n", reset_reason_str(esp_reset_reason()));
  Serial.flush();

  Serial.println("CP1 - before USB.begin()"); Serial.flush();
  USB.begin();
  Serial.println("CP2 - after USB.begin(), before Keyboard.begin()"); Serial.flush();
  Keyboard.begin();
  Serial.println("CP3 - after Keyboard.begin(), before ConsumerControl.begin()"); Serial.flush();
  ConsumerControl.begin();
  Serial.println("CP3b - after ConsumerControl.begin(), before Mouse.begin()"); Serial.flush();
  Mouse.begin();
  Serial.println("CP4 - after Mouse.begin(), before connect_wifi()"); Serial.flush();

  setup_sd_card(); // before load_button_config() so a factory-fresh NVS can restore from it

  load_network_config(); // populate liveWifiSsid/liveWifiPassword/liveHa* from NVS before connect_wifi() reads them
  connect_wifi();
  Serial.println("CP5 - after connect_wifi(), before fetch_geo_and_time()"); Serial.flush();
  load_button_config();
  load_ha_config();
  setup_web_server();
  fetch_geo_and_time();
  Serial.println("CP6 - after fetch_geo_and_time(), before setup_jarvis()"); Serial.flush();

  setup_jarvis();
  Serial.println("CP7 - after setup_jarvis()"); Serial.flush();

  Serial.println("pc_companion with Jarvis voice actions wired in");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  Serial.println("CP8 - after gfx->begin()"); Serial.flush();
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

  Serial.println("CP9 - before display buffer allocation"); Serial.flush();

#ifdef DIRECT_RENDER_MODE
  bufSize = screenWidth * screenHeight;
  disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  bufSize = screenWidth * 40;
  disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif

  Serial.println("CP10 - after display buffer allocation"); Serial.flush();

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

    // ---- Tile grid (2x3, plus HA hanging off Hotkeys 2) ----
    // Top row keeps its original left/right neighbors and gains a
    // down-swipe to the tile below it. Bottom row gets an up-swipe
    // back to its top-row parent. Trackpad has NO swipe directions
    // (see build_trackpad_page for why) -- it's reached by swiping in
    // from Hotkeys2, Media, or System, and left via its own three
    // header nav buttons (left/up/right). HA sits at (0,2), directly
    // under Hotkeys 2 -- swipe down from Hotkeys 2 to reach it, up to
    // come back. It has no left/right neighbors of its own.
    tileObjs[TILE_HOTKEYS]   = lv_tileview_add_tile(tv, 0, 0, (lv_dir_t)(LV_DIR_RIGHT | LV_DIR_BOTTOM));
    tileObjs[TILE_MEDIA]     = lv_tileview_add_tile(tv, 1, 0, (lv_dir_t)(LV_DIR_HOR | LV_DIR_BOTTOM));
    tileObjs[TILE_INFO]      = lv_tileview_add_tile(tv, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_BOTTOM));
    tileObjs[TILE_HOTKEYS2]  = lv_tileview_add_tile(tv, 0, 1, (lv_dir_t)(LV_DIR_RIGHT | LV_DIR_TOP | LV_DIR_BOTTOM));
    tileObjs[TILE_TRACKPAD]  = lv_tileview_add_tile(tv, 1, 1, (lv_dir_t)0);
    tileObjs[TILE_SYSTEM]    = lv_tileview_add_tile(tv, 2, 1, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_TOP));
    tileObjs[TILE_HA]        = lv_tileview_add_tile(tv, 0, 2, (lv_dir_t)(LV_DIR_TOP));
    tileInfoGlobal = tileObjs[TILE_INFO];
    tileSystemGlobal = tileObjs[TILE_SYSTEM];

    build_tile_header(TILE_HOTKEYS,  tileObjs[TILE_HOTKEYS],  false, true,  false, true);
    build_tile_header(TILE_MEDIA,    tileObjs[TILE_MEDIA],    true,  true,  false, true);
    build_tile_header(TILE_INFO,     tileObjs[TILE_INFO],     true,  false, false, true);
    build_tile_header(TILE_HOTKEYS2, tileObjs[TILE_HOTKEYS2], false, true,  true,  true);
    // TILE_TRACKPAD has no build_tile_header() call -- it builds its own
    // header (pill + 3 nav buttons, no page label) in build_trackpad_page().
    build_tile_header(TILE_SYSTEM,   tileObjs[TILE_SYSTEM],   true,  false, true,  false);
    build_tile_header(TILE_HA,       tileObjs[TILE_HA],       false, false, true,  false);

    update_jarvis_headers(false);

    build_button_grid(tileObjs[TILE_HOTKEYS], livePage1, page1_btn_event_cb, page1MainLbl, page1SubLbl);
    build_media_page(tileObjs[TILE_MEDIA]);
    build_info_page(tileObjs[TILE_INFO]);
    build_button_grid(tileObjs[TILE_HOTKEYS2], livePage1b, page1b_btn_event_cb, page1bMainLbl, page1bSubLbl);
    build_trackpad_page(tileObjs[TILE_TRACKPAD]);
    build_system_page(tileObjs[TILE_SYSTEM]);
    build_ha_grid(tileObjs[TILE_HA], liveHA, ha_btn_event_cb, haMainLbl, haSubLbl);

    jarvis_screensaver_init(lv_scr_act()); // built last so it's above every tile in z-order
    Serial.println("[JSS] jarvis_screensaver_init() ran"); // DIAGNOSTIC -- confirms new firmware is actually running

    load_last_known_cache(); // show cached weather immediately -- see fetch_weather() for the live overwrite

    update_time_label();
    update_system_network_card();
    update_sd_status_card();
    fetch_weather();
  }

  Serial.println("Setup done");
}

unsigned long lastClockTick = 0;

void loop()
{
  lv_task_handler();

  poll_telemetry_serial();
  announce_ip_periodically();
  server.handleClient();
  check_boot_button();
  check_pwr_button();

  // Wake-word -> screensaver/header hand-off. See jssWakeShowPending's
  // and jssHeaderUpdatePending's comments above onSrEvent() -- these
  // flags are only ever set from that (other-task) callback and acted
  // on here, in the same loop()/LVGL thread as everything else that
  // touches LVGL.
  if (jssHeaderUpdatePending) {
    jssHeaderUpdatePending = false;
    update_jarvis_headers(jssHeaderListening);
  }
  if (jssWakeShowPending) {
    jssWakeShowPending = false;
    jarvis_screensaver_wake_show();
  }
  if (jssWakeDismissPending) {
    jssWakeDismissPending = false;
    jarvis_screensaver_wake_dismiss();
  }

  jarvis_screensaver_tick();

  if (millis() - lastClockTick >= 1000) {
    lastClockTick = millis();
    update_time_label();
  }

  static unsigned long lastWeatherCheck = 0;
  if (millis() - lastWeatherCheck >= 5000) {
    lastWeatherCheck = millis();
    maybe_refresh_weather();
    check_telemetry_staleness();
  }

  delay(5);
}
