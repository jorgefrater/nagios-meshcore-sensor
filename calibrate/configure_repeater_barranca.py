#!/usr/bin/env python3
"""Configura el Heltec V3 como repetidor MeshCore MC_barranca_fratec (Barranca)."""
import serial, time

PORT, BAUD = '/dev/ttyUSB0', 115200

s = serial.Serial(PORT, BAUD, timeout=0.5)
s.dtr = False
s.rts = True          # reset activo (EN bajo)
time.sleep(0.15)
s.rts = False         # release -> arranca la app
time.sleep(2.5)

def read_avail(seconds=1.0):
    end = time.time() + seconds
    out = b''
    while time.time() < end:
        d = s.read(512)
        if d:
            out += d
    return out.decode(errors='replace')

print("=== BOOT ===")
print(read_avail(4).strip())

cmds = [
    "set radio 910.525,125,11,5",   # malla tica: 910.525 / BW125 / SF11 / CR5
    "set tx 22",                    # 22 dBm
    "set name MC_barranca_fratec",
    "set lat 10.007820",            # Barranca
    "set lon -84.702200",           # Barranca
    "set advert.interval 60",       # advert local (rango 60-240 min)
    "set flood.advert.interval 3",  # flood advert cada 3h (sin esto NO se anuncia)
    "get radio",
    "get name",
    "get public.key",
]
for c in cmds:
    s.write((c + "\r").encode())   # el CLI de MeshCore usa CR como terminador
    time.sleep(1.2)
    r = read_avail(1.0).strip()
    print(f"--- {c}\n{r}")
s.close()
