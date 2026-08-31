#!/usr/bin/env bash
# Instala el daemon mesh_power en el NOC (patrón ha-mesh-bridge).
# Uso: sudo bash install.sh   (en el Pi NOC, con el repo en /opt/mesh_power)
set -euo pipefail

APP_DIR=/opt/mesh_power
CFG_DIR=/etc/mesh_power
VENV=$APP_DIR/venv
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Creando $APP_DIR"
sudo mkdir -p "$APP_DIR"
sudo cp "$SRC_DIR/daemon/mesh_power.py" "$APP_DIR/"
sudo cp "$SRC_DIR/nagios/check_mesh_power" "$APP_DIR/"

echo "==> Venv + meshcore"
sudo python3 -m venv "$VENV"
sudo "$VENV/bin/pip" install -q --upgrade pip
sudo "$VENV/bin/pip" install -q meshcore

echo "==> Config"
sudo mkdir -p "$CFG_DIR" /var/lib/mesh_power
if [ ! -f "$CFG_DIR/config.json" ]; then
  sudo cp "$SRC_DIR/daemon/config.example.json" "$CFG_DIR/config.json"
  echo "   config.example.json copiado — EDITALO: companion port + sites[].pubkey"
else
  echo "   config.json ya existe (no se toca)"
fi
sudo chown -R repeater:repeater "$APP_DIR" "$CFG_DIR" /var/lib/mesh_power 2>/dev/null || true

echo "==> systemd"
sudo cp "$SRC_DIR/daemon/mesh_power.service" /etc/systemd/system/mesh_power.service
sudo systemctl daemon-reload
sudo systemctl enable mesh_power

echo ""
echo "SIGUIENTES PASOS:"
echo "  1. Editar $CFG_DIR/config.json (puerto del companion mesh_power + pubkey del nodo)"
echo "  2. sudo systemctl start mesh_power && sudo journalctl -u mesh_power -f"
echo "  3. Nagios: copiar nagios/check_mesh_power a /usr/lib/nagios/plugins/ y"
echo "     definir el service por sitio (ver nagios/nagios.cfg.example)"
