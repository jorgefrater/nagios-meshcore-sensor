// ============================================================
// mesh_power_sensor — nodo sensor de voltaje de red para la malla MeshCore
//
// Base: example simple_sensor de MeshCore v1.17.1
// Añade: ZMPT101B (GPIO4) → RMS → telemetría LPP + heartbeat "PWR x.x"
//        + alerta inmediata de corte de red (HIGH_PRI, con reintentos).
//
// El nodo además REPITE malla por defecto (allowPacketForward ya devuelve
// true salvo disable_fwd) — requisito: aprovechar el hardware al máximo.
// ============================================================
#include "SensorMesh.h"
#include "Zmpt101b.h"

#ifndef PIN_ZMPT101B
  #define PIN_ZMPT101B 4                  // GPIO4 = ADC1_CH3 (ver DESIGN.md)
#endif
#ifndef ZMPT_VCC_VOLTS
  #define ZMPT_VCC_VOLTS 3.3f             // VCC del módulo = 3.3V del Heltec
#endif
#ifndef CORTE_THRESHOLD_V
  #define CORTE_THRESHOLD_V 30.0f         // < 30V RMS = corte de red
#endif
#ifndef RED_RESTORED_THRESHOLD_V
  #define RED_RESTORED_THRESHOLD_V 50.0f  // > 50V RMS = red restablecida (histéresis)
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

class MyMesh : public SensorMesh {
public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : SensorMesh(board, radio, ms, rng, rtc, tables),
       voltage_data(12*24, 5*60),         // 24h de voltaje de red, cada 5 min
       sensor(PIN_ZMPT101B, ZMPT_VCC_VOLTS)
  {
    // Escala calibrada con multímetro el 30-ago-2026 (122.5V / 0.2152 raw_rms).
    sensor.setScale(569.2f);
  }

protected:
  /* ===================== lógica del nodo sensor ===================== */
  Trigger heartbeat, power_out, power_back;
  TimeSeriesData  voltage_data;
  Zmpt101b  sensor;
  bool was_in_cut = false;

  void onSensorDataRead() override {
    // Voltaje de red RMS — ventana ~0.5s a ~2kHz (60 ciclos de 60Hz).
    const float v = sensor.readLineVoltage(1000, 500);
    publishLineVoltage(v);                          // telemetría canal self
    voltage_data.recordData(getRTCClock(), v);      // serie temporal

    // Heartbeat periódico al NOC: mensaje TXT directo "PWR 121.3" en CADA ciclo
    // de lectura (1 intento por ciclo, sin bloqueo por Trigger/ACK).
    char txt[32];
    snprintf(txt, sizeof(txt), "PWR %.1f", v);
    if (v >= RED_RESTORED_THRESHOLD_V) {
      sendPeriodicMessage(txt);
    }

    // Corte de red: alerta inmediata de ALTA prioridad (no espera el ciclo).
    if (v < CORTE_THRESHOLD_V) {
      alertIf(true, power_out, HIGH_PRI_ALERT, "PWR OUT");
      was_in_cut = true;
    } else if (was_in_cut && v > RED_RESTORED_THRESHOLD_V) {
      alertIf(true, power_back, HIGH_PRI_ALERT, "PWR BACK");
      was_in_cut = false;
    }
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) override {
    voltage_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago, &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) override {
    if (strncmp(command, "pwr", 3) == 0) {          // query manual: 'pwr'
      snprintf(reply, MAX_PACKET_PAYLOAD, "V=%.1f", sensor.getLastRawVrms() * sensor.getScale());
      return true;                                   // handled
    }
    return false;                                    // not handled
  }
  /* ================================================================== */
};

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    Serial.print(c);            // eco
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
}
