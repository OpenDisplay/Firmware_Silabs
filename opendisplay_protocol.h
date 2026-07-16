#ifndef OPENDISPLAY_PROTOCOL_H
#define OPENDISPLAY_PROTOCOL_H

#include <stdint.h>

#define CMD_CONFIG_READ         0x0040u
#define CMD_CONFIG_WRITE        0x0041u
#define CMD_CONFIG_CHUNK        0x0042u
#define CMD_FIRMWARE_VERSION    0x0043u
#define CMD_READ_MSD            0x0044u
#define CMD_DIRECT_WRITE_START  0x0070u
#define CMD_DIRECT_WRITE_DATA   0x0071u
#define CMD_DIRECT_WRITE_END    0x0072u
#define CMD_LED_ACTIVATE        0x0073u
#define CMD_LED_STOP            0x0075u
/* Client-visible but unimplemented on Silabs; dispatch NACKs them (see dispatch()) */
#define CMD_DIRECT_WRITE_PARTIAL_START 0x0076u
#define CMD_BUZZER_ACTIVATE            0x0077u
/* NFC: sub byte0 0=read, 1=write (+rec_type,len16,payload), 0x10/0x11/0x12=chunked write; rec_type OD_NFC_REC_* */
#define CMD_NFC_ENDPOINT        0x0083u
#define CMD_REBOOT              0x000Fu
#define CMD_AUTHENTICATE        0x0050u
#define CMD_ENTER_DFU           0x0051u
#define CMD_DEEP_SLEEP          0x0052u

#define AUTH_STATUS_CHALLENGE   0x00u
#define AUTH_STATUS_SUCCESS     AUTH_STATUS_CHALLENGE
#define AUTH_STATUS_FAILED      0x01u
#define AUTH_STATUS_ALREADY     0x02u
#define AUTH_STATUS_NOT_CONFIG  0x03u
#define AUTH_STATUS_RATE_LIMIT  0x04u
#define AUTH_STATUS_ERROR       0xFFu

#define RESP_AUTH_REQUIRED      0xFEu

#define RESP_CONFIG_READ        0x40u
#define RESP_CONFIG_WRITE       0x41u
#define RESP_CONFIG_CHUNK       0x42u
#define RESP_FIRMWARE_VERSION   0x43u
#define RESP_MSD_READ           0x44u
#define RESP_AUTHENTICATE       0x50u
#define RESP_LED_ACTIVATE_ACK   0x73u
#define RESP_LED_STOP_ACK       0x75u
#define RESP_NFC_ENDPOINT       0x83u
#define RESP_ENTER_DFU          0x51u
#define RESP_DEEP_SLEEP         0x52u

#define OD_PIPE_MAX_PAYLOAD     244u

#endif
