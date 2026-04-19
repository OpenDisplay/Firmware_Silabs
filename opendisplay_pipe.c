#include "opendisplay_pipe.h"
#include "opendisplay_ble.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_protocol.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_structs.h"
#include "em_device.h"
#include "em_system.h"
#include "psa/crypto.h"
#include "sl_sleeptimer.h"
#include <stdio.h>
#include <string.h>

#ifndef OPENDISPLAY_BUILD_ID
#define OPENDISPLAY_BUILD_ID "bg22-dev"
#endif

typedef struct {
  bool active;
  uint32_t total_size;
  uint16_t expected_chunks;
  uint16_t received_chunks;
  uint32_t received_size;
  uint8_t buffer[MAX_CONFIG_SIZE];
  uint8_t connection;
} od_chunked_config_t;

static uint16_t s_pipe_attr;
static bool s_notify;
static od_chunked_config_t s_cfg_chunk;
static uint8_t s_cfg_read_buf[MAX_RESPONSE_DATA_SIZE];
static struct EncryptionSession s_session;
static uint8_t s_long_write_buf[OD_PIPE_MAX_PAYLOAD];
static uint16_t s_long_write_len;
static uint8_t s_long_write_conn = 0xFFu;
static bool s_crypto_ready;
static uint8_t s_plain_buf[512];
static uint8_t s_crypto_payload_buf[513];

#ifndef OD_ALLOW_PLAINTEXT_WITH_SECURITY
#define OD_ALLOW_PLAINTEXT_WITH_SECURITY 0
#endif

static void pipe_send(uint8_t connection, const uint8_t *data, uint16_t len);

static void cfg_chunk_reset(void)
{
  memset(&s_cfg_chunk, 0, sizeof(s_cfg_chunk));
}

static uint32_t od_now_ms(void)
{
  uint32_t ticks = sl_sleeptimer_get_tick_count();
  return sl_sleeptimer_tick_to_ms(ticks);
}

static void crypto_init_once(void)
{
  if (s_crypto_ready) {
    return;
  }
  if (psa_crypto_init() == PSA_SUCCESS) {
    s_crypto_ready = true;
  }
}

static void clear_session(void)
{
  memset(&s_session, 0, sizeof(s_session));
}

static bool sec_enabled(void)
{
  const struct SecurityConfig *sec = od_get_parsed_security();
  return (sec != NULL) && (sec->encryption_enabled != 0u);
}

static bool session_alive(void)
{
  const struct SecurityConfig *sec = od_get_parsed_security();
  uint32_t now_ms;
  uint32_t timeout_ms;

  if (!s_session.authenticated) {
    return false;
  }
  if (sec == NULL || sec->session_timeout_seconds == 0u) {
    return true;
  }
  now_ms = od_now_ms();
  timeout_ms = (uint32_t)sec->session_timeout_seconds * 1000u;
  if ((now_ms - s_session.last_activity_ms) > timeout_ms) {
    clear_session();
    return false;
  }
  return true;
}

static bool aes_cmac_16(const uint8_t key[16], const uint8_t *msg, size_t msg_len, uint8_t out[16])
{
  psa_key_id_t key_id = 0;
  psa_status_t ps;
  size_t mac_len = 0;
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  if (!s_crypto_ready) {
    return false;
  }
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 128);
  psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
  ps = psa_import_key(&attr, key, 16, &key_id);
  if (ps != PSA_SUCCESS) {
    return false;
  }
  ps = psa_mac_compute(key_id, PSA_ALG_CMAC, msg, msg_len, out, 16, &mac_len);
  (void)psa_destroy_key(key_id);
  return (ps == PSA_SUCCESS) && (mac_len == 16u);
}

static bool aes_ecb_encrypt_16(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
  psa_key_id_t key_id = 0;
  psa_status_t ps;
  size_t out_len = 0;
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  if (!s_crypto_ready) {
    return false;
  }
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 128);
  psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
  ps = psa_import_key(&attr, key, 16, &key_id);
  if (ps != PSA_SUCCESS) {
    return false;
  }
  ps = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &out_len);
  (void)psa_destroy_key(key_id);
  return (ps == PSA_SUCCESS) && (out_len == 16u);
}

static bool derive_session_key(const uint8_t master_key[16],
                               const uint8_t client_nonce[16],
                               const uint8_t server_nonce[16],
                               const uint8_t device_id[4],
                               uint8_t session_key[16])
{
  static const uint8_t label[] = "OpenDisplay session";
  uint8_t cmac_input[64];
  uint8_t intermediate[16];
  uint8_t final_input[16];
  size_t offset = 0;

  memcpy(cmac_input + offset, label, sizeof(label) - 1u);
  offset += sizeof(label) - 1u;
  cmac_input[offset++] = 0x00u;
  memcpy(cmac_input + offset, device_id, 4u);
  offset += 4u;
  memcpy(cmac_input + offset, client_nonce, 16u);
  offset += 16u;
  memcpy(cmac_input + offset, server_nonce, 16u);
  offset += 16u;
  cmac_input[offset++] = 0x00u;
  cmac_input[offset++] = 0x80u;

  if (!aes_cmac_16(master_key, cmac_input, offset, intermediate)) {
    return false;
  }
  memset(final_input, 0, sizeof(final_input));
  final_input[7] = 0x01u;
  memcpy(final_input + 8, intermediate, 8u);
  return aes_ecb_encrypt_16(master_key, final_input, session_key);
}

static bool derive_session_id(const uint8_t session_key[16],
                              const uint8_t client_nonce[16],
                              const uint8_t server_nonce[16],
                              uint8_t session_id[8])
{
  uint8_t input[32];
  uint8_t cmac_out[16];
  memcpy(input, client_nonce, 16u);
  memcpy(input + 16, server_nonce, 16u);
  if (!aes_cmac_16(session_key, input, sizeof(input), cmac_out)) {
    return false;
  }
  memcpy(session_id, cmac_out, 8u);
  return true;
}

static bool od_random(uint8_t *buf, size_t len)
{
  if (!s_crypto_ready) {
    return false;
  }
  return psa_generate_random(buf, len) == PSA_SUCCESS;
}

static void send_auth_required_response(uint8_t connection, uint8_t resp_byte)
{
  uint8_t err[] = { 0x00u, resp_byte, RESP_AUTH_REQUIRED };
  pipe_send(connection, err, sizeof(err));
}

/*
 * Minimal AES-CCM (RFC 3610) using AES-ECB as primitive.
 * Fixed parameters: nonce=13B, tag=12B, L=2, AAD=exactly 2 bytes.
 * B_0 flags = Adata(1) | M'=(12-2)/2<<3 | L-1 = 0x40|0x28|0x01 = 0x69
 */
static bool od_ccm_ecb(const uint8_t *in, uint8_t *out)
{
  return aes_ecb_encrypt_16(s_session.session_key, in, out);
}

static bool od_ccm_encrypt(const uint8_t *nonce,
                            const uint8_t *aad,
                            const uint8_t *plain,
                            uint16_t plain_len,
                            uint8_t *cipher,
                            uint8_t *tag)
{
  uint8_t blk[16], mac[16], stream[16];
  uint16_t i, full, rem;

  blk[0] = 0x69u;
  memcpy(&blk[1], nonce, 13u);
  blk[14] = (uint8_t)((plain_len >> 8) & 0xFFu);
  blk[15] = (uint8_t)(plain_len & 0xFFu);
  if (!od_ccm_ecb(blk, mac)) return false;

  memset(blk, 0, 16u);
  blk[0] = 0x00u; blk[1] = 0x02u;
  blk[2] = aad[0]; blk[3] = aad[1];
  for (i = 0; i < 16u; i++) mac[i] ^= blk[i];
  if (!od_ccm_ecb(mac, mac)) return false;

  full = plain_len / 16u; rem = plain_len % 16u;
  for (i = 0; i < full; i++) {
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= plain[i * 16u + j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }
  if (rem > 0u) {
    memset(blk, 0, 16u);
    memcpy(blk, &plain[full * 16u], rem);
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= blk[j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }

  blk[0] = 0x01u;
  memcpy(&blk[1], nonce, 13u);
  blk[14] = 0x00u; blk[15] = 0x00u;
  if (!od_ccm_ecb(blk, stream)) return false;
  for (i = 0; i < 12u; i++) tag[i] = mac[i] ^ stream[i];

  for (i = 0; i < full; i++) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((i + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((i + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < 16u; j++) cipher[i * 16u + j] = plain[i * 16u + j] ^ stream[j];
  }
  if (rem > 0u) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((full + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((full + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < rem; j++) cipher[full * 16u + j] = plain[full * 16u + j] ^ stream[j];
  }
  return true;
}

static bool od_ccm_decrypt(const uint8_t *nonce,
                            const uint8_t *aad,
                            const uint8_t *cipher,
                            uint16_t cipher_len,
                            const uint8_t *tag,
                            uint8_t *plain)
{
  uint8_t blk[16], mac[16], stream[16];
  uint16_t i, full, rem;
  uint8_t expected[12], diff;

  full = cipher_len / 16u; rem = cipher_len % 16u;
  for (i = 0; i < full; i++) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((i + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((i + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < 16u; j++) plain[i * 16u + j] = cipher[i * 16u + j] ^ stream[j];
  }
  if (rem > 0u) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((full + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((full + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < rem; j++) plain[full * 16u + j] = cipher[full * 16u + j] ^ stream[j];
  }

  blk[0] = 0x69u; memcpy(&blk[1], nonce, 13u);
  blk[14] = (uint8_t)((cipher_len >> 8) & 0xFFu);
  blk[15] = (uint8_t)(cipher_len & 0xFFu);
  if (!od_ccm_ecb(blk, mac)) return false;

  memset(blk, 0, 16u);
  blk[0] = 0x00u; blk[1] = 0x02u;
  blk[2] = aad[0]; blk[3] = aad[1];
  for (i = 0; i < 16u; i++) mac[i] ^= blk[i];
  if (!od_ccm_ecb(mac, mac)) return false;

  for (i = 0; i < full; i++) {
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= plain[i * 16u + j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }
  if (rem > 0u) {
    memset(blk, 0, 16u);
    memcpy(blk, &plain[full * 16u], rem);
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= blk[j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }

  blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
  blk[14] = 0x00u; blk[15] = 0x00u;
  if (!od_ccm_ecb(blk, stream)) return false;
  for (i = 0; i < 12u; i++) expected[i] = mac[i] ^ stream[i];

  diff = 0u;
  for (i = 0; i < 12u; i++) diff |= (expected[i] ^ tag[i]);
  return (diff == 0u);
}

static bool decrypt_encrypted_payload(uint16_t cmd,
                                      const uint8_t *payload,
                                      uint16_t payload_len,
                                      uint8_t *out_plain,
                                      uint16_t *out_plain_len)
{
  const uint8_t *nonce;
  const uint8_t *cipher;
  const uint8_t *tag;
  uint8_t nonce_ccm[13];
  uint8_t ad[2];
  uint16_t cipher_len;

  if (payload_len < (16u + 12u + 1u) || !session_alive()) {
    return false;
  }
  nonce      = payload;
  tag        = &payload[payload_len - 12u];
  cipher     = &payload[16u];
  cipher_len = (uint16_t)(payload_len - 16u - 12u);
  memcpy(nonce_ccm, &nonce[3], sizeof(nonce_ccm));
  ad[0] = (uint8_t)((cmd >> 8) & 0xFFu);
  ad[1] = (uint8_t)(cmd & 0xFFu);

  if (!od_ccm_decrypt(nonce_ccm, ad, cipher, cipher_len, tag, out_plain)) {
    return false;
  }
  if (out_plain[0] > (cipher_len - 1u)) {
    return false;
  }
  *out_plain_len = out_plain[0];
  if (*out_plain_len > 0u) {
    memmove(out_plain, &out_plain[1], *out_plain_len);
  }
  s_session.last_activity_ms = od_now_ms();
  return true;
}

static bool encrypt_response_payload(const uint8_t *plain,
                                     uint16_t plain_len,
                                     uint8_t *out,
                                     uint16_t *out_len)
{
  uint8_t nonce[16];
  uint8_t nonce_ccm[13];
  uint8_t ad[2];
  uint8_t tag[12];
  uint16_t payload_len;

  if (!session_alive() || plain_len < 2u || plain_len > 514u) {
    return false;
  }
  payload_len = (uint16_t)(plain_len - 2u);
  memcpy(nonce, s_session.session_id, 8u);
  for (int i = 0; i < 8; i++) {
    nonce[8 + i] = (uint8_t)((s_session.nonce_counter >> (56 - (i * 8))) & 0xFFu);
  }
  s_session.nonce_counter++;
  memcpy(nonce_ccm, &nonce[3], sizeof(nonce_ccm));
  ad[0] = plain[0];
  ad[1] = plain[1];
  s_crypto_payload_buf[0] = (uint8_t)(payload_len & 0xFFu);
  if (payload_len > 0u) {
    memcpy(&s_crypto_payload_buf[1], &plain[2], payload_len);
  }

  memcpy(out, plain, 2u);
  memcpy(&out[2], nonce, 16u);
  if (!od_ccm_encrypt(nonce_ccm, ad,
                       s_crypto_payload_buf, (uint16_t)(payload_len + 1u),
                       &out[18], tag)) {
    return false;
  }
  memcpy(&out[18 + payload_len + 1u], tag, 12u);
  *out_len = (uint16_t)(18u + payload_len + 1u + 12u);
  s_session.last_activity_ms = od_now_ms();
  return true;
}

static void pipe_send_raw(uint8_t connection, const uint8_t *data, uint16_t len)
{
  sl_status_t sc;
  if (!s_notify || len == 0u) {
    return;
  }
  sc = sl_bt_gatt_server_send_notification(connection, s_pipe_attr, len, data);
  if (sc != SL_STATUS_OK) {
    printf("[OD] pipe notify sc=0x%04lX len=%u\r\n", (unsigned long)sc, (unsigned)len);
  }
}

static void pipe_send(uint8_t connection, const uint8_t *data, uint16_t len)
{
  uint8_t enc[544];
  uint8_t err[3];
  uint16_t enc_len = 0;
  uint8_t status;
  uint8_t cmd;
  bool force_plain;

  if (len == 0u) {
    return;
  }
  if (len < 2u) {
    pipe_send_raw(connection, data, len);
    return;
  }
  status = data[0];
  cmd = data[1];
  force_plain = (status == RESP_AUTH_REQUIRED || status == 0xFFu || cmd == RESP_AUTHENTICATE
                 || cmd == RESP_FIRMWARE_VERSION || cmd == RESP_MSD_READ);
  if (force_plain || !session_alive()) {
    pipe_send_raw(connection, data, len);
    return;
  }
  if (encrypt_response_payload(data, len, enc, &enc_len)) {
    pipe_send_raw(connection, enc, enc_len);
    return;
  }
  err[0] = 0x00u;
  err[1] = cmd;
  err[2] = 0xFFu;
  pipe_send_raw(connection, err, sizeof(err));
}

static void reply_firmware_version(uint8_t connection)
{
  uint8_t rsp[2 + 1 + 1 + 1 + 40];
  uint16_t ver = opendisplay_ble_get_app_version();
  uint8_t major = (uint8_t)((ver >> 8) & 0xFFu);
  uint8_t minor = (uint8_t)(ver & 0xFFu);
  const char *sha = OPENDISPLAY_BUILD_ID;
  uint8_t sha_len = (uint8_t)strlen(sha);
  uint16_t o = 0;

  if (sha_len > 40u) {
    sha_len = 40u;
  }
  rsp[o++] = 0x00u;
  rsp[o++] = RESP_FIRMWARE_VERSION;
  rsp[o++] = major;
  rsp[o++] = minor;
  rsp[o++] = sha_len;
  memcpy(&rsp[o], sha, sha_len);
  o += sha_len;
  pipe_send(connection, rsp, o);
}

static void reply_read_msd(uint8_t connection)
{
  uint8_t rsp[2 + 16];

  rsp[0] = 0x00u;
  rsp[1] = RESP_MSD_READ;
  opendisplay_ble_copy_msd_bytes(&rsp[2]);
  pipe_send(connection, rsp, sizeof(rsp));
}

static bool authenticate_handle(const uint8_t *payload, uint16_t payload_len, uint8_t *rsp, uint16_t *rsp_len)
{
  const struct SecurityConfig *sec = od_get_parsed_security();
  static const uint8_t zero8[8] = { 0 };
  uint32_t now;
  uint8_t device_id[4];
  uint8_t expected[16];
  uint8_t challenge_input[36];
  uint8_t server_input[36];

  *rsp_len = 3u;
  rsp[0] = 0x00u;
  rsp[1] = RESP_AUTHENTICATE;
  rsp[2] = AUTH_STATUS_ERROR;
  if (sec == NULL || sec->encryption_enabled == 0u) {
    rsp[2] = AUTH_STATUS_NOT_CONFIG;
    return false;
  }
  now = od_now_ms();
  if (s_session.last_auth_time_ms != 0u && (now - s_session.last_auth_time_ms) < 60000u) {
    if (s_session.auth_attempts >= 10u) {
      rsp[2] = AUTH_STATUS_RATE_LIMIT;
      return false;
    }
  } else {
    s_session.auth_attempts = 0u;
  }
  s_session.auth_attempts++;
  s_session.last_auth_time_ms = now;
  crypto_init_once();
  {
    uint64_t uid = SYSTEM_GetUnique();
    device_id[0] = (uint8_t)((uid >> 24) & 0xFFu);
    device_id[1] = (uint8_t)((uid >> 16) & 0xFFu);
    device_id[2] = (uint8_t)((uid >> 8) & 0xFFu);
    device_id[3] = (uint8_t)(uid & 0xFFu);
  }

  if (payload_len == 1u && payload[0] == 0x00u) {
    if (s_session.authenticated && session_alive()) {
      clear_session();
    }
    if (!od_random(s_session.pending_server_nonce, 16u)) {
      return false;
    }
    s_session.server_nonce_time_ms = now;
    rsp[2] = AUTH_STATUS_CHALLENGE;
    memcpy(&rsp[3], s_session.pending_server_nonce, 16u);
    memcpy(&rsp[19], device_id, 4u);
    *rsp_len = 23u;
    return false;
  }
  if (payload_len != 32u || (now - s_session.server_nonce_time_ms) > 30000u) {
    return false;
  }
  memcpy(challenge_input, s_session.pending_server_nonce, 16u);
  memcpy(&challenge_input[16], payload, 16u);
  memcpy(&challenge_input[32], device_id, 4u);
  if (!aes_cmac_16(sec->encryption_key, challenge_input, sizeof(challenge_input), expected)) {
    return false;
  }
  if (memcmp(expected, &payload[16], 16u) != 0) {
    rsp[2] = AUTH_STATUS_FAILED;
    memset(s_session.pending_server_nonce, 0, 16u);
    return false;
  }
  if (!derive_session_key(sec->encryption_key, payload, s_session.pending_server_nonce, device_id,
                          s_session.session_key)) {
    return false;
  }
  memcpy(s_session.client_nonce, payload, 16u);
  memcpy(s_session.server_nonce, s_session.pending_server_nonce, 16u);
  if (!derive_session_id(s_session.session_key, payload, s_session.server_nonce, s_session.session_id)) {
    return false;
  }
  if (memcmp(s_session.session_id, zero8, sizeof(zero8)) == 0) {
    return false;
  }
  memset(s_session.replay_window, 0, sizeof(s_session.replay_window));
  s_session.last_seen_counter = 0u;
  s_session.replay_idx = 0u;
  s_session.integrity_failures = 0u;
  s_session.authenticated = true;
  s_session.last_activity_ms = now;
  s_session.session_start_ms = now;
  s_session.nonce_counter = 0u;
  memset(s_session.pending_server_nonce, 0, 16u);
  s_session.server_nonce_time_ms = 0u;
  memcpy(server_input, s_session.server_nonce, 16u);
  memcpy(&server_input[16], payload, 16u);
  memcpy(&server_input[32], device_id, 4u);
  if (!aes_cmac_16(s_session.session_key, server_input, sizeof(server_input), expected)) {
    clear_session();
    return false;
  }
  rsp[2] = AUTH_STATUS_SUCCESS;
  memcpy(&rsp[3], expected, 16u);
  *rsp_len = 19u;
  return true;
}

static void handle_direct_write_start(uint8_t connection, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t ok[] = { 0x00u, 0x70u };
  uint8_t err[] = { 0xFFu, 0x70u };
  printf("[OD] pipe 0070 recv len=%u (epd init next)\r\n", (unsigned)payload_len);
  if (opendisplay_display_direct_write_start(payload, payload_len) == 0) {
    pipe_send(connection, ok, sizeof(ok));
  } else {
    pipe_send(connection, err, sizeof(err));
  }
}

static void handle_direct_write_data(uint8_t connection, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t ack_data[] = { 0x00u, 0x71u };
  uint8_t err[] = { 0xFFu, 0x71u };

  if (opendisplay_display_direct_write_data(payload, payload_len) != 0) {
    pipe_send(connection, err, sizeof(err));
    return;
  }

  pipe_send(connection, ack_data, sizeof(ack_data));
}

static void handle_direct_write_end(uint8_t connection, const uint8_t *payload, uint16_t payload_len)
{
  bool refresh_ok = false;
  uint8_t ack_end[] = { 0x00u, 0x72u };
  uint8_t ack_refresh_ok[] = { 0x00u, 0x73u };
  uint8_t ack_refresh_timeout[] = { 0x00u, 0x74u };
  uint8_t err[] = { 0xFFu, 0x72u };

  if (opendisplay_display_direct_write_end(payload, payload_len, &refresh_ok) != 0) {
    pipe_send(connection, err, sizeof(err));
    return;
  }
  pipe_send(connection, ack_end, sizeof(ack_end));
  pipe_send(connection, refresh_ok ? ack_refresh_ok : ack_refresh_timeout,
            refresh_ok ? sizeof(ack_refresh_ok) : sizeof(ack_refresh_timeout));
}

static void handle_config_read(uint8_t connection)
{
  static uint8_t config_data[MAX_CONFIG_SIZE];
  uint32_t config_len = MAX_CONFIG_SIZE;
  const uint16_t max_chunks = 10u;

  if (!initConfigStorage()) {
    uint8_t err[] = { 0xFFu, RESP_CONFIG_READ, 0x00u, 0x00u };
    pipe_send(connection, err, sizeof(err));
    return;
  }

  if (!loadConfig(config_data, &config_len)) {
    uint8_t empty[] = {
      0x00u, RESP_CONFIG_READ, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    pipe_send(connection, empty, sizeof(empty));
    return;
  }

  uint32_t remaining = config_len;
  uint32_t offset = 0;
  uint16_t chunk_number = 0;

  while (remaining > 0 && chunk_number < max_chunks) {
    uint16_t response_len = 0;
    uint16_t chunk_size;

    s_cfg_read_buf[response_len++] = 0x00u;
    s_cfg_read_buf[response_len++] = RESP_CONFIG_READ;
    s_cfg_read_buf[response_len++] = (uint8_t)(chunk_number & 0xFFu);
    s_cfg_read_buf[response_len++] = (uint8_t)((chunk_number >> 8) & 0xFFu);

    if (chunk_number == 0u) {
      s_cfg_read_buf[response_len++] = (uint8_t)(config_len & 0xFFu);
      s_cfg_read_buf[response_len++] = (uint8_t)((config_len >> 8) & 0xFFu);
    }

    {
      uint16_t max_data = (uint16_t)(MAX_RESPONSE_DATA_SIZE - response_len);
      chunk_size = (remaining < max_data) ? (uint16_t)remaining : max_data;
    }

    if (chunk_size == 0u) {
      break;
    }

    memcpy(s_cfg_read_buf + response_len, config_data + offset, chunk_size);
    response_len += chunk_size;

    if (response_len > MAX_RESPONSE_DATA_SIZE) {
      break;
    }

    pipe_send(connection, s_cfg_read_buf, response_len);
    offset += chunk_size;
    remaining -= chunk_size;
    chunk_number++;
  }
}

static void handle_config_write(uint8_t connection, const uint8_t *data, uint16_t len)
{
  uint8_t ack[] = { 0x00u, RESP_CONFIG_WRITE, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_WRITE, 0x00u, 0x00u };

  if (len == 0u) {
    return;
  }

  if (len > CONFIG_CHUNK_SIZE) {
    cfg_chunk_reset();
    s_cfg_chunk.active = true;
    s_cfg_chunk.connection = connection;
    s_cfg_chunk.received_chunks = 1;

    if (len >= CONFIG_CHUNK_SIZE_WITH_PREFIX) {
      s_cfg_chunk.total_size = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
      if (s_cfg_chunk.total_size > MAX_CONFIG_SIZE || s_cfg_chunk.total_size == 0u) {
        cfg_chunk_reset();
        pipe_send(connection, err, sizeof(err));
        return;
      }
      {
        uint16_t chunk_data_size = (uint16_t)(len - 2u);
        if (chunk_data_size > CONFIG_CHUNK_SIZE) {
          chunk_data_size = CONFIG_CHUNK_SIZE;
        }
        if ((uint32_t)chunk_data_size > s_cfg_chunk.total_size) {
          chunk_data_size = (uint16_t)s_cfg_chunk.total_size;
        }
        memcpy(s_cfg_chunk.buffer, data + 2, chunk_data_size);
        s_cfg_chunk.received_size = chunk_data_size;
      }
    } else {
      s_cfg_chunk.total_size = len;
      if (s_cfg_chunk.total_size > MAX_CONFIG_SIZE) {
        cfg_chunk_reset();
        pipe_send(connection, err, sizeof(err));
        return;
      }
      {
        uint16_t chunk_size = (len < CONFIG_CHUNK_SIZE) ? len : CONFIG_CHUNK_SIZE;
        memcpy(s_cfg_chunk.buffer, data, chunk_size);
        s_cfg_chunk.received_size = chunk_size;
      }
    }

    if (s_cfg_chunk.received_size >= s_cfg_chunk.total_size) {
      if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
        opendisplay_ble_reload_config_from_nvm();
        pipe_send(connection, ack, sizeof(ack));
      } else {
        pipe_send(connection, err, sizeof(err));
      }
      cfg_chunk_reset();
      return;
    }

    {
      uint32_t rem = s_cfg_chunk.total_size - s_cfg_chunk.received_size;
      s_cfg_chunk.expected_chunks =
        (uint16_t)(1u + (rem + CONFIG_CHUNK_SIZE - 1u) / CONFIG_CHUNK_SIZE);
    }

    pipe_send(connection, ack, sizeof(ack));
    return;
  }

  if (saveConfig((uint8_t *)(void *)data, len)) {
    opendisplay_ble_reload_config_from_nvm();
    pipe_send(connection, ack, sizeof(ack));
  } else {
    pipe_send(connection, err, sizeof(err));
  }
}

static void handle_config_chunk(uint8_t connection, const uint8_t *data, uint16_t len)
{
  uint8_t ack[] = { 0x00u, RESP_CONFIG_CHUNK, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CHUNK, 0x00u, 0x00u };

  if (!s_cfg_chunk.active || s_cfg_chunk.connection != connection) {
    pipe_send(connection, err, sizeof(err));
    return;
  }
  if (len == 0u) {
    return;
  }
  if (len > CONFIG_CHUNK_SIZE) {
    cfg_chunk_reset();
    pipe_send(connection, err, sizeof(err));
    return;
  }
  if (s_cfg_chunk.received_size + len > MAX_CONFIG_SIZE) {
    cfg_chunk_reset();
    pipe_send(connection, err, sizeof(err));
    return;
  }
  if (s_cfg_chunk.received_chunks >= MAX_CONFIG_CHUNKS) {
    cfg_chunk_reset();
    pipe_send(connection, err, sizeof(err));
    return;
  }

  memcpy(s_cfg_chunk.buffer + s_cfg_chunk.received_size, data, len);
  s_cfg_chunk.received_size += len;
  s_cfg_chunk.received_chunks++;

  if (s_cfg_chunk.received_chunks >= s_cfg_chunk.expected_chunks) {
    if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
      opendisplay_ble_reload_config_from_nvm();
      pipe_send(connection, ack, sizeof(ack));
    } else {
      pipe_send(connection, err, sizeof(err));
    }
    cfg_chunk_reset();
  } else {
    pipe_send(connection, ack, sizeof(ack));
  }
}

static void dispatch(uint8_t connection, uint16_t cmd, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t auth_rsp[32];
  uint16_t auth_rsp_len = 0;
  if (cmd == CMD_AUTHENTICATE) {
    (void)authenticate_handle(payload, payload_len, auth_rsp, &auth_rsp_len);
    pipe_send(connection, auth_rsp, auth_rsp_len);
    return;
  }
  if (sec_enabled() && !session_alive()) {
#if !OD_ALLOW_PLAINTEXT_WITH_SECURITY
    send_auth_required_response(connection, (uint8_t)(cmd & 0xFFu));
    return;
#endif
  }
  switch (cmd) {
    case CMD_FIRMWARE_VERSION:
      reply_firmware_version(connection);
      break;
    case CMD_READ_MSD:
      reply_read_msd(connection);
      break;
    case CMD_CONFIG_READ:
      handle_config_read(connection);
      break;
    case CMD_CONFIG_WRITE:
      handle_config_write(connection, payload, payload_len);
      break;
    case CMD_CONFIG_CHUNK:
      handle_config_chunk(connection, payload, payload_len);
      break;
    case CMD_REBOOT:
      printf("[OD] reboot\r\n");
      for (volatile uint32_t i = 0; i < 800000u; i++) {
      }
      NVIC_SystemReset();
      break;
    case CMD_ENTER_DFU:
      {
        uint8_t ok[] = { 0x00u, RESP_ENTER_DFU };
        pipe_send(connection, ok, sizeof(ok));
      }
      opendisplay_ble_schedule_dfu();
      break;
    case CMD_DEEP_SLEEP:
      {
        uint8_t ok[] = { 0x00u, RESP_DEEP_SLEEP };
        pipe_send(connection, ok, sizeof(ok));
      }
      opendisplay_ble_schedule_deep_sleep();
      break;
    case CMD_LED_ACTIVATE: {
      uint8_t ok[] = { 0x00u, RESP_LED_ACTIVATE_ACK, 0x00u, 0x00u };
      uint8_t e1[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x01u, 0x00u };
      uint8_t e2[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x02u, 0x00u };

      if (payload_len < 1u) {
        pipe_send(connection, e1, sizeof(e1));
        break;
      }
      if (opendisplay_led_activate(payload[0], payload + 1u,
                                  (uint16_t)(payload_len - 1u)) != 0) {
        pipe_send(connection, e2, sizeof(e2));
        break;
      }
      pipe_send(connection, ok, sizeof(ok));
      break;
    }
    case CMD_DIRECT_WRITE_START:
      handle_direct_write_start(connection, payload, payload_len);
      break;
    case CMD_DIRECT_WRITE_DATA:
      handle_direct_write_data(connection, payload, payload_len);
      break;
    case CMD_DIRECT_WRITE_END:
      handle_direct_write_end(connection, payload, payload_len);
      break;
    default:
      printf("[OD] unknown cmd 0x%04X\r\n", (unsigned)cmd);
      break;
  }
}

static void on_pipe_write(uint8_t connection, uint16_t attr, uint8_t att_opcode,
                          uint16_t offset, const uint8_t *data, uint16_t len)
{
  uint16_t cmd;
  const uint8_t *frame = data;
  uint16_t frame_len = len;
  uint16_t plain_len = 0;

  if (attr != s_pipe_attr) {
    return;
  }
  if (att_opcode != (uint8_t)sl_bt_gatt_write_request
      && att_opcode != (uint8_t)sl_bt_gatt_write_command) {
    return;
  }
  if (offset != 0u) {
    if (offset > sizeof(s_long_write_buf) || len > sizeof(s_long_write_buf) - offset) {
      return;
    }
    if (connection != s_long_write_conn) {
      s_long_write_conn = connection;
      s_long_write_len = 0;
    }
    memcpy(&s_long_write_buf[offset], data, len);
    if ((offset + len) > s_long_write_len) {
      s_long_write_len = offset + len;
    }
    return;
  }
  if (s_long_write_conn == connection && s_long_write_len > 0u) {
    frame = s_long_write_buf;
    frame_len = s_long_write_len;
    s_long_write_len = 0;
  }
  if (frame_len < 2u) {
    return;
  }
  if (frame_len > OD_PIPE_MAX_PAYLOAD) {
    uint8_t err[] = { 0xFFu, frame[1], 0xFEu };
    pipe_send(connection, err, sizeof(err));
    return;
  }

  cmd = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
  if (sec_enabled() && cmd != CMD_AUTHENTICATE && frame_len >= 31u) {
    if (!session_alive()) {
      send_auth_required_response(connection, frame[1]);
      return;
    }
    if (!decrypt_encrypted_payload(cmd, &frame[2], (uint16_t)(frame_len - 2u), s_plain_buf, &plain_len)) {
      uint8_t err[] = { 0x00u, frame[1], 0xFFu };
      pipe_send(connection, err, sizeof(err));
      return;
    }
    dispatch(connection, cmd, s_plain_buf, plain_len);
    return;
  }
  dispatch(connection, cmd, &frame[2], (uint16_t)(frame_len - 2u));
}

void opendisplay_pipe_set_characteristic(uint16_t pipe_value_handle)
{
  s_pipe_attr = pipe_value_handle;
  s_notify = false;
}

void opendisplay_pipe_on_connection_closed(void)
{
  s_notify = false;
  clear_session();
  s_long_write_len = 0;
  s_long_write_conn = 0xFFu;
  cfg_chunk_reset();
  opendisplay_display_abort();
}

void opendisplay_pipe_handle_gatt_event(sl_bt_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_gatt_server_attribute_value_id: {
      sl_bt_evt_gatt_server_attribute_value_t *e = &evt->data.evt_gatt_server_attribute_value;
      on_pipe_write(e->connection,
                    e->attribute,
                    e->att_opcode,
                    e->offset,
                    e->value.data,
                    e->value.len);
      break;
    }
    case sl_bt_evt_gatt_server_characteristic_status_id: {
      sl_bt_evt_gatt_server_characteristic_status_t *e =
        &evt->data.evt_gatt_server_characteristic_status;
      if (e->characteristic != s_pipe_attr) {
        break;
      }
      if (e->status_flags == (uint8_t)sl_bt_gatt_server_client_config) {
        s_notify = (e->client_config_flags & (uint16_t)sl_bt_gatt_server_notification) != 0u;
        printf("[OD] pipe notifications %s\r\n", s_notify ? "on" : "off");
      }
      break;
    }
    default:
      break;
  }
}
