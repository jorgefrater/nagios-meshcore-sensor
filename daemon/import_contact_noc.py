#!/usr/bin/env python3
"""Importa el contacto del nodo sensor en el companion mesh_power (0xd3).

Uso (en el NOC): editar NODE_PUBKEY con la pubkey del nodo desplegado.
"""
import sqlite3, time, shutil

DB = "/var/lib/openhop_repeater/repeater.db"
# Pubkey hex de 64 chars del nodo sensor (identidad MeshCore del dispositivo)
NODE_PUBKEY = bytes.fromhex("<pubkey-hex-64-del-nodo-sensor>")
NODE_NAME = "PWR Sensor <SITIO>"

shutil.copy2(DB, f"{DB}.bak-pwrcontact-{int(time.time())}")
print("backup DB OK")

db = sqlite3.connect(DB)
db.execute(
    "INSERT OR IGNORE INTO companion_contacts "
    "(companion_hash, pubkey, name, adv_type, flags, out_path_len, "
    " last_advert_timestamp, lastmod, updated_at) "
    "VALUES (?, ?, ?, 0, 0, -1, 0, 0, ?)",
    ("0xd3", NODE_PUBKEY, NODE_NAME, time.time()),
)
db.commit()
for r in db.execute(
    "SELECT id, companion_hash, name, length(pubkey) FROM companion_contacts "
    "WHERE name=?",
    (NODE_NAME,),
):
    print("contacto importado:", r)
db.close()
