// ============================================================
// Calibración del ZMPT101B — corre en el Heltec V3 conectado por USB.
// Sube el raw RMS y el voltaje estimado por serial (115200 baud).
//
// Procedimiento:
//  1) Compilar y subir este sketch (board: Heltec WiFi LoRa 32 V3).
//  2) ZMPT101B conectado (VCC->3V3, GND->GND, OUT->GPIO4) pero SIN red.
//     Anotar raw_rms ≈ 0 (confirma offset OK).
//  3) Conectar la red (bornas del módulo, CUIDADO: 120V aislados por el
//     transformador, fusible 0.5A en serie).
//  4) Medir con multímetro el voltaje real del tomacorriente (p.ej. 121.3V).
//  5) En el serial: raw_rms_X. Calcular scale = V_ref / raw_rms_X.
//  6) Poner ese valor en `scale` abajo, resubir, verificar que el serial
//     muestre ≈ V_ref. Ajustar el potenciómetro del módulo si la amplitud
//     es muy baja o recorta (picos >2.9V = recorte en el ADC a 3.3V).
// ============================================================
#include <Zmpt101b.h>

const uint8_t PIN_ZMPT = 4;        // GPIO4 (ADC1_CH3)
const float   SCALE    = 569.2f;   // calibrado 30-ago-2026: 122.5V / 0.2152 raw_rms

Zmpt101b sensor(PIN_ZMPT, 3.3f);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nZMPT101B calibration — raw RMS y V_line estimado");
  Serial.printf("Pin GPIO%d, VCC 3.3V, scale %.4f\n\n", PIN_ZMPT, SCALE);
  sensor.setScale(SCALE);
}

void loop() {
  const float raw = sensor.readRawVrms(1000, 500);   // ~0.5s de ventana
  const float v   = raw * sensor.getScale();
  Serial.printf("raw_rms=%.4f V  |  V_line~%.1f V  |  %s\n",
                raw, v, v > 30.0f ? "RED PRESENTE" : "SIN RED");
  delay(2000);
}
