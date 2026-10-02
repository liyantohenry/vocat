init
halt
set h [lindex [read_memory 0x600080D8 32 1] 0]
mww 0x600080D8 [expr {$h | 0x208090}]
if {[catch {reset init} e]} { echo "RESET FAILED: $e"; shutdown error }
if {[catch {flash probe 0} e]} { echo "PROBE FAILED: $e"; shutdown error }
for {set i 10} {$i <= 12} {incr i} {
  set f [format "./scratch/bk/bk_%02d.bin" $i]
  if {[catch {flash read_bank 0 $f [expr {$i * 0x100000}] 0x100000} e]} { echo "CHUNK $i FAILED: $e"; shutdown error }
  echo "CHUNK $i OK"
}
shutdown
