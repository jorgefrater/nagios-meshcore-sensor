#pragma once
//
// Zmpt101b — lectura de voltaje AC de red con módulo ZMPT101B (Heltec V3 / ESP32-S3)
//
// Conexión: VCC->3V3 del Heltec, GND->GND, OUT->GPIO4 (ADC1_CH3).
// El módulo entrega una senoidal centrada en VCC/2; se calcula RMS por muestreo
// y se escala a voltaje real de línea con calibración de 1 punto (2 puntos opcional).
//
#include <Arduino.h>

class Zmpt101b {
public:
  explicit Zmpt101b(uint8_t pin, float vcc_volts = 3.3f);

  // Muestrea `samples` lecturas separadas `sample_interval_us` y devuelve el
  // RMS de la componente AC (voltios). Con samples=1000 a 500us (~2kHz) la
  // ventana es ~0.5s (30 ciclos de 60Hz) — estable.
  float readRawVrms(uint16_t samples = 1000, uint32_t sample_interval_us = 500);

  // Calibración: V_line_real = raw_rms * scale + offset
  // (normalmente offset=0 porque el offset DC se cancela en el RMS).
  void setScale(float scale) { _scale = scale; }
  void setOffset(float offset_volts) { _offset = offset_volts; }
  float getScale() const { return _scale; }

  // Voltaje de línea estimado (RMS, voltios).
  float readLineVoltage(uint16_t samples = 1000, uint32_t sample_interval_us = 500) {
    return readRawVrms(samples, sample_interval_us) * _scale + _offset;
  }

  // Último RMS raw medido (para queries/debug).
  float getLastRawVrms() const { return _last_vrms; }

  // Presencia de red con histéresis: declara corte solo si V < threshold_low
  // y vuelve a "presente" solo si V > threshold_high. Evita flapping en bajones.
  bool isPowerPresent(float threshold_low_v = 30.0f, float threshold_high_v = 50.0f,
                      uint16_t samples = 500, uint32_t sample_interval_us = 500);

private:
  uint8_t _pin;
  float   _vcc;
  float   _scale;
  float   _offset;
  bool    _present;
  float   _last_vrms;
};
