// ---- Jarvis AI screensaver -----------------------------------------
// Full-screen idle overlay, sitting above the tileview on lv_scr_act().
// Recreates the "reactor HUD" look (rotating rings, glowing core,
// squashing eyes, corner brackets, a dim breathing standby dot) using
// plain LVGL widgets/animations -- there's no browser on this board, so
// the original HTML/CSS/SVG mock (tomogachi_hud.html) can't run here
// directly; this is a from-scratch LVGL translation of the same visual
// language, not a port of the file itself.
//
// Three intensity tiers, driven by LVGL's own built-in input-activity
// clock (lv_display_get_inactive_time()) so no separate idle-tracking
// code is needed -- any touch anywhere on the board already resets it:
//   6 min idle  -> JSS_WAKE    (brightest: fast rings, full eyes, brackets lit)
//   8 min idle  -> JSS_IDLE    (calmer: slow rings, squashed eyes, dim brackets)
//   10 min idle -> JSS_STANDBY (near-black, one faint breathing dot -- everything
//                                else hidden, screen-burn-conscious)
// A touch anywhere on the overlay dismisses it immediately and hands
// control back to whatever tile was showing.
//
// The BOOT button (GPIO0) force-triggers it on demand regardless of the
// timer -- see check_boot_button() in the main .ino, which calls
// jarvis_screensaver_force_show() here. A forced show ignores the timer
// cascade entirely (jumps straight to the brightest JSS_WAKE look) and
// stays up until touched, same as a timer-triggered one.
//
// Saying the wake word also pops it open (jarvis_screensaver_wake_show()),
// but unlike BOOT it lets go on its own once Jarvis stops listening
// (jarvis_screensaver_wake_dismiss(), called on SR_EVENT_TIMEOUT) rather
// than requiring a touch -- see the main .ino's onSrEvent()/loop() for
// why that hand-off goes through a polled flag instead of calling these
// directly from the speech-recognition callback.
//
// Usage from pc_companion.ino:
//   jarvis_screensaver_init(lv_scr_act());   // once, in setup(), after the tileview exists
//   jarvis_screensaver_tick();               // every loop()
//   jarvis_screensaver_force_show();         // on a BOOT button press
//   jarvis_screensaver_wake_show();          // on wake word (via a loop()-polled flag)
//   jarvis_screensaver_wake_dismiss();       // on SR_EVENT_TIMEOUT (same)

#ifndef JARVIS_AI_SCREENSAVER_H
#define JARVIS_AI_SCREENSAVER_H

#include <lvgl.h>
#include "esp_heap_caps.h" // for the heap_caps_* diagnostic in jss_apply_state()

// Declared in pc_companion.ino, defined well after this header is
// included (it's #include'd near the top of the file alongside
// page1_buttons.h/page1b_buttons.h, same as those). extern here so
// this header doesn't care about include order.
extern uint32_t screenWidth;
extern uint32_t screenHeight;
extern lv_display_t *disp;

// ---- Timing (ms). Change these to retune the cascade. ----
#define JSS_WAKE_MS    (6UL * 60UL * 1000UL)   // 6 min
#define JSS_IDLE_MS    (8UL * 60UL * 1000UL)   // 6+2 min
#define JSS_STANDBY_MS (10UL * 60UL * 1000UL)  // 6+2+2 min

// ---- Palette (mirrors tomogachi_hud.html's --cyan family) ----
#define JSS_COLOR_CYAN       lv_color_hex(0x3fe7ff)
#define JSS_COLOR_CYAN_MID   lv_color_hex(0x1fa8c4)
#define JSS_COLOR_CYAN_DIM   lv_color_hex(0x0e4650)
#define JSS_COLOR_CORE       lv_color_hex(0xeafbff)

typedef enum {
  JSS_OFF = 0,
  JSS_WAKE,
  JSS_IDLE,
  JSS_STANDBY
} jss_state_t;

// ---- Module state ----
static lv_obj_t *jssOverlay = NULL;
static lv_obj_t *jssFrame = NULL;      // outer bordered rect
static lv_obj_t *jssBracket[4] = {NULL, NULL, NULL, NULL}; // corner L's, each 2 thin rects
static lv_obj_t *jssBracketV[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *jssRingOuter = NULL;  // lv_arc
static lv_obj_t *jssRingInner = NULL;  // lv_arc
static lv_obj_t *jssCore = NULL;       // circle
static lv_obj_t *jssEyeL = NULL;
static lv_obj_t *jssEyeR = NULL;
static lv_obj_t *jssDot = NULL;        // standby-only breathing dot
static int32_t jssEyeCenterY = 0;      // set in init(); eyes resize around this, not transform-scaled
static int32_t jssDotCenterX = 0;      // set in init(); dot breathes by resizing around this point
static int32_t jssDotCenterY = 0;

static jss_state_t jssCurrentState = JSS_OFF;
static bool jssForced = false;
// Wake-word display is up. Needed for the exact same reason jssForced
// is: without a flag like this, jarvis_screensaver_tick() reconciles
// jssCurrentState against the REAL idle timer every single loop() pass,
// with no idea a voice trigger just intentionally set it. wake_show()
// sets state to JSS_WAKE, and on real hardware idle time is still only
// a few seconds -- so the very next tick() call (same loop() iteration)
// saw "wanted=JSS_OFF, current=JSS_WAKE, mismatch" and immediately
// called hide(), undoing the show before the display ever got a chance
// to render it. Same class of bug as everything else in this project
// that only shows up once two independently-reasonable pieces of logic
// run back to back in the same pass.
static bool jssWakeActive = false;
static unsigned long jssWakeActiveSinceMs = 0; // millis() when wake_show() last (re)armed it

// Safety net: if ESP_SR's own SR_EVENT_TIMEOUT never arrives for
// whatever reason (library quirk on total silence, a missed event, a
// stuck SR task -- anything upstream of this file), the wake-triggered
// face would otherwise sit there forever, since jssWakeActive blocks
// the normal idle-timer reconciliation on purpose. This is deliberately
// generous relative to ESP_SR's own listening window (confirmed via
// testing there's plenty of time in the default window) so it never
// fires as a false dismiss during a real conversation -- it only ever
// matters if the real timeout genuinely never shows up.
#define JSS_WAKE_SAFETY_MS (25UL * 1000UL)

// ---- Small anim exec callbacks (LVGL9 signature: void(void*, int32_t)) ----
// FIX: was lv_obj_set_style_transform_rotation() -- a generic style
// transform forces LVGL to allocate one big non-chunkable "layer" buffer
// sized to the whole widget (190x190px here, ~72KB) before it can draw,
// which blows straight through this build's 64KB LV_MEM_SIZE pool. A
// failed allocation there doesn't just fail quietly -- LV_USE_ASSERT_MALLOC
// is on and LV_ASSERT_HANDLER is `while(1);`, so it hangs the whole MCU
// forever. lv_arc_set_rotation() rotates the arc's own drawn angles
// directly, through the arc widget's normal (cheap, chunkable) draw path
// -- no transform layer, no giant allocation, same visual result.
static void jss_anim_rotation_cb(void *obj, int32_t v) { lv_arc_set_rotation((lv_obj_t *)obj, v); }
static void jss_anim_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
// FIX: the standby dot's "breathe" used to be a transform_scale (same
// problem class as the rings/core above -- see jss_anim_rotation_cb's
// comment) and was the one spot that never got converted, which is what
// froze the board on the very first WAKE->STANDBY transition (the first
// time this code path ever ran). This resizes the dot directly instead --
// a plain layout change, no transform layer -- and re-centers it each
// frame around the fixed point recorded in jssDotCenterX/Y so it grows
// and shrinks in place instead of drifting from a corner.
static void jss_anim_dot_size_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  lv_obj_set_size(o, v, v);
  lv_obj_set_pos(o, jssDotCenterX - v / 2, jssDotCenterY - v / 2);
}

static void jss_stop_anim(lv_obj_t *obj)
{
  lv_anim_delete(obj, jss_anim_rotation_cb);
  lv_anim_delete(obj, jss_anim_opa_cb);
  lv_anim_delete(obj, jss_anim_dot_size_cb);
}

// Continuous linear spin, one direction. periodMs is time for a full
// 360 degrees; negative-looking "reverse" is just start/end swapped.
// obj must be an lv_arc (drives lv_arc_set_rotation(), degrees 0-360 --
// NOT the old 0.1-degree style-transform units).
static void jss_start_spin(lv_obj_t *obj, uint32_t periodMs, bool reverse)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, jss_anim_rotation_cb);
  lv_anim_set_time(&a, periodMs);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  if (reverse) lv_anim_set_values(&a, 360, 0);
  else         lv_anim_set_values(&a, 0, 360);
  lv_anim_start(&a);
}

// Ease-in-out breathe: the standby dot grows/shrinks (in actual pixels,
// via jss_anim_dot_size_cb -- not a transform) between lo/hi forever.
// Mirrors tomogachi_hud.html's standby-breathe keyframes (scale 1 -> 1.4,
// 5s ease-in-out) -- lo/hi here are passed as pixel diameters instead of
// a scale factor.
static void jss_start_breathe_dot_size(lv_obj_t *obj, uint32_t periodMs, int32_t lo, int32_t hi)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, jss_anim_dot_size_cb);
  lv_anim_set_time(&a, periodMs / 2);
  lv_anim_set_playback_time(&a, periodMs / 2);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_values(&a, lo, hi);
  lv_anim_start(&a);
}
static void jss_start_breathe_opa(lv_obj_t *obj, uint32_t periodMs, int32_t lo, int32_t hi)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, jss_anim_opa_cb);
  lv_anim_set_time(&a, periodMs / 2);
  lv_anim_set_playback_time(&a, periodMs / 2);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_values(&a, lo, hi);
  lv_anim_start(&a);
}

// ---- Touch anywhere on the overlay dismisses it ----
static void jss_dismiss_cb(lv_event_t *e)
{
  jssForced = false;
  jssWakeActive = false;
  jssCurrentState = JSS_OFF;
  lv_obj_add_flag(jssOverlay, LV_OBJ_FLAG_HIDDEN);
  // Touching the overlay is itself an LVGL input event, so
  // lv_display_get_inactive_time() is already reset by the framework --
  // nothing else to do here.
}

// One-time build. Call after the tileview exists so this ends up on
// top of it in z-order (lv_obj_move_foreground below makes that
// explicit and future-proof against build-order changes).
static void jarvis_screensaver_init(lv_obj_t *parent)
{
  jssOverlay = lv_obj_create(parent);
  lv_obj_remove_style_all(jssOverlay);
  lv_obj_set_size(jssOverlay, screenWidth, screenHeight);
  lv_obj_set_pos(jssOverlay, 0, 0);
  lv_obj_set_style_bg_color(jssOverlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(jssOverlay, LV_OPA_COVER, 0);
  lv_obj_clear_flag(jssOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(jssOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(jssOverlay, jss_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_move_foreground(jssOverlay);

  uint32_t cx = screenWidth / 2;
  uint32_t cy = screenHeight / 2 - 20; // nudge up a bit, matches the original mock

  // ---- outer frame ----
  jssFrame = lv_obj_create(jssOverlay);
  lv_obj_remove_style_all(jssFrame);
  lv_obj_set_size(jssFrame, screenWidth - 28, screenHeight - 28);
  lv_obj_center(jssFrame);
  lv_obj_set_style_radius(jssFrame, 26, 0);
  lv_obj_set_style_bg_opa(jssFrame, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(jssFrame, 1, 0);
  lv_obj_set_style_border_color(jssFrame, JSS_COLOR_CYAN_DIM, 0);
  lv_obj_clear_flag(jssFrame, LV_OBJ_FLAG_CLICKABLE);

  // ---- corner brackets: each an L made of two thin rects ----
  int armLen = 22, armThick = 2, margin = 20;
  int cornerX[4] = {margin, (int)screenWidth - margin - armLen, margin, (int)screenWidth - margin - armLen};
  int cornerY[4] = {margin, margin, (int)screenHeight - margin - armLen, (int)screenHeight - margin - armLen};
  bool flipV[4]  = {false, false, true, true};   // vertical arm hangs down (top corners) or up (bottom corners)
  bool flipH[4]  = {false, true, false, true};   // horizontal arm goes right (left corners) or left (right corners)
  for (int i = 0; i < 4; i++) {
    lv_obj_t *h = lv_obj_create(jssOverlay);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, armLen, armThick);
    lv_obj_set_pos(h, flipH[i] ? (cornerX[i]) : cornerX[i], flipV[i] ? cornerY[i] + armLen - armThick : cornerY[i]);
    lv_obj_set_style_bg_color(h, JSS_COLOR_CYAN_MID, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    jssBracket[i] = h;

    lv_obj_t *v = lv_obj_create(jssOverlay);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, armThick, armLen);
    lv_obj_set_pos(v, flipH[i] ? cornerX[i] + armLen - armThick : cornerX[i], cornerY[i]);
    lv_obj_set_style_bg_color(v, JSS_COLOR_CYAN_MID, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    jssBracketV[i] = v;
  }

  // ---- rotating rings ----
  jssRingOuter = lv_arc_create(jssOverlay);
  lv_obj_remove_style_all(jssRingOuter);
  lv_obj_set_size(jssRingOuter, 190, 190);
  lv_obj_set_pos(jssRingOuter, cx - 95, cy - 95);
  lv_arc_set_bg_angles(jssRingOuter, 0, 300); // partial ring, like the dashed SVG one
  lv_arc_set_value(jssRingOuter, 0);
  lv_obj_set_style_arc_width(jssRingOuter, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(jssRingOuter, JSS_COLOR_CYAN_DIM, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(jssRingOuter, LV_OPA_TRANSP, LV_PART_INDICATOR); // hide the value indicator arc
  lv_obj_set_style_transform_pivot_x(jssRingOuter, 95, 0);
  lv_obj_set_style_transform_pivot_y(jssRingOuter, 95, 0);
  lv_obj_clear_flag(jssRingOuter, LV_OBJ_FLAG_CLICKABLE);

  jssRingInner = lv_arc_create(jssOverlay);
  lv_obj_remove_style_all(jssRingInner);
  lv_obj_set_size(jssRingInner, 160, 160);
  lv_obj_set_pos(jssRingInner, cx - 80, cy - 80);
  lv_arc_set_bg_angles(jssRingInner, 0, 250);
  lv_arc_set_value(jssRingInner, 0);
  lv_obj_set_style_arc_width(jssRingInner, 1, LV_PART_MAIN);
  lv_obj_set_style_arc_color(jssRingInner, JSS_COLOR_CYAN_MID, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(jssRingInner, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_transform_pivot_x(jssRingInner, 80, 0);
  lv_obj_set_style_transform_pivot_y(jssRingInner, 80, 0);
  lv_obj_clear_flag(jssRingInner, LV_OBJ_FLAG_CLICKABLE);

  // ---- core glow ----
  jssCore = lv_obj_create(jssOverlay);
  lv_obj_remove_style_all(jssCore);
  lv_obj_set_size(jssCore, 132, 132);
  lv_obj_set_pos(jssCore, cx - 66, cy - 66);
  lv_obj_set_style_radius(jssCore, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(jssCore, JSS_COLOR_CORE, 0);
  lv_obj_set_style_bg_opa(jssCore, LV_OPA_40, 0);
  // DIAGNOSTIC: shadows disabled -- see jss_apply_state()'s heap print.
  // lv_obj_set_style_shadow_width(jssCore, 40, 0);
  // lv_obj_set_style_shadow_color(jssCore, JSS_COLOR_CYAN, 0);
  // lv_obj_set_style_shadow_opa(jssCore, LV_OPA_60, 0);
  lv_obj_set_style_transform_pivot_x(jssCore, 66, 0);
  lv_obj_set_style_transform_pivot_y(jssCore, 66, 0);
  lv_obj_clear_flag(jssCore, LV_OBJ_FLAG_CLICKABLE);

  // ---- eyes ----
  jssEyeL = lv_obj_create(jssOverlay);
  lv_obj_remove_style_all(jssEyeL);
  lv_obj_set_size(jssEyeL, 42, 16);
  lv_obj_set_pos(jssEyeL, cx - 52, cy - 8);
  lv_obj_set_style_radius(jssEyeL, 8, 0);
  lv_obj_set_style_bg_color(jssEyeL, JSS_COLOR_CYAN, 0);
  lv_obj_set_style_bg_opa(jssEyeL, LV_OPA_COVER, 0);
  // lv_obj_set_style_shadow_width(jssEyeL, 14, 0);
  // lv_obj_set_style_shadow_color(jssEyeL, JSS_COLOR_CYAN, 0);
  // lv_obj_set_style_shadow_opa(jssEyeL, LV_OPA_70, 0);
  lv_obj_set_style_transform_pivot_x(jssEyeL, 21, 0);
  lv_obj_set_style_transform_pivot_y(jssEyeL, 8, 0);
  lv_obj_clear_flag(jssEyeL, LV_OBJ_FLAG_CLICKABLE);

  jssEyeR = lv_obj_create(jssOverlay);
  lv_obj_remove_style_all(jssEyeR);
  lv_obj_set_size(jssEyeR, 42, 16);
  lv_obj_set_pos(jssEyeR, cx + 10, cy - 8);
  lv_obj_set_style_radius(jssEyeR, 8, 0);
  lv_obj_set_style_bg_color(jssEyeR, JSS_COLOR_CYAN, 0);
  lv_obj_set_style_bg_opa(jssEyeR, LV_OPA_COVER, 0);
  // lv_obj_set_style_shadow_width(jssEyeR, 14, 0);
  // lv_obj_set_style_shadow_color(jssEyeR, JSS_COLOR_CYAN, 0);
  // lv_obj_set_style_shadow_opa(jssEyeR, LV_OPA_70, 0);
  lv_obj_set_style_transform_pivot_x(jssEyeR, 21, 0);
  lv_obj_set_style_transform_pivot_y(jssEyeR, 8, 0);
  lv_obj_clear_flag(jssEyeR, LV_OBJ_FLAG_CLICKABLE);

  jssEyeCenterY = cy; // eyes are positioned at (cy - 8) with height 16 -> center is cy

  // ---- standby dot (hidden unless JSS_STANDBY) ----
  jssDot = lv_obj_create(jssOverlay);
  lv_obj_remove_style_all(jssDot);
  lv_obj_set_size(jssDot, 6, 6);
  lv_obj_set_pos(jssDot, cx - 3, cy - 3);
  lv_obj_set_style_radius(jssDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(jssDot, JSS_COLOR_CYAN_DIM, 0);
  lv_obj_set_style_bg_opa(jssDot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(jssDot, 8, 0);
  lv_obj_set_style_shadow_color(jssDot, JSS_COLOR_CYAN_DIM, 0);
  lv_obj_set_style_shadow_opa(jssDot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_transform_pivot_x(jssDot, 3, 0);
  lv_obj_set_style_transform_pivot_y(jssDot, 3, 0);
  lv_obj_clear_flag(jssDot, LV_OBJ_FLAG_CLICKABLE);
  jssDotCenterX = cx; // dot is positioned at (cx-3, cy-3) with size 6x6 -> center is (cx, cy)
  jssDotCenterY = cy;

  lv_obj_add_flag(jssOverlay, LV_OBJ_FLAG_HIDDEN);
}

// Apply one of the three visible looks (or OFF, which just hides
// everything -- callers use jarvis_screensaver_hide() for that instead
// of calling this with JSS_OFF directly).
// DIAGNOSTIC: checkpoint print that blocks until it's actually out the
// UART (Serial.flush()), so if the very next line hangs the MCU, this
// checkpoint has still definitely been seen -- unlike a plain Serial.print
// which can sit in the TX buffer. Pinpoints exactly which line in
// jss_apply_state() the freeze is happening on. Remove once found.
#define JSS_CP(label) do { Serial.println("[JSS-CP] " label); Serial.flush(); } while (0)

static void jss_apply_state(jss_state_t state)
{
  if (jssOverlay == NULL) return;
  jssCurrentState = state;
  JSS_CP("start");
  lv_obj_clear_flag(jssOverlay, LV_OBJ_FLAG_HIDDEN);
  JSS_CP("cleared hidden flag");
  lv_obj_move_foreground(jssOverlay);
  JSS_CP("moved foreground");
  lv_obj_invalidate(jssOverlay);
  JSS_CP("invalidated overlay");

  // Stop any running animations before re-styling -- avoids stacking
  // multiple anims on the same object across repeated state changes.
  jss_stop_anim(jssRingOuter);
  JSS_CP("stopped ring outer anim");
  jss_stop_anim(jssRingInner);
  JSS_CP("stopped ring inner anim");
  jss_stop_anim(jssCore);
  JSS_CP("stopped core anim");
  jss_stop_anim(jssDot);
  JSS_CP("stopped dot anim");

  bool showFrameAndEyes = (state != JSS_STANDBY);
  lv_opa_t frameOpa    = (state == JSS_WAKE) ? LV_OPA_COVER : (state == JSS_IDLE ? LV_OPA_60 : LV_OPA_TRANSP);
  lv_opa_t bracketOpa  = (state == JSS_WAKE) ? LV_OPA_COVER : LV_OPA_TRANSP;
  lv_opa_t eyeOpa      = showFrameAndEyes ? LV_OPA_COVER : LV_OPA_TRANSP;
  // FIX: was a transform_scale_y (256=100%, ~90=35% squashed) -- same
  // problem class as the rings/core: a non-identity scale transform still
  // needs its own layer buffer even on a small object, and this is the
  // first state (JSS_IDLE/STANDBY) that ever asks for a non-identity value
  // here, which lines up with the freeze happening on the very first
  // WAKE->IDLE transition. Resizing height directly is a plain layout
  // change -- no transform layer, same squashed look.
  int32_t eyeH = (state == JSS_WAKE) ? 16 : 6;
  uint32_t ringPeriod  = (state == JSS_WAKE) ? 3000 : 26000;
  uint32_t ringPeriod2 = (state == JSS_WAKE) ? 2200 : 18000;
  lv_opa_t ringOpa     = showFrameAndEyes ? ((state == JSS_WAKE) ? LV_OPA_COVER : LV_OPA_60) : LV_OPA_TRANSP;
  uint32_t corePeriod  = (state == JSS_WAKE) ? 1000 : 4200;
  lv_opa_t coreOpa     = showFrameAndEyes ? LV_OPA_60 : LV_OPA_TRANSP;

  lv_obj_set_style_opa(jssFrame, frameOpa, 0);
  JSS_CP("set frame opa");
  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_opa(jssBracket[i], bracketOpa, 0);
    lv_obj_set_style_opa(jssBracketV[i], bracketOpa, 0);
  }
  JSS_CP("set bracket opas");
  lv_obj_set_style_opa(jssEyeL, eyeOpa, 0);
  lv_obj_set_style_opa(jssEyeR, eyeOpa, 0);
  JSS_CP("set eye opas");
  lv_obj_set_height(jssEyeL, eyeH);
  lv_obj_set_height(jssEyeR, eyeH);
  lv_obj_set_y(jssEyeL, jssEyeCenterY - eyeH / 2);
  lv_obj_set_y(jssEyeR, jssEyeCenterY - eyeH / 2);
  JSS_CP("set eye scale");

  lv_obj_set_style_arc_opa(jssRingOuter, ringOpa, LV_PART_MAIN);
  JSS_CP("set ring outer arc opa");
  lv_obj_set_style_arc_opa(jssRingInner, ringOpa, LV_PART_MAIN);
  JSS_CP("set ring inner arc opa");
  lv_obj_set_style_opa(jssCore, coreOpa, 0);
  JSS_CP("set core opa");

  if (state != JSS_STANDBY) {
    jss_start_spin(jssRingOuter, ringPeriod, false);
    JSS_CP("started ring outer spin");
    jss_start_spin(jssRingInner, ringPeriod2, true);
    JSS_CP("started ring inner spin");
    // FIX: was jss_start_breathe_scale() -- a scale transform on this
    // 132x132px core needs its own ~34KB non-chunkable layer buffer, same
    // problem class as the rings above. Breathing opacity instead gets a
    // similar pulsing look through the widget's normal opacity blending,
    // which LVGL can draw in small chunks -- no big layer allocation.
    lv_opa_t breatheLo = (state == JSS_WAKE) ? LV_OPA_40 : LV_OPA_20;
    lv_opa_t breatheHi = (state == JSS_WAKE) ? LV_OPA_COVER : LV_OPA_50;
    jss_start_breathe_opa(jssCore, corePeriod, breatheLo, breatheHi);
    JSS_CP("started core breathe");
  }

  if (state == JSS_STANDBY) {
    // Mirrors tomogachi_hud.html's standby-breathe keyframes (6px dot,
    // scale 1 -> 1.4 i.e. ~6px -> ~9px, opacity .25 -> .7, 5s ease-in-out).
    // FIX: was jss_start_breathe_scale() -- a transform_scale on this small
    // dot still needs its own layer buffer, and this was the one spot left
    // over from the rings/core/eyes fixes above, so it's what froze the
    // board the first time the cascade ever reached STANDBY. Resizing the
    // dot's actual width/height instead (jss_start_breathe_dot_size, which
    // re-centers it around jssDotCenterX/Y each frame) gets the same
    // "growing dot" look with no transform layer, same as the eye squash.
    jss_start_breathe_dot_size(jssDot, 5000, 6, 9);
    jss_start_breathe_opa(jssDot, 5000, LV_OPA_20, LV_OPA_70);
    lv_obj_set_style_bg_opa(jssDot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_opa(jssDot, LV_OPA_60, 0);
  } else {
    lv_obj_set_style_bg_opa(jssDot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(jssDot, LV_OPA_TRANSP, 0);
  }
  JSS_CP("handled dot");

  // DIAGNOSTIC: heap snapshot on every state change -- kept as a cheap
  // sanity check now that the actual cause (LV_MEM_SIZE-busting transform
  // layers on the rings/core, see jss_anim_rotation_cb above) is fixed.
  // Safe to remove once a few runs confirm this stays healthy.
  Serial.printf("[JSS] free heap: total=%u internal=%u largest_internal_block=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  // jssOverlay was already invalidated above -- this app's normal
  // loop()-driven lv_task_handler() picks it up on the very next pass,
  // same as every other UI update in this firmware (clock, weather,
  // button labels). The forced lv_refr_now()/extra lv_task_handler()
  // calls tried earlier were band-aids for the wrong problem.
}

// Hide it entirely (used both by the dismiss handler and by the
// timer tick when we've dropped back below the wake threshold, e.g.
// right after a touch resets the activity clock).
//
// FIX (found via a wake-word freeze, Sep 2026): this used to end with a
// forced lv_refr_now(NULL) -- a leftover from before jss_apply_state()
// was fixed to drop that exact pattern (see its own comment above,
// "band-aids for the wrong problem"). This function just never got the
// same cleanup. A forced synchronous full-screen redraw, called from
// inside the SR_EVENT_TIMEOUT hand-off, is now called far more often
// than it ever was before (every wake-word dismiss, not just an
// occasional touch), and a Serial log caught loop() going completely
// silent -- not just the screensaver, EVERY periodic print in loop(),
// meaning the whole board hung -- immediately after a "Timeout, back to
// listening" print, with a manual power cycle being the only way back.
// That's this line. Removed; jssOverlay is already hidden/invalidated
// above, so the very next lv_task_handler() pass (already called every
// loop(), same as everywhere else in this firmware) picks it up exactly
// like every other UI update here does.
static void jarvis_screensaver_hide()
{
  if (jssOverlay == NULL) return;
  if (jssCurrentState == JSS_OFF) return;
  jss_stop_anim(jssRingOuter);
  jss_stop_anim(jssRingInner);
  jss_stop_anim(jssCore);
  jss_stop_anim(jssDot);
  jssCurrentState = JSS_OFF;
  jssForced = false;
  lv_obj_add_flag(jssOverlay, LV_OBJ_FLAG_HIDDEN);
}

// BOOT button -- jump straight to the brightest look and stay there
// (ignoring the timer cascade) until touched. See check_boot_button()
// in the main .ino for the actual GPIO read/debounce.
static void jarvis_screensaver_force_show()
{
  jssForced = true;
  jss_apply_state(JSS_WAKE);
}

// ---- Wake-word trigger: "Hey Jarvis" pops the face open too ----
// Deliberately NOT called directly from the ESP_SR event callback
// (onSrEvent() in the main .ino) -- that callback fires from the
// speech-recognition library's own task, not the loop()/LVGL task, and
// jss_apply_state() is a much heavier LVGL operation (starts four
// animations, moves/invalidates a full-screen overlay) than the simple
// header-color updates that callback already does safely. LVGL's object
// tree isn't thread-safe, so calling this straight from another task
// risks exactly the kind of hard-to-reproduce corruption we just spent
// a long debugging session chasing down for a completely different
// reason (see style_hud_button()'s comment). Instead, onSrEvent() only
// sets a plain volatile flag; loop() polls it and calls this from the
// same thread lv_task_handler() runs on -- identical pattern to how
// check_boot_button() already handles the physical button.
//
// Unlike force_show(), this does NOT set jssForced -- a voice command
// should hand control back once Jarvis stops listening, not require a
// touch to dismiss. It DOES set jssWakeActive, though -- without that,
// jarvis_screensaver_tick() (below) reconciles state against the real
// idle timer on every single loop() pass and would immediately call
// hide() right back, since idle time is still only seconds at this
// point, undoing this call before the display ever renders it. Found
// the hard way: the show/dismiss prints all traced through cleanly and
// the board never crashed, but nothing visibly changed -- because it
// really was shown, then hidden again, all within the same loop() pass.
static void jarvis_screensaver_wake_show()
{
  jssWakeActiveSinceMs = millis(); // (re)arm the safety net on every wake, even a repeat one
  // Already up from a previous wake-word hit in the same conversation --
  // don't tear down and restart all four animations from scratch. That
  // churn (stop 4 anims, re-init them, re-invalidate the whole overlay)
  // was pure waste for something already visible and already correct,
  // and repeated rapid-fire wake words were the most likely way to
  // actually exercise whatever's behind the freeze we're chasing.
  if (jssWakeActive && jssCurrentState == JSS_WAKE) return;
  jssWakeActive = true;
  jss_apply_state(JSS_WAKE);
}

// Companion to the above, called once Jarvis times back out to
// wake-word listening (SR_EVENT_TIMEOUT), so the face doesn't sit there
// after a voice command finishes. No-ops if BOOT forced it open first --
// that should still require an actual touch to dismiss, same as always.
static void jarvis_screensaver_wake_dismiss()
{
  if (jssForced) return;
  jssWakeActive = false;
  jarvis_screensaver_hide();
}

// Call every loop(). No-ops almost instantly (a few comparisons) so
// it's cheap to call unconditionally.
static void jarvis_screensaver_tick()
{
  if (jssOverlay == NULL) return;
  if (jssForced) return; // BOOT: stays until touched, no safety net -- that's the point of BOOT

  if (jssWakeActive) {
    // Safety net: if SR_EVENT_TIMEOUT never actually arrives (ESP_SR
    // library quirk, a missed/dropped event, anything upstream of this
    // file), don't sit here forever -- see JSS_WAKE_SAFETY_MS's comment.
    // The heartbeat print is diagnostic: if the board genuinely locks up
    // rather than just "waiting," this is the last thing that'll show up
    // in the log, and how far the elapsed counter got tells us whether
    // loop() itself froze (prints stop advancing) or kept running fine
    // and this safety net is what actually saved it (prints keep going,
    // then a hide happens right around 25000ms).
    static unsigned long jssLastWakeHeartbeat = 0;
    if (millis() - jssLastWakeHeartbeat > 3000) {
      jssLastWakeHeartbeat = millis();
      Serial.printf("[JSS] wake-active heartbeat: %lums since last wake\n", (unsigned long)(millis() - jssWakeActiveSinceMs));
    }
    if (millis() - jssWakeActiveSinceMs > JSS_WAKE_SAFETY_MS) {
      Serial.println("[JSS] wake-word safety net fired -- SR_EVENT_TIMEOUT never arrived, dismissing anyway");
      jssWakeActive = false;
      jarvis_screensaver_hide();
    }
    return; // still let the idle timer stay out of this either way
  }

  uint32_t idleMs = lv_display_get_inactive_time(disp);

  // DIAGNOSTIC: print the running idle clock every 15s so we can watch
  // it count up (or catch it stuck at 0 / not counting) without waiting
  // the full 6-10 minutes to find out. Remove once confirmed working.
  static unsigned long jssLastDebugPrint = 0;
  if (millis() - jssLastDebugPrint > 15000) {
    jssLastDebugPrint = millis();
    Serial.printf("[JSS] idle=%lums state=%d forced=%d\n", (unsigned long)idleMs, (int)jssCurrentState, (int)jssForced);
  }

  jss_state_t wanted;
  if (idleMs >= JSS_STANDBY_MS) wanted = JSS_STANDBY;
  else if (idleMs >= JSS_IDLE_MS) wanted = JSS_IDLE;
  else if (idleMs >= JSS_WAKE_MS) wanted = JSS_WAKE;
  else wanted = JSS_OFF;

  if (wanted == JSS_OFF) {
    if (jssCurrentState != JSS_OFF) jarvis_screensaver_hide();
    return;
  }
  if (wanted != jssCurrentState) jss_apply_state(wanted);
}

#endif // JARVIS_AI_SCREENSAVER_H
