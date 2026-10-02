#!/bin/bash
# Flash the ESP Private Agents voice_chat build (cyberai_echoear board) over the ESP32-S3 USB JTAG.
# Download mode and esptool resets do not work on this board (any chip reset cuts the power),
# so every session: pad-holds GPIO7 (power latch), GPIO15 and GPIO21 high, does a system reset
# (holds survive it), then writes with OpenOCD's flash driver. Parts are written in separate
# sessions because OpenOCD has crashed on long transfers.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; V="$(cd "$HERE/.." && pwd)"
source "$HERE/idf55-env.sh"
# APP=desk_keys flashes the Wi-Fi-only desk app; the default is the voice app
APP="${APP:-voice_chat}"
B="$V/firmware/examples/$APP/build"
LOG="$V/logs/flash_cyberai_jtag.log"
HOLD_MASK=0x208080
: > $LOG

# The quick-keys firmware turns the USB port into a keyboard (303a:4005) 8 s after boot, which removes
# USB-Serial-JTAG (303a:1001). Ask it over its CDC port to hand the port back, then wait for JTAG.
release_usb() {
  lsusb | grep -qi 303a:1001 && return 0
  local tty
  for tty in /dev/serial/by-id/usb-CyberAI_VoCat_Quick_Keys_*-if00; do
    [ -e "$tty" ] || continue
    echo "asking the VoCat keyboard ($tty) to hand USB back to JTAG" | tee -a $LOG
    stty -F "$tty" raw -echo 2>/dev/null
    printf 'vocat usb-release\n' > "$tty"
  done
  for _ in $(seq 1 50); do
    lsusb | grep -qi 303a:1001 && return 0
    sleep 0.2
  done
  return 1
}

session() {  # $1 = label, rest = tcl lines run after reset+probe
  local label=$1; shift
  local tcl=$(mktemp --suffix=.tcl)
  {
    echo 'init'
    echo 'halt'
    echo 'set h [lindex [read_memory 0x600080D8 32 1] 0]'
    echo "mww 0x600080D8 [expr {\$h | $HOLD_MASK}]"
    echo 'if {[catch {reset init} e]} { echo "RESET FAILED: $e"; shutdown error }'
    echo 'if {[catch {flash probe 0} e]} { echo "PROBE FAILED: $e"; shutdown error }'
    echo 'if {[dict get [lindex [flash list] 0] size] != 0x1000000} { echo "ABORT: flash size"; shutdown error }'
    echo 'esp compression on'
    printf '%s\n' "$@"
    echo "echo \"SESSION_OK $label\""
    echo 'shutdown'
  } > $tcl
  for try in 1 2; do
    echo "$(date +%T) session $label (try $try)" | tee -a $LOG
    timeout 900 openocd -c "set ESP_RTOS none" -f board/esp32s3-builtin.cfg -f $tcl >> $LOG 2>&1
    if grep -q "SESSION_OK $label" $LOG; then rm -f $tcl; return 0; fi
    release_usb || { echo "board disappeared from USB" | tee -a $LOG; rm -f $tcl; return 1; }
  done
  rm -f $tcl; return 1
}

write_verify() {  # $1 = file, $2 = offset
  echo "if {[catch {flash write_image erase {$1} $2} e]} { echo \"WRITE FAILED $1: \$e\"; shutdown error }"
  echo "if {[catch {esp verify_bank_hash 0 {$1} $2} e]} { echo \"VERIFY FAILED $1: \$e\"; shutdown error }"
  echo "echo \"VERIFIED $(basename $1) @ $2\""
}

boot_firmware() {
  # Starting the app straight after a flash session leaves the display blank (something the
  # flash stub changes survives one reset), so reset into a halted state first, then run.
  echo "$(date +%T) booting firmware" | tee -a $LOG
  local keep="init; halt; set h [lindex [read_memory 0x600080D8 32 1] 0]; mww 0x600080D8 [expr {\$h | $HOLD_MASK}]"
  timeout 60 openocd -c "set ESP_FLASH_SIZE 0" -c "set ESP_RTOS none" -f board/esp32s3-builtin.cfg \
    -c "$keep; reset halt; shutdown" >> $LOG 2>&1
  timeout 60 openocd -c "set ESP_FLASH_SIZE 0" -c "set ESP_RTOS none" -f board/esp32s3-builtin.cfg \
    -c "$keep; reset run; shutdown" >> $LOG 2>&1
}

release_usb || { echo "board not on USB (no JTAG, and no VoCat keyboard answered)"; exit 1; }

# `flash_cyberai_jtag.sh app` rewrites only the application (ota_0), keeping NVS/claiming data
if [ "${1:-all}" = "app" ]; then
  session app "$(write_verify $B/$APP.bin 0x30000)" || { echo "FAILED in app"; exit 1; }
  grep -E "VERIFIED|FAILED|ABORT" $LOG
  boot_firmware
  echo "ALL_DONE"
  exit 0
fi

# `flash_cyberai_jtag.sh bootapp` rewrites the bootloader and the application, keeping NVS/claiming data
if [ "${1:-all}" = "bootapp" ]; then
  session bootapp \
    "$(write_verify $B/bootloader/bootloader.bin 0x0)" \
    "$(write_verify $B/$APP.bin 0x30000)" || { echo "FAILED in bootapp"; exit 1; }
  grep -E "VERIFIED|FAILED|ABORT" $LOG
  boot_firmware
  echo "ALL_DONE"
  exit 0
fi

# `flash_cyberai_jtag.sh assets` rewrites only the emote UI assets (anim_icon partition)
if [ "${1:-all}" = "assets" ]; then
  session assets "$(write_verify $B/asset_test.bin 0xd70000)" || { echo "FAILED in assets"; exit 1; }
  grep -E "VERIFIED|FAILED|ABORT" $LOG
  boot_firmware
  echo "ALL_DONE"
  exit 0
fi

# 1: clear old CyberAI metadata (nvs/otadata/phy at 0x9000-0x2FFFF overlaps the new
#    esp_secure_cert/nvs_key/fctry/nvs/otadata), then bootloader, partition table, otadata
session meta \
  'if {[catch {flash erase_address 0x9000 0x27000} e]} { echo "ERASE FAILED: $e"; shutdown error }' \
  'echo "ERASED 0x9000-0x2FFFF"' \
  "$(write_verify $B/bootloader/bootloader.bin 0x0)" \
  "$(write_verify $B/partition_table/partition-table.bin 0x8000)" \
  "$(write_verify $B/ota_data_initial.bin 0x1c000)" || { echo "FAILED in meta"; exit 1; }

# 2: application
session app "$(write_verify $B/$APP.bin 0x30000)" || { echo "FAILED in app"; exit 1; }

# 3: speech models (voice app only) and assets
models=""
[ -f $B/srmodels/srmodels.bin ] && models="$(write_verify $B/srmodels/srmodels.bin 0xc30000)"
session data \
  "$models" \
  "$(write_verify $B/asset_test.bin 0xd70000)" || { echo "FAILED in data"; exit 1; }

grep -E "ERASED|VERIFIED|FAILED|ABORT" $LOG

# 4: boot the new firmware (pads stay held through the reset; the new bootloader re-holds them)
boot_firmware
echo "ALL_DONE"
