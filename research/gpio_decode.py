import re, sys
r = {}
for line in open(sys.argv[1]):
    m = re.match(r'^(\w+):\s*((?:0x[0-9a-fA-F]+\s*)+)$', line.strip())
    if m: r[m.group(1)] = [int(x, 16) for x in m.group(2).split()]
if 'OUT' not in r: print("no register dump found"); sys.exit(1)
out = r['OUT'][0] | r['OUT1'][0] << 32; en = r['EN'][0] | r['EN1'][0] << 32; inp = r['IN'][0] | r['IN1'][0] << 32
known = {0:'BOOT (official)', 9:'POWER_CTRL (official, driven low)', 15:'PA_CTRL (official)', 47:'LCD_RST (official)', 48:'CODEC_PWR (official)', 19:'USB D-', 20:'USB D+'}
print(f"STRAP_REG = {r['STRAP'][0]:#x}")
print("pin  out_en  src         level  in  pulls      note")
for n in range(49):
    sel = r['OUTSEL'][n] & 0x1FF; mux = r['IOMUX'][n]
    wpu, wpd = (mux >> 8) & 1, (mux >> 7) & 1; func = (mux >> 12) & 7
    oe = (en >> n) & 1
    src = 'GPIO' if sel == 256 else f'periph#{sel}'
    if oe or n in known:
        print(f"{n:3d}  {oe:6d}  {src:10s}  {(out>>n)&1 if sel==256 else '-':>5}  {(inp>>n)&1:2d}  {'PU' if wpu else ''}{'PD' if wpd else '':<8}  f{func} {known.get(n,'')}")
rout, ren, rin = r['RTCOUT'][0] >> 10, r['RTCEN'][0] >> 10, r['RTCIN'][0] >> 10
print("RTC-GPIO outputs enabled:", [(n, (rout >> n) & 1) for n in range(22) if (ren >> n) & 1] or "none")
print(f"PAD_HOLD={r['PADHOLD'][0]:#x} DIG_PAD_HOLD={r['DIGPADHOLD'][0]:#x} DIG_ISO={r['DIGISO'][0]:#x}")
