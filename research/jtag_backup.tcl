init
if {[catch {reset init} e]} { echo "RESET FAILED: $e"; shutdown error }
echo "RESET_HALT_OK"
if {[catch {flash probe 0} e]} { echo "PROBE FAILED: $e"; shutdown error }
set size [dict get [lindex [flash list] 0] size]
echo "FLASH_SIZE: $size"
if {$size != 0x1000000 && $size != 0x2000000} { echo "ABORT: unexpected flash size"; shutdown error }
echo "== backup to ./echoear_cyberai_backup_20260914_0752.bin =="
if {[catch {flash read_bank 0 {./echoear_cyberai_backup_20260914_0752.bin}} e]} { echo "BACKUP FAILED: $e"; shutdown error }
echo "BACKUP OK"
shutdown
