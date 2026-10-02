init
halt
proc rd {name addr {n 1}} { echo "$name [format 0x%08x $addr]: [read_memory $addr 32 $n]" }
rd GPIO_OUT     0x60004004
rd GPIO_OUT1    0x60004010
rd GPIO_ENABLE  0x60004020
rd GPIO_ENABLE1 0x6000402C
rd GPIO_IN      0x6000403C
rd GPIO_IN1     0x60004040
foreach p {4 7 15 21 38 39 40 41 42} { rd "OUT_SEL$p" [expr {0x60004554 + 4*$p}] }
foreach p {4 7 15 21 38 39 40 41 42} { rd "IO_MUX$p"  [expr {0x60009004 + 4*$p}] }
rd PAD_HOLD     0x600080D8
rd I2S0_BLOCK   0x6000F000 48
rd RTCIO_BLOCK  0x60008400 64
resume
echo PINREAD_OK
shutdown
