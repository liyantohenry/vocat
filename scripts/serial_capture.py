import serial, glob, sys, time
end = time.time() + float(sys.argv[2])
while time.time() < end:
    try:
        s = serial.Serial(sorted(glob.glob('/dev/ttyACM*'))[-1], 115200, timeout=0.2)
        with open(sys.argv[1], "ab") as f:
            while time.time() < end:
                d = s.read(8192)
                if d: f.write(d); f.flush()
    except Exception:
        time.sleep(0.5)
