// ============================================================
// Hotkeys 2 (second bank, under Hotkeys) button configuration.
// Same format and rules as page1_buttons.h -- edit this file to
// make Hotkeys 2 your own.
//
// These 8 are PLACEHOLDERS (Ctrl+Alt+1 .. Ctrl+Alt+8) so the sketch
// compiles and the page is fully functional out of the box -- replace
// the label/subLabel/modifiers/key with whatever you actually want
// on this page.
//
// NOTE: unlike Hotkeys (Page 1), these are touch-only for now -- they
// are not wired into Jarvis voice commands. See the comment above
// sr_commands in the main sketch for how to add voice mapping once
// you've decided what these buttons should say.
// ============================================================

const Page1Button PAGE1B_BUTTONS[BTNS_PER_PAGE] = {
  {"Custom 1", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '1'},
  {"Custom 2", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '2'},
  {"Custom 3", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '3'},
  {"Custom 4", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '4'},
  {"Custom 5", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '5'},
  {"Custom 6", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '6'},
  {"Custom 7", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '7'},
  {"Custom 8", "", KEY_LEFT_CTRL, KEY_LEFT_ALT, '8'},
};
