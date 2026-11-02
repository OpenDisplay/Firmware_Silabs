#ifndef OPENDISPLAY_RUNTIME_H
#define OPENDISPLAY_RUNTIME_H

/*
 * Firmware-local (RAM-only) OpenDisplay types and flags.
 *
 * These are intentionally NOT part of the shared wire-protocol header
 * opendisplay_structs.h. That header is meant to be a byte-for-byte vendored
 * copy of opendisplay-protocol/src/opendisplay_structs.h (managed by
 * tools/sync_protocol_header.py), and the canonical header explicitly excludes
 * repo-specific / in-memory types such as GlobalConfig and EncryptionSession.
 *
 * Keeping them here lets opendisplay_structs.h become a clean vendored copy
 * while this firmware-owned header holds everything Silabs-specific.
 *
 * The wire structs referenced below (SystemConfig, SecurityConfig, ...) come
 * from opendisplay_structs.h, included here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

/* In-memory aggregate of all parsed config packets. Not a wire type. */
struct GlobalConfig {
  struct SystemConfig system_config;
  struct ManufacturerData manufacturer_data;
  struct PowerOption power_option;
  struct DisplayConfig displays[4];
  uint8_t display_count;
  /* Panel DRIVE data (0x2E/0x2F/0x31) is deliberately ABSENT from this struct.
   *
   * It used to live here as display_modes[4] + display_luts[2] -- 1710 bytes of
   * a 32 KB part, holding a second copy of bytes that are already in NVM3. The
   * driver consumes them exactly twice per refresh (once to walk the sequence,
   * once to clock the waveform out of SPI), so residency bought nothing.
   *
   * od_epd reads them on demand through od_cfg_index: a 192-byte map of where
   * each packet body sits, bodies fetched with nvm3_readPartialData. The 535-
   * byte waveform is streamed to the panel 64 bytes at a time and never lands
   * in RAM whole at all. */
  struct LedConfig leds[4];
  uint8_t led_count;
  struct SensorData sensors[4];
  uint8_t sensor_count;
  struct DataBus data_buses[4];
  uint8_t data_bus_count;
  struct BinaryInputs binary_inputs[4];
  uint8_t binary_input_count;
  struct NfcConfig nfc_configs[2];
  uint8_t nfc_config_count;
  struct FlashConfig flash_configs[2];
  uint8_t flash_config_count;
  struct FindMyConfig findmy_config;
  bool has_findmy_config;
  uint8_t version;
  uint8_t minor_version;
  bool loaded;
};

/* In-memory BLE encryption / auth session + replay-protection state.
 * Not a wire type. */
struct EncryptionSession {
  bool authenticated;
  uint8_t session_key[16];
  uint8_t session_id[8];
  uint64_t nonce_counter;
  uint64_t last_seen_counter;
  uint64_t replay_window[64];
  uint8_t replay_idx;
  uint32_t last_activity_ms;
  uint8_t integrity_failures;
  uint32_t session_start_ms;
  uint8_t auth_attempts;
  uint32_t last_auth_time_ms;
  uint8_t client_nonce[16];
  uint8_t server_nonce[16];
  uint8_t pending_server_nonce[16];
  uint32_t server_nonce_time_ms;
};

/* Firmware-local interpretation of the SecurityConfig.flags bitfield
 * (SecurityConfig itself is a wire struct in opendisplay_structs.h). */
#define SECURITY_FLAG_REWRITE_ALLOWED     (1 << 0)
#define SECURITY_FLAG_SHOW_KEY_ON_SCREEN  (1 << 1)
#define SECURITY_FLAG_RESET_PIN_ENABLED   (1 << 2)
#define SECURITY_FLAG_RESET_PIN_POLARITY  (1 << 3)
#define SECURITY_FLAG_RESET_PIN_PULLUP    (1 << 4)
#define SECURITY_FLAG_RESET_PIN_PULLDOWN  (1 << 5)

#endif /* OPENDISPLAY_RUNTIME_H */
