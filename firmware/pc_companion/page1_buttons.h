// ============================================================
// Page 1 button configuration -- this is the ONLY file most
// people need to edit to make Page 1 their own.
//
// Each row is one button: {label, subLabel, modifier1, modifier2, key}
// - label: the big, bold word -- also what you SAY to Jarvis to
//   trigger it by voice
// - subLabel: smaller, fainter text underneath -- use "" for none
// - modifier1/modifier2: use 0 if not needed
// - Common modifiers: KEY_LEFT_CTRL, KEY_LEFT_ALT,
//   KEY_LEFT_SHIFT, KEY_LEFT_GUI (Windows key)
// - key: the actual letter/character sent alongside the
//   modifiers, e.g. 'k' for Ctrl+Alt+K
//
// Order below = grid order, left-to-right, top-to-bottom.
// This same order is also the voice command ID order -- see
// SR_COMMAND_PHRASES in the main sketch.
//
// NOTE: THE "TOOLBOX" AND "MITHRIL" BUTTONS BELOW DO NOT WORK OUT OF
// THE BOX. They're wired to two of the original author's other
// personal projects (an IT admin toolbox app and a TUI called Mithril)
// that have not been released on GitHub yet -- see docs/SETUP.md
// section 5. Relabel these two rows (and their matching entries in
// PAGE1_DEFAULT_ACTIONS in pc_companion.ino, and hotkeys.ahk) to point
// at your own scripts/apps.
// ============================================================

struct Page1Button {
  const char* label;
  const char* subLabel;
  uint8_t modifier1;
  uint8_t modifier2;
  char key;
};

const Page1Button PAGE1_BUTTONS[BTNS_PER_PAGE] = {
  {"Kairos",         "Start day",     KEY_LEFT_CTRL, KEY_LEFT_ALT,   'k'},
  {"Telos",          "End day",       KEY_LEFT_CTRL, KEY_LEFT_ALT,   't'},
  {"Toolbox",        "IT Toolbox",    KEY_LEFT_CTRL, KEY_LEFT_ALT,   'i'},
  {"Mithril",        "1 TUI 2 Rule",  KEY_LEFT_CTRL, KEY_LEFT_ALT,   'm'},
  {"Snip",           "Snipping Tool", KEY_LEFT_GUI,  KEY_LEFT_SHIFT, 's'},
  {"PowerShell",     "",              KEY_LEFT_CTRL, KEY_LEFT_ALT,   'p'},
  {"Notepad",        "",              KEY_LEFT_CTRL, KEY_LEFT_ALT,   'n'},
  {"Lock",           "Lock PC",       KEY_LEFT_GUI,  0,              'l'},
};
