#include "Zmpt101b.h"

Zmpt101b::Zmpt101b(uint8_t pin, float vcc_volts)
    : _pin(pin), _vcc(vcc_volts), _scale(1.0f), _offset(0.0f), _present(false), _last_vrms(0.0f) {}

float Zmpt101b::readRawVrms(uint16_t samples, uint32_t sample_interval_us) {
  // analogReadMilliVolts devuelve el voltaje real del pin (0..~3100 mV con
  // atenuación 11dB). Disponible en arduino-esp32 v3.x (PlatformIO).
  if (samples < 16) samples = 16;

  float sum = 0.0f;
  float sum_sq = 0.0f;
  const uint32_t t0 = micros();

  for (uint16_t i = 0; i < samples; i++) {
    const float v = analogReadMilliVolts(_pin) / 1000.0f;   // voltios
    sum += v;
    sum_sq += v * v;
    // Espera restante del intervalo para mantener la frecuencia de muestreo
    // estable aunque analogRead tarde un poco (evita alias en la senoidal).
    const int32_t remaining = (int32_t)sample_interval_us - (int32_t)(micros() - t0 - (uint32_t)i * sample_interval_us);
    if (remaining > 0) delayMicroseconds((uint32_t)remaining);
  }

  const float mean = sum / samples;
  // varianza = media de cuadrados - cuadrado de la media (>=0 por construcción)
  float variance = (sum_sq / samples) - (mean * mean);
  if (variance < 0.0f) variance = 0.0f;

  _last_vrms = sqrtf(variance);
  return _last_vrms;
}

bool Zmpt101b::isPowerPresent(float threshold_low_v, float threshold_high_v,
                              uint16_t samples, uint32_t sample_interval_us) {
  const float v = readLineVoltage(samples, sample_interval_us);
  if (_present) {
    if (v < threshold_low_v) _present = false;      // corte declarado
  } else {
    if (v > threshold_high_v) _present = true;      // red restablecida
  }
  return _present;
}
