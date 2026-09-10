#!/usr/bin/env python3
"""
mesh_power daemon — receptor de voltaje de red vía malla MeshCore (NOC).

Se conecta al companion TCP del openHop Repeater del NOC y escucha los
mensajes directos de los nodos sensor remotos:

    "PWR 121.3"   heartbeat periódico (cada ~5 min) con el voltaje RMS
    "PWR OUT"     alerta inmediata de corte de red (alta prioridad)
    "PWR BACK"    red restablecida

Identifica el sitio por la PUBKEY completa del remitente (whitelist en
config) — no por el texto. Escribe /var/lib/mesh_power/<sitio>.state:

    {"v": 121.3, "ts": 1699999999, "node": "<pubkey hex>"}

El plugin Nagios check_mesh_power lee ese estado: CRIT si v < 80V (corte)
o si el estado tiene más de 15 min de antigüedad (nodo muerto / sin RF).

Config: JSON — env CONFIG_FILE o --config (default /etc/mesh_power/config.json)
"""
import argparse
import asyncio
import http.server
import json
import logging
import os
import re
import sys
import threading
import time

from meshcore import MeshCore
from meshcore.events import EventType

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("mesh_power")

DEFAULT_CONFIG = "/etc/mesh_power/config.json"
DEFAULT_STATE_DIR = "/var/lib/mesh_power"

DEFAULTS = {
    "companion": {"host": "127.0.0.1", "port": 5052},
    "sites": [],                       # [{"name": "sitio1", "pubkey": "<64-hex>"}, ...]
    "state_dir": DEFAULT_STATE_DIR,
    "http_port": 5053,                 # 0 = deshabilitado (Nagios remoto consulta por HTTP)
    "watchdog": {"probe_interval": 30, "probe_timeout": 8, "reconnect_delay": 5,
                 "silence_reconnect_secs": 720},   # 0 = desactivado; 720 = 2 ciclos de 5 min
}

RE_PWR_VALUE = re.compile(r"^PWR\s+(\d+(?:\.\d+)?)$", re.IGNORECASE)
PWR_OUT = "PWR OUT"
PWR_BACK = "PWR BACK"


def deep_merge(base, override):
    out = dict(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load_config(path):
    with open(path) as f:
        raw = json.load(f)
    cfg = deep_merge(DEFAULTS, raw)
    cfg["_path"] = path
    return cfg


def parse_pwr(text):
    """Devuelve ('value', float) | ('out', None) | ('back', None) | None."""
    t = text.strip()
    if not t:
        return None
    if t.upper() == PWR_OUT:
        return ("out", None)
    if t.upper() == PWR_BACK:
        return ("back", None)
    m = RE_PWR_VALUE.match(t)
    if m:
        return ("value", float(m.group(1)))
    return None


def write_state(state_dir, site_name, v, ts, node):
    """Escritura atómica del estado del sitio."""
    os.makedirs(state_dir, exist_ok=True)
    path = os.path.join(state_dir, f"{site_name}.state")
    tmp = path + ".tmp"
    payload = {"v": v, "ts": ts, "node": node}
    with open(tmp, "w") as f:
        json.dump(payload, f)
    os.replace(tmp, path)   # atómico: el plugin nunca ve un archivo a medias
    log.info("STATE %s -> v=%.1f ts=%d", site_name, v, ts)


# ---------------------------------------------------------------------------
# Mini servidor HTTP para Nagios remoto: GET /state/<sitio> -> JSON del .state
# (Nagios puede correr en otro host; el diseño contempla check por HTTP).
# ---------------------------------------------------------------------------
class StateHandler(http.server.BaseHTTPRequestHandler):
    state_dir = DEFAULT_STATE_DIR

    def do_GET(self):
        try:
            if self.path.startswith("/state/"):
                site = self.path[len("/state/"):].split("/")[0]
                path = os.path.join(self.state_dir, f"{site}.state")
                with open(path) as fh:
                    body = fh.read().encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path in ("/", "/sites"):
                sites = sorted(f[:-6] for f in os.listdir(self.state_dir) if f.endswith(".state"))
                body = json.dumps({"sites": sites}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_error(404)
        except FileNotFoundError:
            self.send_error(404, "no state for site")
        except Exception:
            self.send_error(500)

    def log_message(self, fmt, *args):
        log.info("HTTP %s", fmt % args)


def start_http_server(state_dir, port):
    """Lanza el servidor HTTP en un thread daemon. Devuelve el thread."""
    if not port:
        return None
    handler = type("StateHandler", (StateHandler,), {"state_dir": state_dir})
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    log.info("HTTP API en :%d (GET /state/<sitio>)", port)
    return t


async def connect_and_serve(cfg):
    """Conecta al companion y sirve mensajes. Lanza si el enlace muere."""
    sites = {s["pubkey"]: s["name"] for s in cfg.get("sites", [])}
    if not sites:
        log.warning("No hay sitios configurados (sites[] con pubkey) — nada que escuchar")
    comp = cfg["companion"]
    log.info("Conectando al companion %s:%s (%d sitios)...", comp["host"], comp["port"], len(sites))

    mc = await MeshCore.create_tcp(comp["host"], comp["port"])
    log.info("Conectado al companion")
    last_msg = {"ts": time.time()}

    async def on_disconnected(event):
        # El companion/repeater se reinició o el socket murió: loguear.
        # El watchdog lo detecta con is_connected() en <= probe_interval.
        log.warning("Evento DISCONNECTED del companion — el watchdog reconstruirá el enlace")

    async def on_contact_msg(event):
        try:
            last_msg["ts"] = time.time()   # cualquier mensaje = enlace vivo
            text = (event.payload.get("text") or "").strip()
            prefix = event.payload.get("pubkey_prefix", "")
            if not text:
                return
            # Identificar el sitio por la PUBKEY del remitente (diseño):
            # 1) matcheo por prefijo (6 bytes hex del evento, único en la práctica)
            #    — case-insensitive: el evento trae minúsculas, el config mayúsculas
            site = next((name for pk, name in sites.items()
                         if pk.lower().startswith(prefix.lower())), None)
            if site is None:
                contact = mc.get_contact_by_key_prefix(prefix)
                if contact is None:
                    try:
                        await mc.commands.get_contacts()
                        contact = mc.get_contact_by_key_prefix(prefix)
                    except Exception:
                        contact = None
                if contact is not None:
                    site = sites.get(contact.get("public_key", ""))
            if site is None:
                log.warning("Mensaje de pubkey desconocida %s (no está en sites[]) — ignorado: %r",
                            prefix, text)
                return
            sender_pk = next((pk for pk, n in sites.items() if n == site), prefix)
            parsed = parse_pwr(text)
            if parsed is None:
                log.info("Mensaje no-PWR de %s ignorado: %r", site, text)
                return
            kind, val = parsed
            now = int(time.time())
            if kind == "value":
                write_state(cfg["state_dir"], site, val, now, sender_pk)
            elif kind == "out":
                write_state(cfg["state_dir"], site, 0.0, now, sender_pk)
                log.warning("!!! CORTE DE RED en %s", site)
            else:  # back: el heartbeat con el valor real lo sobreescribe
                log.info("Red restablecida en %s (esperando heartbeat PWR)", site)
        except Exception as e:
            log.error("Callback error: %s", e, exc_info=True)

    mc.subscribe(EventType.CONTACT_MSG_RECV, on_contact_msg)
    try:
        mc.subscribe(EventType.DISCONNECTED, on_disconnected)
    except Exception as e:
        log.warning("No se pudo suscribir a DISCONNECTED: %s", e)
    await mc.start_auto_message_fetching()
    log.info("mesh_power listo. Esperando mensajes PWR de los nodos sensor...")

    # Watchdog de 3 capas — el probe solo NO basta: get_contacts() completa sin
    # excepción con el socket medio-muerto (verificado 9-sep-2026: tras reiniciar
    # el repeater por un upgrade, el daemon quedó sordo ~48 min; asyncio logueaba
    # "socket.send() raised exception" pero el probe daba por buena la conexión).
    #   1. is_connected() — detecta el socket muerto de forma directa.
    #   2. probe activo (get_contacts con timeout) — detecta cuelgues del companion.
    #   3. watchdog de silencio — si no llega NINGÚN mensaje en
    #      silence_reconnect_secs (default 720 = 2 ciclos de 5 min), recicla el
    #      enlace por precaución (0 = desactivado).
    wd = cfg["watchdog"]
    silence_secs = wd.get("silence_reconnect_secs", 720)
    while True:
        await asyncio.sleep(wd.get("probe_interval", 30))
        try:
            attr = mc.is_connected
            alive = attr() if callable(attr) else bool(attr)
        except Exception:
            alive = False
        if not alive:
            log.warning("Companion desconectado (is_connected=False) — reconstruyendo enlace")
            raise RuntimeError("companion desconectado")
        try:
            await asyncio.wait_for(mc.commands.get_contacts(), timeout=wd.get("probe_timeout", 8))
        except Exception as e:
            log.warning("Probe falló (%s) — reconstruyendo enlace", e)
            raise
        if silence_secs:
            idle = time.time() - last_msg["ts"]
            if idle > silence_secs:
                log.warning("Sin mensajes de la malla hace %.0fs (> %ds) — reciclando enlace por precaución",
                            idle, silence_secs)
                raise RuntimeError("silencio prolongado")


def main():
    ap = argparse.ArgumentParser(description="mesh_power daemon (NOC)")
    ap.add_argument("--config", default=os.environ.get("CONFIG_FILE", DEFAULT_CONFIG))
    args = ap.parse_args()

    cfg = load_config(args.config)
    start_http_server(cfg["state_dir"], cfg.get("http_port", 0))

    async def run():
        wd = cfg["watchdog"]
        while True:
            try:
                await connect_and_serve(cfg)
            except Exception as e:
                log.error("Enlace al companion caído: %s — reconectando en %ss",
                          e, wd["reconnect_delay"])
            await asyncio.sleep(wd["reconnect_delay"])

    asyncio.run(run())


if __name__ == "__main__":
    sys.exit(main())
