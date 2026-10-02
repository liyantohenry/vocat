init
if {[catch {reset init} e]} { echo "RESET FAILED: $e"; shutdown error }
echo "RESET_HALT_OK"
flash probe 0
set b0 [lindex [flash list] 0]
echo "BANK0 size: [dict get $b0 size]"
if {[dict get $b0 size] != 0x1000000} { echo "ABORT: flash size is not 16MB"; shutdown error }
echo "== backup to ./echoear_backup_20260913_2303.bin =="
if {[catch {flash read_bank 0 {./echoear_backup_20260913_2303.bin}} e]} { echo "BACKUP FAILED: $e"; shutdown error }
echo "BACKUP OK"
esp compression on
echo "== writing firmware =="
if {[catch {flash write_image erase {./scratch/speaker_rainmakerai_0_12_6_ctm_esp_vocat_1_2.bin} 0x0} e]} { echo "WRITE FAILED: $e"; shutdown error }
echo "WRITE OK"
if {[catch {esp verify_bank_hash 0 {./scratch/speaker_rainmakerai_0_12_6_ctm_esp_vocat_1_2.bin} 0x0} e]} { echo "VERIFY FAILED: $e"; shutdown error }
echo "VERIFY OK"
echo "== resetting into new firmware =="
reset run
shutdown
