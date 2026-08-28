#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_config_storage.h"
#include <stdio.h>
#include <string.h>

static struct SecurityConfig s_od_security_parsed;

const struct SecurityConfig *od_get_parsed_security(void)
{
  return &s_od_security_parsed;
}


#define TRANSMISSION_MODE_CLEAR_ON_BOOT (1 << 7)

static uint16_t crc16_ccitt_feed(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)((uint16_t)b << 8);
    for (int j = 0; j < 8; j++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)(((uint32_t)crc << 1) ^ OD_CONFIG_CRC_POLY);
        } else {
            crc = (uint16_t)((uint32_t)crc << 1);
        }
    }
    return crc;
}

static uint16_t config_toolbox_outer_crc16(const uint8_t *data, uint32_t body_len)
{
    if (body_len < 2U) {
        uint16_t crc = OD_CONFIG_CRC_INIT;

        for (uint32_t i = 0; i < body_len; i++) {
            crc = crc16_ccitt_feed(crc, data[i]);
        }
        return crc;
    }
    uint16_t crc = OD_CONFIG_CRC_INIT;

    crc = crc16_ccitt_feed(crc, 0);
    crc = crc16_ccitt_feed(crc, 0);
    for (uint32_t i = 2U; i < body_len; i++) {
        crc = crc16_ccitt_feed(crc, data[i]);
    }
    return crc;
}

bool parseConfigBytes(uint8_t* configData, uint32_t configLen, struct GlobalConfig* globalConfig) {
    if (globalConfig == NULL || configData == NULL) {
        printf("Invalid parameters for parseConfigBytes\n");
        return false;
    }
    
    memset(globalConfig, 0, sizeof(struct GlobalConfig));
    
    if (configLen < 3) {
        printf("Config too short: %u bytes\r\n", (unsigned)configLen);
        globalConfig->loaded = false;
        return false;
    }
    
    printf("Parsing config: %u bytes\r\n", (unsigned)configLen);
    
    uint32_t offset = 0;
    offset += 2;
    
    globalConfig->version = configData[offset++];
    globalConfig->minor_version = 0; // Not stored in current format
    
    uint32_t packetIndex = 0;
    while (offset < configLen - 2) { // -2 for CRC
        if (offset > configLen) {
            printf("Offset overflow: offset=%u > configLen=%u\r\n", (unsigned)offset, (unsigned)configLen);
            globalConfig->loaded = false;
            return false;
        }
        
        uint32_t remaining = configLen - 2 - offset;
        if (offset + 2 > configLen - 2) {
            printf("Loop exit: not enough for header (need 2, have %u)\r\n", (unsigned)remaining);
            break;
        }
        
        uint8_t packetNum = configData[offset];
        uint8_t packetId = configData[offset + 1];
        offset += 2; // Advance past packet header
        
        if (offset > configLen) {
            printf("Offset overflow after header: offset=%u > configLen=%u\r\n", (unsigned)offset, (unsigned)configLen);
            globalConfig->loaded = false;
            return false;
        }
        
        packetIndex++; // Count this packet (before processing, so we count even if we skip it)
        if (packetId == CONFIG_PKT_SYSTEM || packetId == CONFIG_PKT_MANUFACTURER || 
            packetId == CONFIG_PKT_POWER || packetId == CONFIG_PKT_DISPLAY) {
            printf("Pkt #%u ID=0x%02X\r\n", (unsigned)packetNum, packetId);
        }
        
        switch (packetId) {
            case CONFIG_PKT_SYSTEM: // system_config
                if (offset > configLen) {
                    printf("Offset overflow before system_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct SystemConfig) <= configLen - 2) {
                    memcpy(&globalConfig->system_config, &configData[offset], sizeof(struct SystemConfig));
                    offset += sizeof(struct SystemConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after system_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("system_config: need %zu, have %u\r\n", sizeof(struct SystemConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_MANUFACTURER: // manufacturer_data
                if (offset > configLen) {
                    printf("Offset overflow before manufacturer_data\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct ManufacturerData) <= configLen - 2) {
                    memcpy(&globalConfig->manufacturer_data, &configData[offset], sizeof(struct ManufacturerData));
                    offset += sizeof(struct ManufacturerData);
                    if (offset > configLen) {
                        printf("Offset overflow after manufacturer_data\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("manufacturer_data: need %zu, have %u\r\n", sizeof(struct ManufacturerData), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_POWER: // power_option
                if (offset > configLen) {
                    printf("Offset overflow before power_option\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct PowerOption) <= configLen - 2) {
                    memcpy(&globalConfig->power_option, &configData[offset], sizeof(struct PowerOption));
                    offset += sizeof(struct PowerOption);
                    if (offset > configLen) {
                        printf("Offset overflow after power_option\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("power_option: need %zu, have %u\r\n", sizeof(struct PowerOption), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_DISPLAY: // display
                if (offset > configLen) {
                    printf("Offset overflow before display\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->display_count < 4 && offset + sizeof(struct DisplayConfig) <= configLen - 2) {
                    memcpy(&globalConfig->displays[globalConfig->display_count], &configData[offset], sizeof(struct DisplayConfig));
                    printf("Display: ic=0x%04X %dx%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].panel_ic_type,
                                 globalConfig->displays[globalConfig->display_count].pixel_width,
                                 globalConfig->displays[globalConfig->display_count].pixel_height);
                    printf("Display: RST=%d BUSY=%d DC=%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].reset_pin,
                                 globalConfig->displays[globalConfig->display_count].busy_pin,
                                 globalConfig->displays[globalConfig->display_count].dc_pin);
                    printf("Display: CS=%d DATA=%d CLK=%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].cs_pin,
                                 globalConfig->displays[globalConfig->display_count].data_pin,
                                 globalConfig->displays[globalConfig->display_count].clk_pin);
                    printf("Display: color=%d modes=0x%02X\r\n", 
                                 globalConfig->displays[globalConfig->display_count].color_scheme,
                                 globalConfig->displays[globalConfig->display_count].transmission_modes);
                    offset += sizeof(struct DisplayConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after display\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->display_count++;
                } else if (globalConfig->display_count >= 4) {
                    offset += sizeof(struct DisplayConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after display (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("display: need %zu, have %u\r\n", sizeof(struct DisplayConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_LED: // led - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before led\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->led_count < 4 && offset + sizeof(struct LedConfig) <= configLen - 2) {
                    memcpy(&globalConfig->leds[globalConfig->led_count], &configData[offset], sizeof(struct LedConfig));
                    offset += sizeof(struct LedConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after led\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->led_count++;
                } else if (globalConfig->led_count >= 4) {
                    offset += sizeof(struct LedConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after led (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("led: need %zu, have %u\r\n", sizeof(struct LedConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_SENSOR: // sensor_data - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before sensor\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->sensor_count < 4 && offset + sizeof(struct SensorData) <= configLen - 2) {
                    memcpy(&globalConfig->sensors[globalConfig->sensor_count], &configData[offset], sizeof(struct SensorData));
                    offset += sizeof(struct SensorData);
                    if (offset > configLen) {
                        printf("Offset overflow after sensor\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->sensor_count++;
                } else if (globalConfig->sensor_count >= 4) {
                    offset += sizeof(struct SensorData);
                    if (offset > configLen) {
                        printf("Offset overflow after sensor (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("sensor: need %zu, have %u\r\n", sizeof(struct SensorData), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_DATA_BUS: // data_bus - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before data_bus\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->data_bus_count < 4 && offset + sizeof(struct DataBus) <= configLen - 2) {
                    memcpy(&globalConfig->data_buses[globalConfig->data_bus_count], &configData[offset], sizeof(struct DataBus));
                    offset += sizeof(struct DataBus);
                    if (offset > configLen) {
                        printf("Offset overflow after data_bus\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->data_bus_count++;
                } else if (globalConfig->data_bus_count >= 4) {
                    offset += sizeof(struct DataBus);
                    if (offset > configLen) {
                        printf("Offset overflow after data_bus (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("data_bus: need %zu, have %u\r\n", sizeof(struct DataBus), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_BINARY_INPUT: // binary_inputs - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before binary_input\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->binary_input_count < 4 && offset + sizeof(struct BinaryInputs) <= configLen - 2) {
                    memcpy(&globalConfig->binary_inputs[globalConfig->binary_input_count], &configData[offset], sizeof(struct BinaryInputs));
                    offset += sizeof(struct BinaryInputs);
                    if (offset > configLen) {
                        printf("Offset overflow after binary_input\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->binary_input_count++;
                } else if (globalConfig->binary_input_count >= 4) {
                    offset += sizeof(struct BinaryInputs);
                    if (offset > configLen) {
                        printf("Offset overflow after binary_input (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("binary_input: need %zu, have %u\r\n", sizeof(struct BinaryInputs), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_WIFI: // wifi_config - skip this as requested
                if (offset > configLen) {
                    printf("Offset overflow before wifi\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + 162 <= configLen - 2) {
                    offset += 162;
                    if (offset > configLen) {
                        printf("Offset overflow after wifi\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    offset = configLen - 2; // Skip to CRC
                }
                break;

            case CONFIG_PKT_SECURITY: // security_config (0x27)
                if (offset > configLen) {
                    printf("Offset overflow before security_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct SecurityConfig) <= configLen - 2) {
                    memcpy(&s_od_security_parsed, &configData[offset], sizeof(struct SecurityConfig));
                    offset += sizeof(struct SecurityConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after security_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    printf("Security: enabled=%d, flags=0x%02X, reset_pin=%d\r\n",
                                 s_od_security_parsed.encryption_enabled,
                                 s_od_security_parsed.flags,
                                 s_od_security_parsed.reset_pin);
                } else {
                    printf("security_config: need %zu, have %u\r\n",
                                  sizeof(struct SecurityConfig), (unsigned)(configLen - 2 - offset));
                    offset = configLen - 2;
                }
                break;

            case CONFIG_PKT_TOUCH: // touch_controller (0x28) - handled by main MCU, skip on Silabs
                if (offset > configLen) {
                    printf("Offset overflow before touch\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + CONFIG_PKT_TOUCH_SIZE <= configLen - 2) {
                    offset += CONFIG_PKT_TOUCH_SIZE;
                    if (offset > configLen) {
                        printf("Offset overflow after touch\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    offset = configLen - 2; // Skip to CRC
                }
                break;

            case CONFIG_PKT_BUZZER: // passive_buzzer (0x29) - handled by main MCU, skip on Silabs
                if (offset > configLen) {
                    printf("Offset overflow before buzzer\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + CONFIG_PKT_BUZZER_SIZE <= configLen - 2) {
                    offset += CONFIG_PKT_BUZZER_SIZE;
                    if (offset > configLen) {
                        printf("Offset overflow after buzzer\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    offset = configLen - 2; // Skip to CRC
                }
                break;

            case CONFIG_PKT_NFC: // nfc_config (0x2A)
                if (offset > configLen) {
                    printf("Offset overflow before nfc_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->nfc_config_count < 2 && offset + sizeof(struct NfcConfig) <= configLen - 2) {
                    memcpy(&globalConfig->nfc_configs[globalConfig->nfc_config_count], &configData[offset], sizeof(struct NfcConfig));
                    offset += sizeof(struct NfcConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after nfc_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->nfc_config_count++;
                } else if (globalConfig->nfc_config_count >= 2) {
                    offset += sizeof(struct NfcConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after nfc_config (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("nfc_config: need %zu, have %u\r\n", sizeof(struct NfcConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_FLASH: // flash_config (0x2B)
                if (offset > configLen) {
                    printf("Offset overflow before flash_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->flash_config_count < 2 && offset + sizeof(struct FlashConfig) <= configLen - 2) {
                    memcpy(&globalConfig->flash_configs[globalConfig->flash_config_count], &configData[offset], sizeof(struct FlashConfig));
                    offset += sizeof(struct FlashConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after flash_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->flash_config_count++;
                } else if (globalConfig->flash_config_count >= 2) {
                    offset += sizeof(struct FlashConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after flash_config (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("flash_config: need %zu, have %u\r\n", sizeof(struct FlashConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_DATA_EXTENDED: // data_extended (0x2C) - host-only strings, skip on Silabs
                if (offset > configLen) {
                    printf("Offset overflow before data_extended\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + CONFIG_PKT_DATA_EXTENDED_SIZE <= configLen - 2) {
                    offset += CONFIG_PKT_DATA_EXTENDED_SIZE;
                    if (offset > configLen) {
                        printf("Offset overflow after data_extended\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    offset = configLen - 2; // Skip to CRC
                }
                break;

            case CONFIG_PKT_FINDMY: // findmy_config (0x2D)
                if (offset > configLen) {
                    printf("Offset overflow before findmy_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct FindMyConfig) <= configLen - 2) {
                    memcpy(&globalConfig->findmy_config, &configData[offset], sizeof(struct FindMyConfig));
                    offset += sizeof(struct FindMyConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after findmy_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->has_findmy_config = true;
                    printf("FindMy: flags=0x%02X, curve=%u, k=%u\r\n",
                                 globalConfig->findmy_config.flags,
                                 globalConfig->findmy_config.fmdn_curve,
                                 globalConfig->findmy_config.fmdn_k);
                } else {
                    printf("findmy_config: need %zu, have %u\r\n",
                                 sizeof(struct FindMyConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            default:
                printf("Unknown pkt 0x%02X @%u\r\n", packetId, (unsigned)(offset - 2));
                offset = configLen - 2; // Skip to CRC
                break;
        }
    }
    
    printf("Parsed %u pkts, offset=%u/%u\r\n", (unsigned)packetIndex, (unsigned)offset, (unsigned)(configLen - 2));
    
    if (configLen >= 2) {
        uint16_t crcGiven = configData[configLen - 2] | (configData[configLen - 1] << 8);
        uint16_t crcCalculated = config_toolbox_outer_crc16(configData, configLen - 2);
        if (crcGiven != crcCalculated) {
            printf("CRC mismatch: 0x%04X vs 0x%04X\r\n", crcGiven, crcCalculated);
        }
    }
    
    globalConfig->loaded = true;
    printf("Config parsed successfully: version=%d, displays=%d, leds=%d, sensors=%d, data_buses=%d, binary_inputs=%d, nfc=%d, flash=%d\r\n",
                 globalConfig->version, globalConfig->display_count, globalConfig->led_count,
                 globalConfig->sensor_count, globalConfig->data_bus_count, globalConfig->binary_input_count,
                 globalConfig->nfc_config_count, globalConfig->flash_config_count);
    return true;
}

bool loadGlobalConfig(struct GlobalConfig* globalConfig) {
    if (globalConfig == NULL) {
        printf("Invalid parameter for loadGlobalConfig\n");
        return false;
    }
    
    memset(globalConfig, 0, sizeof(struct GlobalConfig));
    globalConfig->loaded = false;
    
    uint8_t *configData = opendisplay_config_buf();
    uint32_t configLen = MAX_CONFIG_SIZE;
    
    if (!initConfigStorage()) {
        printf("Failed to initialize config storage\n");
        return false;
    }
    
    if (!loadConfig(configData, &configLen)) {
        printf("No config found\n");
        return false;
    }
    
    return parseConfigBytes(configData, configLen, globalConfig);
}
