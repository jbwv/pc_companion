// ============================================================
// Page 1 button configuration — this is the ONLY file most
// people need to edit to make Page 1 their own.
//
// Each row is one button: {label, modifier1, modifier2, key}
// - modifier1/modifier2: use 0 if not needed
// - Common modifiers: KEY_LEFT_CTRL, KEY_LEFT_ALT,
//   KEY_LEFT_SHIFT, KEY_LEFT_GUI (Windows key)
// - key: the actual letter/character sent alongside the
//   modifiers, e.g. 'k' for Ctrl+Alt+K
//
// Order below = grid order, left-to-right, top-to-bottom.
// ============================================================
struct Page1Button {
  const char* label;
  uint8_t modifier1;
  uint8_t modifier2;
  char key;
};
const Page1Button PAGE1_BUTTONS[BTNS_PER_PAGE] = {
  {"Kairos",         KEY_LEFT_CTRL, KEY_LEFT_ALT,   'k'},
  {"Telos",          KEY_LEFT_CTRL, KEY_LEFT_ALT,   't'},
  {"File Explorer",  KEY_LEFT_GUI,  0,              'e'},
  {"Task Manager",   KEY_LEFT_CTRL, KEY_LEFT_SHIFT, KEY_ESC},
  {"Snipping Tool",  KEY_LEFT_GUI,  KEY_LEFT_SHIFT, 's'},
  {"PowerShell",     KEY_LEFT_CTRL, KEY_LEFT_ALT,   'p'},
  {"Notepad",        KEY_LEFT_CTRL, KEY_LEFT_ALT,   'n'},
  {"Lock Computer",  KEY_LEFT_GUI,  0,              'l'},
};