# mesh_power — Mains voltage monitoring over a MeshCore LoRa mesh

Detect **power outages at remote sites** using a MeshCore mesh node with a
ZMPT101B voltage transformer, and alert via the mesh → NOC → Nagios pipeline.
No internet required between the sensor and the NOC.

```
[SITE]                                      [NOC]
Heltec V3 + solar battery                    Pi (openHop Repeater + companion)
├─ ZMPT101B → ADC (measures 120V AC)         ├─ mesh_power.py daemon (receives,
├─ MeshCore firmware + power sensor          │  validates sender pubkey, writes .state)
└─ telemetry/alerts over encrypted RF ────►  ├─ /var/lib/mesh_power/<site>.state
   (periodic heartbeat + outage alert)       └─ Nagios → check_mesh_power
```

- **Instant outage alert**: voltage drops from >100V to <30V RMS → alert hits
  Nagios in under a minute (no waiting for the periodic cycle).
- **Periodic heartbeat**: the node reports voltage every 5 min (direct send —
  no ACK/Trigger dependency, reliable over marginal links).
- **Identity-based**: the daemon identifies the site by the sender's MeshCore
  **public key**, never by message text (spoof-proof).

## Components

| Path | What |
|---|---|
| `firmware/examples/mesh_power_sensor/` | ESP32 firmware (Heltec V3): ZMPT101B sampling, RMS, outage detection, periodic heartbeat, mesh repeater role |
| `daemon/mesh_power.py` | NOC daemon: connects to a MeshCore companion (TCP), validates pubkeys, writes per-site state, exposes HTTP :5053 |
| `daemon/install.sh` | systemd installer for the NOC daemon (venv + meshcore + unit) |
| `daemon/import_contact_noc.py` | One-time pairing helper: imports the sensor node into the companion's contact DB |
| `nagios/check_mesh_power` | Nagios plugin: OK/WARN/CRIT on voltage + state freshness |
| `nagios/mesh_power.cfg` | Nagios host/service definition (example) |
| `calibrate/` | ZMPT101B calibration sketch + node provisioning script |
| `DESIGN.md` | Full design notes (pins, BOM, decisions, pitfalls) |

## How it works

1. **Sensor node** (Heltec V3 + ZMPT101B, solar/battery powered — NOT by the
   mains it measures, so it survives the outage it reports). Samples the AC
   line (RMS over ~1s window at 2 kHz), publishes CayenneLPP telemetry and
   sends a `PWR x.x` text heartbeat every cycle to contacts with alert
   permission.
2. **On outage** (`<30V RMS`) it sends an immediate high-priority alert;
   recovery (`>50V`) sends another.
3. **NOC daemon** listens on the MeshCore companion socket, matches the
   sender's pubkey prefix against `config.json` sites, and writes
   `/var/lib/mesh_power/<site>.state` `{"v": 121.3, "ts": ...}`.
4. **Nagios** checks the state (locally or via the daemon's HTTP endpoint):
   CRIT on low voltage **or** stale state (node dead / no RF).

## Requirements

- **Sensor node**: [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/)
  (or compatible ESP32-S3), ZMPT101B module, 3× 18650 + 8W solar panel (BOM in DESIGN.md).
- **Firmware**: built with PlatformIO against the official
  [MeshCore firmware repo](https://github.com/meshcore-dev/MeshCore)
  (based on the `simple_sensor` example). Copy `firmware/examples/mesh_power_sensor/`
  into the MeshCore repo and `pio run -e heltec_v3`.
- **NOC**: Raspberry Pi running an [openHop Repeater](https://github.com/openhop-dev/openhop_repeater)
  with a companion, Python 3.10+, Nagios Core.

## Install (NOC daemon)

```bash
git clone <this repo> /opt/mesh_power_src
cd /opt/mesh_power_src
sudo bash daemon/install.sh            # venv + meshcore + systemd unit
sudo nano /etc/mesh_power/config.json  # companion port + sites[].pubkey
sudo systemctl start mesh_power
```

`config.example.json`:

```json
{
  "companion": { "host": "127.0.0.1", "port": 5052 },
  "sites": [
    { "name": "sitio1", "pubkey": "<pubkey-hex-64-del-nodo-sensor>" }
  ],
  "state_dir": "/var/lib/mesh_power",
  "watchdog": { "probe_interval": 30, "probe_timeout": 8, "reconnect_delay": 5 }
}
```

## Pairing node ↔ NOC (no RF discovery wait)

1. **On the node** (BLE/serial CLI): give the NOC's companion pubkey alert
   permission (role ADMIN=3 + alerts LOW=64 + HIGH=128 → `195`):
   ```
   setperm <noc-pubkey-prefix> 195
   ```
   ⚠️ `192` leaves the role at GUEST and the node will reject the contact.
2. **On the NOC**: insert the node's pubkey into the companion's contact DB
   (edit `import_contact_noc.py` and run it — it backs up the DB first).

## Nagios

```bash
sudo cp nagios/check_mesh_power /usr/local/nagios/libexec/
# define host ac_sitios + one service per site (see nagios/mesh_power.cfg)
```

Plugin usage: `check_mesh_power -s <site> | -f <file> | -u <url> [-c 80] [-w 100] [-a 900]`

## Calibration

1. Flash `calibrate/calibrate.ino`, note the raw RMS with the mains connected.
2. `scale = V_ref_multimeter / raw_rms` → `setScale(scale)` in `main.cpp`.
3. Outage threshold: `<30V RMS` (noise floor is ~0.005V → 12× margin).

## Pitfalls (learned the hard way)

- **ESP32-S3 flashing**: `firmware.bin` from `.pio/build` is NOT a merged image.
  Flash all three: `esptool write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin`.
  Flashing the app at 0x0 → crash loop ("SHA-256 comparison failed"). The
  SPIFFS (identity/config/contacts) survives app reflashes.
- **Periodic heartbeats**: do NOT use `alertIf`+Trigger for periodic reports —
  the trigger blocks ~20 min on missing ACK and skips cycles. Send directly
  each cycle instead; keep `alertIf` for high-priority outage alerts only.
- **openHop `lbt_enabled: false`** = single TX attempt, no retry (modem's
  receiving-guard can refuse with `ERR_CHANNEL_BUSY` under noisy RF). Enable
  LBT with a few attempts for reliable TX.
- **Node wiring**: use GPIO4 for the ZMPT101B OUT — GPIO1 is the Heltec V3
  battery-divider pin (corrupts both readings).

## License

MIT — see [LICENSE](LICENSE).
