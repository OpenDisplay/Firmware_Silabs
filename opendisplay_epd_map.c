#include "opendisplay_epd_map.h"
#include "bb_epaper.h"

int opendisplay_map_epd(int id)
{
  switch (id) {
    case 0x0000: return EP_PANEL_UNDEFINED;
    case 0x0001: return EP42_400x300;
    case 0x0002: return EP42B_400x300;
    case 0x0003: return EP213_122x250;
    case 0x0004: return EP213B_122x250;
    case 0x0005: return EP293_128x296;
    case 0x0006: return EP294_128x296;
    case 0x0007: return EP295_128x296;
    case 0x0008: return EP295_128x296_4GRAY;
    case 0x0009: return EP266_152x296;
    case 0x000A: return EP102_80x128;
    case 0x000B: return EP27B_176x264;
    case 0x000C: return EP29R_128x296;
    case 0x000D: return EP122_192x176;
    case 0x000E: return EP154R_152x152;
    case 0x000F: return EP42R_400x300;
    case 0x0010: return EP42R2_400x300;
    case 0x0011: return EP37_240x416;
    case 0x0012: return EP37B_240x416;
    case 0x0013: return EP213_104x212;
    case 0x0014: return EP75_800x480;
    case 0x0015: return EP75_800x480_4GRAY;
    case 0x0016: return EP75_800x480_4GRAY_V2;
    case 0x0017: return EP29_128x296;
    case 0x0018: return EP29_128x296_4GRAY;
    case 0x0019: return EP213R_122x250;
    case 0x001A: return EP154_200x200;
    case 0x001B: return EP154B_200x200;
    case 0x001C: return EP266YR_184x360;
    case 0x001D: return EP29YR_128x296;
    case 0x001E: return EP29YR_168x384;
    case 0x001F: return EP583_648x480;
    case 0x0020: return EP296_128x296;
    case 0x0021: return EP26R_152x296;
    case 0x0022: return EP73_800x480;
    case 0x0023: return EP73_SPECTRA_800x480;
    case 0x0024: return EP74R_640x384;
    case 0x0025: return EP583R_600x448;
    case 0x0026: return EP75R_800x480;
    case 0x0027: return EP426_800x480;
    case 0x0028: return EP426_800x480_4GRAY;
    case 0x0029: return EP29R2_128x296;
    case 0x002A: return EP41_640x400;
    case 0x002B: return EP81_SPECTRA_1024x576;
    case 0x002C: return EP7_960x640;
    case 0x002D: return EP213R2_122x250;
    case 0x002E: return EP29Z_128x296;
    case 0x002F: return EP29Z_128x296_4GRAY;
    case 0x0030: return EP213Z_122x250;
    case 0x0031: return EP213Z_122x250_4GRAY;
    case 0x0032: return EP154Z_152x152;
    case 0x0033: return EP579_792x272;
    case 0x0034: return EP213YR_122x250;
    case 0x0035: return EP37YR_240x416;
    case 0x0036: return EP35YR_184x384;
    case 0x0037: return EP397YR_800x480;
    case 0x0038: return EP154YR_200x200;
    case 0x0039: return EP266YR2_184x360;
    case 0x003A: return EP42YR_400x300;
    case 0x003B: return EP75_800x480_GEN2;
    case 0x003C: return EP75_800x480_4GRAY_GEN2;
    case 0x003D: return EP215YR_160x296;
    case 0x003E: return EP1085_1360x480;
    case 0x003F: return EP31_240x320;
    case 0x0040: return EP75YR_800x480;
    case 0x0041: return EP_PANEL_UNDEFINED;
    default: return EP_PANEL_UNDEFINED;
  }
}
