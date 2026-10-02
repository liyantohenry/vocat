init
halt
echo "OUT: [read_memory 0x60004004 32 1]"
echo "OUT1: [read_memory 0x60004010 32 1]"
echo "EN: [read_memory 0x60004020 32 1]"
echo "EN1: [read_memory 0x6000402C 32 1]"
echo "STRAP: [read_memory 0x60004038 32 1]"
echo "IN: [read_memory 0x6000403C 32 1]"
echo "IN1: [read_memory 0x60004040 32 1]"
echo "OUTSEL: [read_memory 0x60004554 32 49]"
echo "IOMUX: [read_memory 0x60009004 32 49]"
echo "RTCOUT: [read_memory 0x60008400 32 1]"
echo "RTCEN: [read_memory 0x6000840C 32 1]"
echo "RTCIN: [read_memory 0x60008424 32 1]"
echo "PADHOLD: [read_memory 0x600080D8 32 1]"
echo "DIGPADHOLD: [read_memory 0x600080DC 32 1]"
echo "DIGISO: [read_memory 0x60008094 32 1]"
resume
shutdown
