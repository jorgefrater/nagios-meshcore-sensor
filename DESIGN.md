# mesh_power — Sensor de voltaje de red por la malla MeshCore

Repetidor MeshCore (Heltec V3) + ZMPT101B → detección de cortes de energía
en sitios remotos → alertas por la malla → Nagios en el NOC.

## Arquitectura (acordada 2026-08-15)

```
[SITIO REMOTO N]                              [NOC]
Heltec V3 + batería solar                      Pi 4 pyMC-NOC (red interna)
├─ ZMPT101B → ADC (mide red 120V AC)           ├─ openHop Repeater + companion
├─ firmware MeshCore simple_sensor + ADC       ├─ daemon mesh_power.py (recibe,
└─ telemetría/alertas por RF cifrada ────────►  │  valida pubkey, escribe .state)
   (advert sensor periódico + alerta en corte)  ├─ /var/lib/mesh_power/<sitio>.state
                                               └─ Nagios Core → check_mesh_power
```

- El nodo remoto es **repetidor de malla + sensor** (decisión del usuario).
- El nodo se alimenta por batería + panel solar, **NO por la red que mide**:
  cuando se va la luz, el nodo sigue vivo y es quien reporta el corte.
- **Alerta de corte inmediata** (V pasa de >100V a <30V RMS) sin esperar el
  ciclo periódico → aviso a Nagios en <1 min.

## Hardware / BOM (cerrado 2026-08-15)

| Pieza | Nota |
|---|---|
| Heltec WiFi LoRa 32 V3 (AYWHP clon, preset `heltec_v3`) | 3-pack |
| ZMPT101B | transformador de voltaje aislado 0–250V AC |
| Panel solar 8W (USB 5V) | sobredimensionado a propósito (época lluviosa) |
| 3× 18650 button-top protegidas 3500mAh (holder con resorte) | emparejadas |
| Fusible 1A en línea + pigtail JST | |
| Antena 915MHz 5.8dBi + pigtail N→SMA | |
| Caja IP65+ | |

## Pines Heltec V3 → ZMPT101B

**Conexión (3 cables):**

| ZMPT101B | Heltec V3 | Nota |
|---|---|---|
| VCC | pin **3V3** | alimentar a 3.3V → salida centrada ~1.65V (dentro del ADC) |
| GND | **GND** | común |
| OUT | **GPIO4** (ADC1_CH3) | señal AC centrada en 1.65V |

**⚠️ No usar GPIO1**: en el Heltec V3 está conectado internamente al divisor
de batería del JST (lo usa el firmware para medir el pack). Usarlo para el
sensor corrompe ambas lecturas.

Pines ADC1 libres en el V3 (verificados contra `boards/heltec_v3.h` del
firmware): **GPIO2, 3, 4, 5, 6, 7**. Elegido **GPIO4** (ADC1_CH3).
Ocupados por el board: GPIO0 (botón PRG), GPIO1 (batería), GPIO8/12/13/14
(LoRa SX1262: NSS/RST/BUSY/DIO1), GPIO9/10/11 (SPI SCK/MISO/MOSI),
GPIO17/18 (I2C OLED), GPIO19/20 (USB nativo), GPIO21 (reset OLED),
GPIO36 (VEXT enable), GPIO43/44 (UART0→CP2102).

### Alimentación del módulo

- **Opción A (recomendada): VCC = 3.3V.** Salida centrada en 1.65V ± pico.
  Con red 120V y el potenciómetro a media ganancia el pico queda en el rango
  0.2–2.9V, dentro del ADC (atenuación 11dB, rango útil 0–3.1V).
- **Opción B: VCC = 5V + divisor 2:1 en OUT** (2×10kΩ) si a 3.3V el módulo
  no da amplitud suficiente. El V3 expone 5V en el header.

### Medición (RMS por muestreo)

- ~1000–2000 muestras con `analogReadMilliVolts` a ~2kHz (intervalo 500µs,
  ≈1s de ventana = 60 ciclos de 60Hz).
- RMS = sqrt(media((v_i − v_media)²)); el offset (centro ~1.65V) se cancela.
- Presencia de red: umbral de corte **<30V RMS** (diseño acordado); volver a
  estado normal con histéresis (p.ej. >50V) para evitar flapping.
- ⚠️ Seguridad: el módulo aísla (transformador), pero el lado de red va en
  bornas dentro de la caja con **fusible 0.5A**, conectado post-breaker del
  sitio. Nada de electrónica DIY tocando la fase.

### Calibración

1. Sketch `firmware/examples/calibrate/calibrate.ino` imprime el RMS raw.
2. Con la red desconectada: raw ≈ 0 (offset cancelado).
3. Con multímetro de referencia en el tomacorriente (p.ej. 121.3V):
   raw_rms_X → scale = V_ref / raw_rms_X. `setScale(scale)`.
4. El potenciómetro del módulo se ajusta para buena amplitud sin recorte y
   no se toca más.

**Calibración verificada 30-ago-2026 (nodo de banco, Heltec V3.2 + ZMPT101B):**
- scale = **569.2** (122.5V multímetro / 0.2152 raw_rms), potenciómetro sin tocar
- Lectura en vivo: **122.7V** vs multímetro 122.5V (±0.3V, estable)
- Corte: 122.5V → 0V al desconectar; retorno: 0V → 122.7V al reconectar
- Ruido sin red: ≤0.005V raw (~2.5V) → margen 12x contra el umbral de 30V
- El scale está compilado en el firmware del nodo (`main.cpp`, `setScale(569.2f)`)
  y en el `calibrate.ino`

## Firmware del nodo (el "firmware especial")

Base: **example `simple_sensor` del repo oficial MeshCore**
(`meshcore-dev/MeshCore`, v1.17.1) — ya trae:

- Telemetría **CayenneLPP** por canales (`LPP_VOLTAGE`, `LPP_CURRENT`,
  `LPP_POWER`, ...) expuesta en adverts de tipo sensor (ADV_TYPE_SENSOR).
- **Alertas** con re-intentos y acks (`alertIf` + `sendAlert` a peers con
  permiso `PERM_RECV_ALERTS_LO/HI`).
- Series de tiempo (TimeSeriesData) y comandos personalizados.
- Identidad con pubkey (validación del remitente en recepción).

Cambios del fork (`mesh_power/firmware/`):

1. **`zmpt101b` lib** (nueva): muestreo ADC → RMS + calibración + presencia
   de red (ver `firmware/lib/zmpt101b/`).
2. `onSensorDataRead()`: leer ZMPT101B → publicar en telemetría (voltaje de
   red) + registrar serie temporal.
3. Alerta inmediata en corte (`alertIf(v < 30V, ..., HIGH_PRI_ALERT, "PWR OUT")`)
   y heartbeat periódico de voltaje.
4. `allowPacketForward()` → activar forwarding para que el nodo **repita
   malla** además de sensar (requisito del usuario).
5. Config de radio de la malla tica: **910.525 MHz / SF11 / BW125 / CR4/5 /
   preámbulo 32 / TX 22 dBm** (igual que los nodos existentes) — vía
   configuración del nodo (BLE/serial) al desplegar.

Compilar: PlatformIO, `pio run -e heltec_v3` (board JSON ya incluido en el
repo MeshCore).

## Lado NOC (receptor)

- **Companion** dedicado en el Pi NOC (patrón HA bridge; el companion del NOC
  en TCP 5050 existe como referencia).
- **Daemon `mesh_power.py`** (daemon/): recibe telemetría/alertas del nodo,
  identifica el sitio por **pubkey del remitente** (no por el texto), escribe
  `/var/lib/mesh_power/<sitio>.state` con valor + timestamp.
- **Plugin Nagios `check_mesh_power`** (nagios/):
  - `CRIT` si V < 80V (corte) — o si el estado tiene > 15 min de antigüedad
    (nodo muerto; alerta distinta).
  - `WARN` configurable. Service por sitio, check cada 2–5 min.
- Airtime: SF11/BW125 ≈ 2.5–3s/paquete; fases escalonadas por sitio
  (1 mensaje cada ~30s con 10 sitios) → canal tranquilo.

## Consumo medido en campo (11-sep-2026, nodo MC_barranca_fratec)

Serie real medida por telemetría remota (vía companion de Escazú, cada 2 h):

| Hora | Batería | Δ |
|---|---|---|
| 18:51 | 4104 mV (93 %) | base, aún con sol |
| 21:16 | 4087 mV | −17 mV (−7.0 mV/h) |
| 23:17 · 01:18 · 03:19 | 4087 mV | 0 (meseta de la curva Li-ion) |
| 05:20 | 4069 mV | −18 mV (−8.9 mV/h) |
| 09:22 | 4122 mV (95 %) | **+53 mV con sol (+13.1 mV/h)** |

- **Noche completa (10.5 h): −35 mV ≈ −2.8 % de carga ≈ 0.21 Ah → ~20 mA promedio**
  con `powersaving on` (vs 30-45 mA estimado por datasheet → el ahorro rinde muy bien).
- **Recarga**: el panel de 8 W repone la noche (−35 mV) en **~2.7 h de sol**.
- **Autonomía** con 3×18650 de 2500 mAh: **~29 noches sin sol** (80 % utilizable).
- **Uptime continuo 19.4 h** — el nodo no se reinició en toda la noche (con las baterías
  viejas de 990 mAh se apagaba): alimentación estable.
- La meseta de 4087 mV durante 6 h confirma que el voltaje es un indicador pobre en la
  zona plana de la curva Li-ion: el consumo se lee en las caídas de los extremos.
- Recomendación: 3 celdas sanas de 2500-3500 mAh + panel 8 W + `powersaving on` es un
  diseño sobrado para 24×7 (incluso en época lluviosa).

## Hitos

- [x] Diseño y BOM (15-ago-2026)
- [x] Kickoff: estructura del proyecto + DESIGN.md + lib zmpt101b + check_nagios
- [x] Fork firmware: example `mesh_power_sensor` (base simple_sensor + zmpt101b) + env
      `Heltec_v3_mesh_power` — compilado y verificado (ago-2026)
- [x] Daemon mesh_power.py completo (patrón ha_bridge, companion TCP, whitelist pubkey)
      — parser probado 7/7, conexión validada contra companion 5050 del NOC
- [x] API del repeater documentada: login POST /auth/login (NO /api/auth/login)
- [x] Despliegue: companion mesh_power (5052) creado + daemon instalado y activo
      en el NOC (config con 1 sitio dado de alta)
- [x] Nodo de banco armado y calibrado: Heltec V3.2 + ZMPT101B, scale 569.2,
      verificado en vivo (122.7V vs multímetro 122.5V, corte y retorno OK)
- [x] Nodo flasheado con firmware mesh_power y CONFIGURADO:
      nombre "PWR Sensor <sitio>" · radio 910.525/SF11/BW125/CR5 · TX 22 dBm ·
      pubkey 58C132E4…EFA3C (identidad única del nodo) ·
      ubicación lat/lon del despliegue · sensor responde `pwr` → V=123.x
- [x] Emparejamiento manual NODO↔NOC (sin esperar descubrimiento RF):
      - nodo: `setperm d3c9b135…4f71 195` (rol ADMIN=3 + alertas LOW=64 + HIGH=128;
        ¡con 192 el rol queda GUEST y rechaza!) — crea contacto + secreto ECDH
      - NOC: INSERT en companion_contacts (0xd3) con la pubkey del nodo
        (script daemon/import_contact_noc.py) — el companion deriva el secreto
      - DEMOSTRADO: el nodo real mandó "PWR 123.0" → malla → NOC → companion →
   daemon (llegó como CONTACT_MSG_RECV con pubkey del remitente)
- [x] Fixes del daemon: watchdog probe (get_contacts cada 30s, reconecta si el
      repeater se reinicia) + matcheo de sitio por PREFIJO de pubkey
      case-insensitive (el evento trae minúsculas, el config mayúsculas)
- [x] 🔧 ANTENA YAGI (31-ago): enlace casa→NOC pasó de MARGINAL a SÓLIDO.
      Medición post-Yagi (API del repeater): señal directa del nodo -95/-98 dBm
      con SNR +6.2 y score 1.0 (antes -101/-126 sin SNR) · noise floor -114
      (antes -112) · heartbeats cada 5 min estables · TX del NOC con LBT
      activo (transmitted:1, lbt_attempts:2, channel_busy:1 → transmitió
      igual, "transmitting anyway"). tx_errors companion: 0.
      Los paquetes a -124/-127 son copias relayed del flood por otros nodos.
- [x] 📡 API del repeater (openHop): el login POST /auth/login requiere
      `client_id` además de username/password ("Missing required fields").
      Endpoints útiles: /api/noise_floor_stats, /api/noise_floor_history,
      /api/recent_packets (campos: src_hash, rssi, snr, score, lbt_attempts,
      lbt_channel_busy, transmitted, drop_reason), /api/companion/stats.
- [x] 🔧 FIX CRÍTICO (30-ago): el NOC NO transmitía — modem respondía
      `ERR_CHANNEL_BUSY 0x0E` / "TX failed — no TX_DONE" (receiving guard:
      el SX1262 en PREAMBLE_DETECTED por el noise alto -112 rechaza TX).
      Con `lbt_enabled: false` el openHop hacía 1 SOLO intento sin reintento.
      Fix: `lbt_enabled: true` + `lbt_max_attempts: 10` (CAD + backoff + 
      "transmitting anyway") → TX failed = 0 y Escazú ya ve los adverts del NOC
- [x] 🔧 FIX REPORTE PERIÓDICO (30-ago): el heartbeat con alertIf+Trigger se
      BLOQUEABA ~20 min si el ACK del NOC no llegaba (enlace marginal) →
      reemplazado por envío DIRECTO por ciclo (sendPeriodicMessage: 1 TXT
      "PWR x.x" cada SENSOR_READ_INTERVAL_SECS a todos los contactos con
      permiso de alertas, sin Trigger/ACK). VERIFICADO: STATE cada 5:13 min
      (19:47, 19:52). El corte PWR OUT/BACK sigue con alertIf de alta prioridad.
- [x] ⚠️ FLASHEO ESP32-S3: ¡el firmware.bin de .pio/build NO es merged!
      Flashear las 3 imágenes: write_flash 0x0 bootloader.bin 0x8000
      partitions.bin 0x10000 firmware.bin (flashear la app en 0x0 = crash
      loop "SHA-256 comparison failed" + WDT resets). El merge-bin.py genera
      firmware-merged.bin solo en algunos builds; el método 3-archivos es fiable.
- [x] 📱 NOTIFICACIÓN TELEGRAM (30-ago, cambio del usuario): host ac_sitios y
      service "mesh_power Barranca" con `_SENDTELEGRAM 1` y contact_groups
      ac-admins1 → las alertas PROBLEM/RECOVERY llegan por Telegram además
      del email (bot del servidor Nagios via /usr/local/nagios/sbin/telegram-notify/
      telegram-bot.php; chat_id del contacto en contacts.cfg). El alias del host
      pasó a "AC Sitios". Sincronizado en nagios/mesh_power.cfg (workspace).
      Nota: el grupo ac-admins1 es el de clientes telecom del template
      ac-host-telecom (templates.cfg) — no confundir con 'admins'.
- [x] 📡 Repetidor MC_barranca_fratec (31-ago): Heltec V3 nuevo con env
      `Heltec_v3_repeater` (example simple_repeater del repo MeshCore).
      Configurado por CLI serial: radio 910.525,125,11,5 · TX 22 dBm ·
      nombre MC_barranca_fratec · lat/lon 10.007820,-84.702200 (Barranca) ·
      advert.interval 60 · flood.advert.interval 3. Pubkey/ID:
      CC49ABA8F45A4FB350F70213BB5CBB195C006BF4910136CDEA7B8F0CD65E963E.
      Script reproducible: calibrate/configure_repeater_barranca.py.
      Pendiente: instalar en el sitio (le dará malla al sensor PWR Barranca).
- [x] 🔧 FIX WATCHDOG (9-sep): tras reiniciar el repeater (upgrade), el daemon
      quedó SORDO 48 min — el probe `get_contacts()` NO lanza excepción con el
      socket medio-muerto (asyncio logueaba "socket.send() raised exception"
      pero el probe daba por buena la conexión) → Nagios "SIN REPORTE 45 min".
      Reemplazado por watchdog de 3 capas: (1) `mc.is_connected()` (property)
      detecta el socket muerto, (2) probe activo (get_contacts con timeout),
      (3) watchdog de silencio — si no llega ningún mensaje en
      `watchdog.silence_reconnect_secs` (default 720 = 2 ciclos de 5 min)
      recicla el enlace. Además se suscribe a `EventType.DISCONNECTED`.
      VERIFICADO en vivo: reinicio del repeater → detectado en 12 s,
      reconectado en 17 s (antes: intervención manual).
      El contacto del sensor y los datos NUNCA se perdieron: al reconectar, el
      daemon drenó los mensajes en cola del companion con valores frescos.
- [ ] Verificación de preámbulo RF: firmware nativo usa preamble 16 (SF11);
      openHop NOC usa 32 — si el enlace nativo→openHop no funciona, alinear
      (bajar el openHop a 16 o parchear preambleLengthForSF)
- [x] Nagios (servidor central): plugin check_mesh_power instalado + host `ac_sitios` + service
      `mesh_power Barranca` (URL /state/sitio1) — config validada (0 errores) y
      recargada. Pipeline END-TO-END probado con estado simulado: OK 121.5V /
      CRITICAL corte 0V. El daemon del NOC expone HTTP :5053 (GET /state/<sitio>).
- [ ] Despliegue piloto sitio 1 + verificación Nagios
