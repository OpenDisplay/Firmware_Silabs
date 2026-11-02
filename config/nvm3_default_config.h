#ifndef NVM3_DEFAULT_CONFIG_H
#define NVM3_DEFAULT_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>NVM3 Default Instance Configuration

#ifndef NVM3_DEFAULT_CACHE_SIZE
// <o NVM3_DEFAULT_CACHE_SIZE> NVM3 Default Instance Cache Size
// <i> Number of NVM3 objects to cache. To reduce access times this number
// <i> should be equal to or higher than the number of NVM3 objects in the
// <i> default NVM3 instance.
// <i> Default: 200
#define NVM3_DEFAULT_CACHE_SIZE  200
#endif

#ifndef NVM3_DEFAULT_MAX_OBJECT_SIZE
// <o NVM3_DEFAULT_MAX_OBJECT_SIZE> NVM3 Default Instance Max Object Size
// <i> Max NVM3 object size that can be stored.
// <i> Must cover opendisplay config record: 16-byte header + MAX_CONFIG_SIZE (4080).
// <i> Keep in sync with NVM3_MAX_OBJECT_SIZE in cmake_gcc/opendisplay-bg22.cmake.
// <i>
// <i> THIS VALUE AND NVM3_DEFAULT_NVM_SIZE MOVE TOGETHER. nvm3_open() rejects the
// <i> instance when nvmSize < thrSize, and thrSize scales with max-object:
// <i>   max-object 2112 -> thrSize 20588   max-object 4096 -> thrSize 24556
// <i> With the old 24576-byte region, 4096 cleared the threshold by TWENTY BYTES
// <i> and failed in practice: NVM3 would not open, the stored config read back as
// <i> 0 bytes, and every config write NACKed. Measured on ODBB1B76 2026-11-02.
// <i> At 32768 the same setting has ~8 KB of margin.
#define NVM3_DEFAULT_MAX_OBJECT_SIZE  4096
#endif

#ifndef NVM3_DEFAULT_REPACK_HEADROOM
// <o NVM3_DEFAULT_REPACK_HEADROOM> NVM3 Default Instance User Repack Headroom
// <i> Headroom determining how many bytes below the forced repack limit the user
// <i> repack limit should be placed. The default is 0, which means the user and
// <i> forced repack limits are equal.
// <i> Default: 0
#define NVM3_DEFAULT_REPACK_HEADROOM  0
#endif

#ifndef NVM3_DEFAULT_NVM_SIZE
// <o NVM3_DEFAULT_NVM_SIZE> NVM3 Default Instance Size
// <i> Size of the NVM3 storage region in flash. This size should be aligned with
// <i> the flash page size of the device.
// <i> EFR32BG22: FLASH_PAGE_SIZE 8192; 4 pages (32768) supports max-object 4096
// <i> with ~8 KB of margin. 3 pages (24576) only supported max-object 2112.
// <i>
// <i> GROWING THIS IS NON-DESTRUCTIVE. Measured on ODBB1B76 2026-11-02: the
// <i> original encryption key and drive data both survived 24576 -> 32768 with
// <i> no restore step. The region is pinned to __main_flash_end__ and grows
// <i> DOWNWARD (0x50000 -> 0x4E000, end fixed at 0x56000), so existing pages
// <i> stay in range with valid headers; NVM3 opens, keeps every object, and
// <i> formats only the newly added page. Two further units (ODBF2156,
// <i> ODCF0ED5) were migrated on 2026-11-02 and authenticated afterwards with
// <i> their existing keys.
// <i>
// <i> SHRINKING IT, or any change that moves the region's END, is a different
// <i> matter and is not covered by that measurement.
// <i>
// <i> If NVM3 ever does fail to open, the device still boots, advertises and
// <i> accepts an UNENCRYPTED config write, because with no stored security
// <i> packet there is nothing to authenticate against. The recovery is then:
// <i> read the config off the device, OTA, write it back over BLE. Read it
// <i> FIRST -- afterwards the key is gone and you cannot re-derive it.
// <i>
// <i> Flash cost: NVM3 sits at the top of the app region (0x56000) and grows
// <i> downward. The linker ASSERT below the .nvm section turns an overflow into a
// <i> build failure, not a bricked device. At 32768 the gap between the end of
// <i> code and NVM3 is ~10.8 KB; a 5th page would leave only ~2.6 KB.
#define NVM3_DEFAULT_NVM_SIZE  32768
#endif

// </h>

// <<< end of configuration section >>>

#endif // NVM3_DEFAULT_CONFIG_H

