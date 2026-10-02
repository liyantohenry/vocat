# VoCat

Espressif's [ESP Private Agents firmware](https://github.com/espressif/esp-agents-firmware) ported to a
CyberAI "EchoEar Pro": a Wi-Fi-only ESP32-S3 clone of Espressif's ESP-VoCat/EchoEar, sold by the Taobao
shop 赛博智能 (cyberailife.com), with its own pinout and power circuit. On top of the port the round
360x360 screen becomes a desk device: a clock page, and "quick keys" that send media and desktop keys to
the computer over USB.

Two applications share the same UI:

| | `desk_keys` | `voice_chat` |
|---|---|---|
| Dashboard, quick keys | yes | yes |
| Voice agent | no | yes |
| Setting up | the board's own Wi-Fi network and a captive page | ESP RainMaker app, BLE provisioning |
| Account needed | no | yes |

`desk_keys` exists because the voice agent needs a RainMaker login; without it the board is just a Wi-Fi
device you set up from a phone.

## The board

It looks like an EchoEar and shares the ES8311 + ES7210 audio path and the ST77916 round display, but the
pinout is the clone's own, worked out from the stock firmware and by probing:

| | |
|---|---|
| Display | ST77916 360x360 QSPI — CLK 14, CS 10, D0-D3 13/9/12/46, backlight PWM 3 |
| Touch | CST816S at I2C 0x15, powered by GPIO21 |
| Audio | ES8311 at 0x18, ES7210 at 0x40, I2C SDA 2 / SCL 1; I2S MCLK 42, BCLK 40, WS 39, DOUT 41, DIN 38 |
| Speaker amp | GPIO4, active high, switched by the codec driver |
| Power latch | GPIO7, held high by a bootloader hook |
| POWER button | GPIO16, active low |
| Radio | 2.4 GHz only (ESP32-S3), no Ethernet, no cellular |

The power circuit is the thing to know about: **any chip or system reset cuts the board's power**, so
esptool, web flashers and BOOT-button download mode all fail halfway. A bootloader hook drives the power
latch and pad-holds it so the hold survives a reset, and flashing goes over the built-in USB JTAG:

    APP=desk_keys scripts/flash_cyberai_jtag.sh app

Holding POWER for 2 s releases that latch and switches the board off, with or without USB attached.

## Building

    source scripts/idf55-env.sh                     # ESP-IDF 5.5
    cd firmware/examples/desk_keys                  # or voice_chat
    idf.py select-board --board cyberai_echoear
    idf.py build

## Setting up Wi-Fi (`desk_keys`)

With no saved network the board opens an open network `VoCat-Setup-XXXX` and shows a join QR code; the
captive page at 192.168.4.1 lists what it can see and asks for a password. Once it is on the home network
the same server offers a settings page — the name shown on the clock page, which computer the desktop keys
are for, screen brightness, changing network, switching off — at the board's address, which the clock page
shows for 30 s after it connects.

## What works

Display, touch, audio in and out, the dashboard, the quick keys as a USB keyboard (`303a:4005`), Wi-Fi
setup and settings that survive a power cycle, and switching off. Known gaps and the voice agent's own
problems are listed at the end of [CLAUDE.md](CLAUDE.md).

## Layout

    firmware/       esp-agents-firmware (upstream c5d6798, 2026-05-25) with the port on top
      examples/common/boards/cyberai_echoear/   this board: pins, devices, feature flags
      examples/common/app_ui/                   screen, dashboard, quick keys, USB keyboard, settings, power
      examples/desk_keys/                       the Wi-Fi-only app
    scripts/        ESP-IDF environment, the JTAG flash script, serial capture, JTAG pin reads
    patches/        our changes to downloaded components, for reference
    docs/           ESP-VoCat schematics and the ESP32-S3 module datasheet
    research/       notes and sources from the porting sessions

[CLAUDE.md](CLAUDE.md) is the working notes: build and flash details, how to inspect the screen, and the
things that cost a session to find out.

## Credit and licence

The firmware is Espressif's [esp-agents-firmware](https://github.com/espressif/esp-agents-firmware),
imported at commit c5d6798 (2026-05-25) and Apache-2.0 as stated in its source headers; the port keeps
that licence. Diff `firmware/` against that commit to see exactly what this port changes.
`docs/` holds Espressif's published ESP-VoCat v1.2 schematics and the module datasheet, for reference. The
board itself is CyberAI's "EchoEar Pro", sold through their Taobao shop 赛博智能 (cyberailife.com); their
product material is not reproduced here. Nothing here is endorsed by Espressif or by CyberAI.
