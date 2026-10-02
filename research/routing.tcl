init
halt
set h [lindex [read_memory 0x600080D8 32 1] 0]
mww 0x600080D8 [expr {$h | 0x208090}]
reset run
sleep 12000
halt
echo "INSEL: [read_memory 0x60004154 32 256]"
echo "IOMUX: [read_memory 0x60009004 32 49]"
echo "PAD_HOLD: [read_memory 0x600080D8 32 1]"
resume
shutdown
