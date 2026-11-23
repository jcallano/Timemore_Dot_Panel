// Timemore Dot Panel — API del puente BLE (ble_bridge.cpp) para la interfaz.
//
// El puente lee la Timemore Dot (cliente BLE) y, si esta activado, se anuncia
// como bascula BooKoo ("BOOKOO_SC_U XXXXXX") para la Wendougee / Decaid.
// Todas las funciones son no bloqueantes.
#pragma once
#include <stdint.h>

struct BridgeSample {
  uint32_t seq;          // se incrementa con cada muestra nueva de la Dot (~10 Hz)
  float weight;          // peso con la tara aplicada [g]
  float flow;            // caudal EMA [g/s]
  uint32_t arrival_us;   // esp_timer (32 bits) cuando llego la muestra
};

void bridge_begin(bool bridgeEnabled);   // arranca NimBLE y las tareas del puente
void bridge_poll();                      // diagnostico por serie (llamar desde loop)
void bridge_serial_cmd(char c);          // comandos del puente: ? l t r s p

void bridge_get_sample(BridgeSample* out);
uint32_t bridge_sample_seq();
bool bridge_scale_connected();
bool bridge_scale_scanning();
bool bridge_scale_released();
bool bridge_app_connected();
bool bridge_enabled();
bool bridge_app_timer_running();
uint8_t bridge_scale_battery();
const char* bridge_name();
const char* bridge_scale_mac();

void bridge_tare();                      // tara por software inmediata + tara fisica en la Dot
void bridge_release_scale();             // desconecta la Dot y deja de buscarla
void bridge_reconnect_scale();           // vuelve a buscar la Dot
void bridge_forget_scale();              // borra la MAC guardada y los bonds
void bridge_set_enabled(bool en);        // activa o desactiva el anuncio BooKoo

// Aviso de muestra nueva: se llama desde la tarea bridge_tx (nucleo 0) con el peso y
// el caudal ya calculados. Debe ser rapido y no bloquear (p. ej. despertar otra tarea).
void bridge_set_sample_hook(void (*cb)(void));
void bridge_probe_dot();                 // diagnostico: un comando ligero; su respuesta mide la cola de la Dot
