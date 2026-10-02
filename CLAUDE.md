# VoCat: ESP Private Agents on the CyberAI EchoEar Pro

Port of Espressif's ESP Private Agents `voice_chat` example (esp-agents-firmware) to a CyberAI
"EchoEar Pro" (Taobao shop 赛博智能, cyberailife.com): a Wi-Fi-only ESP32-S3 clone of Espressif's
ESP-VoCat/EchoEar with its own pinout and power circuit. It runs a desk dashboard (clock + weather, a USD
rate ticker) and "quick keys": Media and Desktop key pages on the round screen, sent to the computer as a
USB keyboard. Two apps share that UI:

- `desk_keys`: dashboard + quick keys only. No voice, no accounts: Wi-Fi is set up from any phone or laptop
  through the board's own setup network and captive page. This is the app on the board now.
- `voice_chat`: the same plus the voice agent (RainMaker login, BLE provisioning, timers, conversation view).

This project is separate from the Anna Home Assistant / ESPHome stack: don't build on it or name
things after Anna.

## Layout

- `firmware/`: esp-agents-firmware, imported from https://github.com/espressif/esp-agents-firmware at
  c5d6798 (2026-05-25) with the port on top. Compare against upstream to see what the port changed; this
  repository starts at a single commit, so the import is not a commit of its own.
  - `examples/common/boards/cyberai_echoear/`: board definition (pins, devices, `sdkconfig.defaults`,
    feature flags in `board_defs.h`, 360x360 emote layout override)
  - `examples/common/app_ui/`: the screen and USB side, with no voice dependencies: display
    (`app_display.c`), dashboard (`app_dashboard.c`), quick keys (`app_quick_keys.c`), the USB keyboard
    (`app_usb_hid.c`, TinyUSB HID + CDC control port), the saved settings (`app_settings.c`) and switching
    off (`app_power.c`). Voice behaviour plugs in through hooks (`app_display_set_touch_cb`,
    `app_dashboard_configure`, `app_display_show_qrcode`).
  - `examples/common/app_common/`: the voice side (agent, audio, device state machine). `app_device_init()`
    registers the voice hooks on `app_ui`.
  - `examples/common/espressif2022__esp_emote_gfx/`: vendored esp_emote_gfx 2.1.0 used by both apps (it
    overrides the downloaded copy; our diff is in `patches/`). The package has two label implementations:
    the linker uses `src/widget/gfx_draw_label.c`, and `gfx_label.c` is dead code, so label fixes go in
    `gfx_draw_label.c`.
  - `examples/desk_keys/`: the Wi-Fi-only app. `main/wifi_setup.c` is the Wi-Fi manager and captive setup
    page; `components/dns_server/` is ESP-IDF's captive-portal DNS example, rewritten (see below).
  - `examples/voice_chat/`: the voice app.
  - `examples/voice_chat/bootloader_components/cyberai_power_hold/`: bootloader hook that keeps the board
    powered (`desk_keys/bootloader_components` links to it)
- `scripts/`: `idf55-env.sh` (ESP-IDF 5.5 environment), `flash_cyberai_jtag.sh`, `serial_capture.py`,
  `jtag_pinread.tcl` (live GPIO/I2S register snapshot)
- `patches/`: our changes to downloaded components, for reference
- `docs/`: official ESP-VoCat v1.2 schematics, ESP32-S3 module datasheet (the CyberAI product listing is
  not in git: it is the vendor's own material, kept in `backup/`)
- `research/`: notes and sources from the porting sessions (stock firmware analysis, xiaozhi board
  sources, probe scripts)
- `backup/` (not in git): full 16 MB dumps of the stock CyberAI flash, one per board (the second board,
  `..._b960_...`, came with a newer stock firmware; same partition table)
- `logs/` (not in git): build and flash logs

## Build

    source scripts/idf55-env.sh
    cd firmware/examples/desk_keys          # or voice_chat
    idf.py select-board --board cyberai_echoear   # once per app, and after editing the board YAML files
    idf.py build

- ESP-IDF 5.5 is installed at `~/esp/esp-idf`, with tools in `~/.espressif`. Keep the ESP-IDF 5.4 tools in
  `~/.espressif`: another project (`~/projects/empy/esp32/lvgl_micropython`) uses them.
- The root disk is nearly full. Check free space before large downloads.
- `sdkconfig` is gitignored and regenerated. Put settings that must last in the board's `sdkconfig.defaults`
  (both apps) or the app's own `sdkconfig.defaults`.
- Adding or removing a source file needs `idf.py reconfigure` before the build picks it up.
- Both apps use the same partition table, so switching between them only needs an app flash; NVS (Wi-Fi,
  settings) is kept.
- After changing `app_ui`, build both apps.

## Flashing: JTAG only

Any chip or system reset cuts the board's power, so esptool, web flashers and BOOT-button download
mode do not work. `flash_cyberai_jtag.sh` flashes over the built-in USB JTAG. Before each reset it
pad-holds the power latch (GPIO7) plus GPIO15/21, and those holds survive the reset.

    APP=desk_keys scripts/flash_cyberai_jtag.sh app   # the Wi-Fi-only app (APP defaults to voice_chat)
    scripts/flash_cyberai_jtag.sh app      # application only; keeps Wi-Fi and claiming (NVS)
    scripts/flash_cyberai_jtag.sh bootapp  # bootloader + application; keeps NVS
    scripts/flash_cyberai_jtag.sh assets   # emote UI assets only
    scripts/flash_cyberai_jtag.sh          # everything; erases NVS, so re-provision afterwards

- A first-try `ABORT: flash size` is normal; the script retries.
- Quick keys (`BOARD_ENABLE_QUICK_KEYS`): 8 s after boot the firmware turns the USB port into a keyboard
  (`303a:4005`, "CyberAI VoCat Quick Keys"), and USB-Serial-JTAG (`303a:1001`) disappears. The flash
  script first sends `vocat usb-release` to the keyboard's CDC port, which hands USB back to JTAG until the
  next boot. If that fails, hold a finger on the screen through boot: the keyboard then doesn't start.
- A stale `Guru Meditation ... (Cache error)` line at the start of a capture comes from the flash session's
  halt/reset, left in the USB-Serial-JTAG buffer. It isn't a crash of the running app.
- Never `reset run` a running app without halting it first: that causes a cache-error panic.
- End a JTAG session that halts the chip (a flash dump, a register read) with `reset run`, or the board
  stays powered and enumerated with the CPU stopped and the screen dark, looking switched off.
- Serial console: `$IDF_PYTHON_ENV_PATH/bin/python scripts/serial_capture.py out.bin 60`. The system
  Python has no pyserial, and the port may show up as `/dev/ttyACM1`. Once the keyboard starts, logs
  continue on its CDC port (the capture script follows the switch); lines logged while no host has that
  port open are dropped. Don't stop a capture with `pkill -f serial_capture.py` inside a longer command:
  it also kills that command.

## Settings, and switching off

- `app_settings.c` keeps the name shown on the clock page, which computer the Desktop keys are for, and
  the screen brightness in NVS (namespace `vocat`). Its setters only touch memory and hand the flash write
  to its own task, so the gfx task and the web handlers can call them.
- Wi-Fi credentials are not in there: the Wi-Fi driver saves those itself, and they survive reboots,
  switching off and an application flash. Only a full `flash_cyberai_jtag.sh` (no argument) erases them.
- desk_keys serves a settings page (name, computer, brightness, change network, turn off) at the board's
  address on the home network, and at `/settings` on the setup network. The address shows on the clock
  page for 30 s after it connects.
- Holding the POWER button (GPIO16, active low) for 2 s switches the board off: `app_power.c` releases the
  bootloader's hold on the power latch (GPIO7) and drives it low, which cuts power with or without USB.
  Switching on again needs the button.
- The Neurons page (`app_neurons.c`) is a drifting network of nodes; a tap makes them spell the name and
  then the Neurons texts from the settings. The dashboard opens on it (`PAGE_START`), or on the clock page
  while Wi-Fi setup runs. Its colour, texts and hold time are on the settings page.
- The clock page shows the battery level under the ticker (`app_battery.c`): it reads the BQ27220 fuel gauge
  at I2C 0x55 every 30 s, the same chip and registers as the stock firmware. It stays hidden while the gauge
  doesn't answer. Grey normally, amber at 20 % and below, red with "Charge me" at 10 % and below, blue with a
  bolt on USB.
- Holding the clock page for 10 s asks before forgetting the Wi-Fi network; a second hold within 20 s does
  it. One hold used to be enough, and picking the board up wiped the network.

## Wi-Fi setup (desk_keys)

- With no saved network, or after 3 failed connects, the board opens the open network
  `VoCat-Setup-XXXX` at 192.168.4.1 (DHCP option 114 and a catch-all DNS make it a captive portal). The
  clock page shows a join QR code and the address. The page lists 2.4 GHz networks only: the ESP32-S3
  has no 5 GHz radio.
- A network entered on the page is tried with RAM-only storage and saved only once it connects.
- Holding the clock page for 10 s, or `vocat wifi forget` on the CDC port, forgets the network and opens
  the setup network.
- To look at the setup page from this laptop, briefly join the setup network with `nmcli` and switch back
  to the home network in the same command (the laptop's only link is Wi-Fi, so this session is offline
  meanwhile). The user has been fine with this.

## Checking the screen without touching it

- Dev commands on the keyboard's CDC port (`/dev/serial/by-id/usb-CyberAI_VoCat_Quick_Keys_*-if00`, after
  `stty raw -echo`): `vocat page <n>` shows dashboard page n (0 clock, 1 Media, 2 Desktop, 3 Neurons;
  voice_chat also 4 Timers, 5 conversation); `vocat touch <0|1> <x> <y>` releases or presses at a screen
  point. Pressing a key and releasing somewhere else cancels it without sending anything to the computer.
- The same port sets things without the settings page (handy when this laptop is on another network):
  `vocat set name <text>`, `vocat set text <1-4> [text]`, `vocat set color <RRGGBB>`, `vocat set hold <s>`.
- When the user puts the board in front of it, the webcam at `/dev/video2` can see the screen. Auto
  exposure washes out the dark UI (it made the dark ring look light blue), so capture with fixed exposure:
  `v4l2-ctl -d /dev/video2 -c auto_exposure=1 -c exposure_time_absolute=250 -c gain=0`, then set
  `auto_exposure=3` again afterwards. The camera shares a USB hub with the board and can return an I/O
  error right after a flash; retry.
- Images drawn into the gfx buffer must be byte-swapped RGB565 (the display runs with `swap` on, and image
  pixels are copied as-is). Label colours are swapped by the library.
- gfx labels draw text from the top of their box: to centre a label on a point, make the box as tall as the
  font's `line_height`.

## Board facts

- ES8311 codec at I2C 0x18 plus ES7210 mic ADC at 0x40, on I2C SDA 2 / SCL 1. I2S uses MCLK 42,
  BCLK 40, WS 39, DOUT 41, DIN 38.
- The speaker amp enable is **GPIO4**, active high, switched by the ES8311 driver. Holding it
  statically high gives no sound.
- The ST77916 360x360 QSPI display: CLK 14, CS 10, D0-D3 13/9/12/46, backlight PWM on GPIO3.
  The CST816S touch controller is at 0x15 and is powered by GPIO21.
- voice_chat provisions over BLE with the ESP RainMaker Home app; desk_keys uses its setup network.

## Rules learned the hard way

- Never do NVS or flash work in the gfx task: the touch callback, gfx timers and `ui_tick` all run
  there. Its stack is in PSRAM, so a flash write asserts (`esp_task_stack_is_sane_cache_disabled`)
  and reboots. Queue the work to a normal task instead.
- Leave `BOARD_DISABLE_INTERACTIVE_CONSOLE` on. The agent_console REPL stalls the app, and so blanks
  the screen, while a USB host is attached but the port isn't being read.
- Don't trust an I2C probe sweep on this board: it reports phantom addresses.
- Full-screen animation: every visible gfx object under the changed area is redrawn for each 10-line chunk,
  and a label blends its whole mask pixel by pixel. The dashboard's full-screen backdrop label alone cost
  about 150 ms a frame under the Neurons image, so a page that covers the screen hides the backdrop and the
  emote face. The Neurons frames are drawn on core 1 (the gfx task is on core 0) into three images in turn.
- Web pages from `esp_http_server`: `httpd_resp_sendstr_chunk(req, "")` sends a zero-length chunk, which
  ends the whole response (use `send_str()` in `wifi_setup.c`). Captive-portal windows don't reliably run
  JavaScript, so the setup pages use plain links and forms.
- `esp_wifi_set_config()` skips the NVS write when the configuration hasn't changed, so trying a network
  with `WIFI_STORAGE_RAM` and writing the same values back with `WIFI_STORAGE_FLASH` saves nothing: the
  board connected but lost the network at the next power cycle. Write the network being tried, and put the
  previous one back if it fails.
- ESP-IDF's captive-portal DNS example tests header flags without `ntohs()`, so it ignores every query
  with the AD flag set, and it appends answers after EDNS records. `desk_keys/components/dns_server` has
  the fixed version; don't copy the example again.

## Known issues

- Nova Sonic ends conversations with `AUDIO_CONVERSATION_ERROR` ("Timed out waiting for audio
  bytes") and the WebSocket disconnects.
- `esp_gmf_rate_cvt` crashes (LoadProhibited) when two chimes start within about 0.7 s.
- The head capacitive touch pad's pin is unknown, so that input is disabled. POWER-button off
  behaviour is untested.
- Getting a DHCP address from the home router takes about 10 s after the Wi-Fi association.
