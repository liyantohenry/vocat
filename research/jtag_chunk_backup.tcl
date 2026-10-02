init
halt
set h [lindex [read_memory 0x600080D8 32 1] 0]
echo "PAD_HOLD before: $h"
mww 0x600080D8 [expr {$h | 0x208090}]
if {[catch {reset init} e]} { echo "RESET FAILED: $e"; shutdown error }
echo "RESET_OK"
if {[catch {flash probe 0} e]} { echo "PROBE FAILED: $e"; shutdown error }
if {[dict get [lindex [flash list] 0] size] != 0x1000000} { echo "ABORT: size"; shutdown error }
for {set i 0} {$i < 16} {incr i} {
  set f [format "./scratch/bk/bk_%02d.bin" $i]
  if {[catch {flash read_bank 0 $f [expr {$i * 0x100000}] 0x100000} e]} { echo "CHUNK $i FAILED: $e"; shutdown error }
  echo "CHUNK $i OK"
}
echo "BACKUP_CHUNKS_OK"
reset run
sleep 12000
halt
echo "OUT: [read_memory 0x60004004 32 1]"
echo "OUT1: [read_memory 0x60004010 32 1]"
echo "EN: [read_memory 0x60004020 32 1]"
echo "EN1: [read_memory 0x6000402C 32 1]"
echo "IN: [read_memory 0x6000403C 32 1]"
echo "IN1: [read_memory 0x60004040 32 1]"
echo "OUTSEL: [read_memory 0x60004554 32 49]"
echo "INSEL: [read_memory 0x60004154 32 256]"
echo "IOMUX: [read_memory 0x60009004 32 49]"
echo "PAD_HOLD: [read_memory 0x600080D8 32 1]"
resume
shutdown
