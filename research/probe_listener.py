import serial, glob, time
end = time.time() + 300
out = "./scratch/probe_console.bin"
while time.time() < end:
    try:
        s = serial.Serial(sorted(glob.glob('/dev/ttyACM*'))[-1], 115200, timeout=0.2)
        with open(out, "ab") as f:
            while time.time() < end:
                d = s.read(8192)
                if d: f.write(d); f.flush()
    except Exception:
        time.sleep(1)
