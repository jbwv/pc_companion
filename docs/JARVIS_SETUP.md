# Adding a Custom Wake Word ("Jarvis") with ESP-SR

This board can listen for a wake word and respond to voice commands
using Espressif's ESP-SR library. Out of the box, the bundled example
only ships with the default wake word ("Hi ESP"). This guide covers
building a custom `srmodels.bin` with a different wake word --
**"Jarvis"** in this case -- and getting it running on real hardware.

ESP-SR itself is a normal Arduino library (`ESP_SR`), already bundled
with the `esp32` board package (3.0.2+) -- no extra install needed for
day-to-day use. The **one-time exception** is building a custom
`srmodels.bin`, which requires the ESP-IDF toolchain. Everything after
that step happens back in Arduino IDE as normal.

## Part 1: One-Time ESP-IDF Setup

You only need to do this section once, regardless of how many times
you change the wake word/command set later.

1. Download the **Espressif Installation Manager (EIM)** -- the CLI
   version -- from https://dl.espressif.com/dl/eim/
2. Run it:
   ```powershell
   .\eim-cli-windows-x64.exe install
   ```
   This downloads the full ESP-IDF toolchain (compilers, Python
   environment, build tools). It's a large download and will take a
   while -- let it run to completion.
3. Once done, the installer places a **desktop shortcut** for an
   "ESP-IDF PowerShell" -- a terminal with the environment already
   activated. Use this shortcut (not a regular PowerShell window) for
   every command in Part 2.
4. Clone the ESP-SR repository somewhere convenient:
   ```powershell
   git clone https://github.com/espressif/esp-sr.git
   ```

## Part 2: Building a Custom `srmodels.bin`

Do this whenever you want to change the wake word or command-model
set. All commands below run in the **ESP-IDF PowerShell**.

1. Create a minimal ESP-IDF project and link `esp-sr` into it:
   ```powershell
   idf.py create-project sr_jarvis
   cd sr_jarvis
   mkdir components
   cmd /c mklink /J components\esp-sr "<path to your cloned esp-sr folder>"
   idf.py set-target esp32s3
   ```

2. Open the model selection menu:
   ```powershell
   idf.py menuconfig
   ```
   Inside menuconfig, press `/` to search -- it's much faster than
   navigating the nested menus manually.
   - Search `wake word`, open **"Load Multiple Wake Words (WakeNet9 or
     WakeNet10)"**, find **Jarvis (`wn9_jarvis_tts`)**, press Space to
     check it. Leave every other wake word unchecked.
   - Search `multinet`, find an English MultiNet model. **Use a
     quantized (Q8) model** -- `SR_MN_EN_MULTINET5_SINGLE_RECOGNITION_QUANT8`
     is a good choice. This matters: this board's default model
     partition is only ~3968 KB, and the larger non-quantized MultiNet6
     model doesn't fit. The Q8 model comes in comfortably smaller.
   - Press `Q` to quit, save when prompted.

3. Build once (this generates the config the packaging script reads):
   ```powershell
   idf.py build
   ```

4. Package the selected models into a real `srmodels.bin`:
   ```powershell
   cd <path to your cloned esp-sr>\model
   python movemodel.py -d1 "<path to sr_jarvis>\sdkconfig" -d2 "<path to your cloned esp-sr>" -d3 "<path to sr_jarvis>\build"
   ```
   This prints a report of exactly which models got packaged and their
   total size -- confirm it's comfortably under 3968 KB before moving on.

5. Find the finished file at:
   ```
   <path to sr_jarvis>\build\srmodels\srmodels.bin
   ```

## Part 3: Installing the New Model File

1. Locate your current `srmodels.bin` (back it up first):
   ```powershell
   Get-ChildItem "C:\Users\<you>\AppData\Local\Arduino15\packages\esp32\tools\esp32s3-libs\<version>\esp_sr\srmodels.bin"
   ```
2. Copy the new file over it:
   ```powershell
   Copy-Item "<path to sr_jarvis>\build\srmodels\srmodels.bin" "C:\Users\<you>\AppData\Local\Arduino15\packages\esp32\tools\esp32s3-libs\<version>\esp_sr\srmodels.bin" -Force
   ```
3. **Clear Arduino's build cache** before reflashing, so it doesn't
   reuse a stale cached copy of the old model:
   ```powershell
   Remove-Item "C:\Users\<you>\AppData\Local\arduino\sketches\*" -Recurse -Force
   ```

## Part 4: Board-Specific Firmware Setup

Back in Arduino IDE from here on.

**Confirmed I2S pins for this board's onboard microphone** (found in
Waveshare's own official demo examples, not the generic ESP_SR sample
code):

```cpp
#define I2S_MCK_PIN   44
#define I2S_BCK_PIN   13
#define I2S_LRCK_PIN  15
#define I2S_DOUT_PIN  16
#define I2S_DIN_PIN   14  // microphone data-in
```

The microphone routes through an **ES8311 audio codec chip**, which
needs to be explicitly initialized over I2C before it will pass audio
onto the I2S bus:

```cpp
#include "Wire.h"
#include "es8311.h"
#include "esp_check.h"

static const char *TAG = "your_tag_here";

static esp_err_t es8311_codec_init(void) {
  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = 16000 * 256,
    .sample_frequency = 16000
  };
  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, 70, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
  return ESP_OK;
}
```

Call `Wire.begin(I2C_SDA, I2C_SCL)` and `es8311_codec_init()` before
initializing I2S and ESP_SR.

## Board Settings

- **Partition Scheme:** "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"
  -- required for the dedicated model storage partition
- **USB Mode:** USB-OTG (TinyUSB) -- for normal operation with HID
  keyboard/mouse output
- **Upload Mode:** USB-OTG CDC (TinyUSB)

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) if the board hangs at
`USB.begin()` during bring-up -- that's a known, expected symptom of
testing under the wrong USB Mode, not a real bug.

## Result

With all of the above in place, the board correctly detects the
"Jarvis" wake word and recognizes configured command phrases, firing
real actions in response.

## Part 5: Making Voice Commands Actually Work Reliably

Getting the wake word working is only half the story. The command
words that follow "Jarvis" (e.g. "Kairos", "Lock") need to be
**converted to phonemes** before MultiNet can reliably recognize them
-- plain English text works for some words by rough approximation, but
short or uncommon words are unreliable without proper conversion.

1. In the same cloned `esp-sr` repo, navigate to the `tool` folder:
   ```powershell
   cd <path to your cloned esp-sr>\tool
   ```

2. The conversion script needs a few Python packages not included by
   default:
   ```powershell
   pip install g2p_en pandas
   ```

3. First run downloads two NLTK data files automatically. If it stops
   partway with a `LookupError` about a missing tagger resource, grab
   it directly:
   ```powershell
   python -c "import nltk; nltk.download('averaged_perceptron_tagger_eng')"
   ```

4. Convert your actual command phrases. Separate distinct commands
   with `;`, and group alternate phrasings for the same command with
   `,` (no spaces around either):
   ```powershell
   python multinet_g2p.py -t "Kairos;Telos;Toolbox,IT Toolbox;Lock"
   ```
   This prints each phrase converted into its phoneme-encoded form,
   e.g. `Lock` becomes `LnK`.

5. Use the converted strings -- not the plain English -- in your
   `sr_cmd_t` array in the firmware:
   ```cpp
   static const sr_cmd_t sr_commands[] = {
     {0, "KfRbS"},   // Kairos
     {1, "TfLbS"},   // Telos
     {2, "ToLBeKS"},        // Toolbox
     {2, "gT ToLBeKS"},     // IT Toolbox (same command, alt phrasing)
     {7, "LnK"},     // Lock
   };
   ```
   A single command ID can have multiple phoneme entries, same as the
   plain-text version -- useful for accepting a couple of natural
   variations for the same action.

## Reference material

- [jarvis_boot_log_reference.txt](jarvis_boot_log_reference.txt) -- a
  real serial-monitor capture of a successful boot, for comparison.
- [jarvis_tools_settings.png](jarvis_tools_settings.png) -- a
  screenshot of the exact Arduino IDE Tools menu settings used on a
  known-working build.
