#include "boardconfig.h"

#include "statusleds.h"
#include "log.h"

#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/flash.h>
#include <pico/unique_id.h>
#include "pico/multicore.h"

extern StatusLeds statusLeds;

extern void core1_tasks();

const uint8_t *config_flash_contents = (const uint8_t *) (XIP_BASE + CONFIG_FLASH_OFFSET);

ConfigData* BoardConfig::activeConfig;
ConfigSource BoardConfig::configSource = ConfigSource::Fallback;
uint8_t BoardConfig::shortId;
char BoardConfig::boardSerialString[25];
char BoardConfig::boardHostnameString[12];
bool BoardConfig::boardIsPicoW;

void BoardConfig::initI2C() {
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    // Pull-ups are populated on the base board
    // However, we enable the internal ones as well, doesn't hurt
    // Please note that the external ones are required according to the
    // RP2040 datasheet and my measurements confirm that
    gpio_pull_up(PIN_I2C_SCL);
    gpio_pull_up(PIN_I2C_SDA);
}

void BoardConfig::init() {
    initI2C();

    // Compute the third byte of the IP with a value from
    // the unique board id: 169.254.X.1 (board), 169.254.X.2 (host)
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    shortId = id.id[6];
    snprintf(boardSerialString, 24, "dmxsun_%02x%02x%02x%02x%02x%02x%02x%02x",
        id.id[0],
        id.id[1],
        id.id[2],
        id.id[3],
        id.id[4],
        id.id[5],
        id.id[6],
        id.id[7]
    );

    snprintf(boardHostnameString, 11, "dmxsun_%d",
        shortId
    );

    memset(this->rawData, 0xff, 5*2048);
}

const uint8_t I2C_BASE_ADDR = 0x50; // this is 0x50 HEX, 80 DECIMAL

void BoardConfig::readIOBoards() {
    this->responding[0] = false;
    this->responding[1] = false;
    this->responding[2] = false;
    this->responding[3] = false;

    for (int idx = 0; idx < 4; ++idx) {
        int ret;
        uint8_t buf[3];
        uint8_t src = 0;
        uint8_t i2cAddr = I2C_BASE_ADDR + idx;
        uint16_t eepromAddr = 0;
        // Set EEPROM address to read from
        ret = i2c_write_blocking(i2c0, i2cAddr, (const uint8_t *)&eepromAddr, 2, true);
        // Try to read the EEPROM data
        ret = i2c_read_blocking(i2c0, i2cAddr, rawData[idx], 2048, false);
        if (ret > 0) {
            responding[idx] = true;
            if (this->rawData[idx][0] == 0xff) {
                // EEPROM detected but content (boardType) invalid => yellow LED
                statusLeds.setStatic(idx, 1, 1, 0);
            } else {
                // EEPROM detected and content seems valid => green LED
                statusLeds.setStatic(idx, 0, 1, 0);
            }
        } else {
            // EEPROM not detected :(
            statusLeds.setStatic(idx, 1, 0, 0);
        }
    }
    statusLeds.writeLeds();
}

void BoardConfig::prepareConfig() {
    configData[0] = (ConfigData*)this->rawData[0];
    configData[1] = (ConfigData*)this->rawData[1];
    configData[2] = (ConfigData*)this->rawData[2];
    configData[3] = (ConfigData*)this->rawData[3];
    configData[4] = (ConfigData*)this->rawData[4];

    // Also copy the data from the internal flash to RAM so it can be modified
    memcpy(this->rawData[4], config_flash_contents, 2048);

    // Check if any board is connected and has a valid config
    // All IO boards in order, followed by baseboard
    bool foundConfig = false;
    for (int i = 0; i < 5; i++) {
        if (
            (configData[i]->boardType > BoardType::invalid_00) &&
            (configData[i]->boardType < BoardType::invalid_ff) &&
            (configData[i]->configVersion == CONFIG_VERSION)
        ) {
            BoardConfig::activeConfig = configData[i];
            BoardConfig::configSource = (ConfigSource)i;
            statusLeds.setStatic(i, 0, 0, 1);
            foundConfig = true;
            break;
        }
    }
    createdDefaultConfig = false;
    if (!foundConfig) {
        // We don't have any valid configuration at all :-O
        // Create a default one and use it for now
        // Since we don't know the nature of the IO boards, we save that
        // default config in the slot of the base board!
        *configData[4] = this->defaultConfig();
        createdDefaultConfig = true;
        BoardConfig::activeConfig = configData[4];
        BoardConfig::configSource = ConfigSource::Fallback;
        statusLeds.setStatic(4, 1, 0, 1);
    }
    statusLeds.setBrightness(activeConfig->statusLedBrightness);
    statusLeds.writeLeds();
}

ConfigData BoardConfig::defaultConfig() {
    ConfigData cfg;

    memcpy(&cfg, &constDefaultConfig, sizeof(ConfigData));

    snprintf(cfg.boardName, 32, "! Fallback config v%d!", cfg.configVersion);

    cfg.ownIp = (cfg.ownIp & 0xff00ffff) | ((uint32_t)shortId << 16);
    cfg.hostIp = (cfg.hostIp & 0xff00ffff) | ((uint32_t)shortId << 16);

    snprintf(cfg.wifi_STA_SSID, 32, "YourWifiName");
    snprintf(cfg.wifi_STA_PSK, 32, "YourWifiPass");

    snprintf(cfg.wifi_AP_SSID, 32, "dmxsun_%d PW=dmxsun_pw", shortId);
    snprintf(cfg.wifi_AP_PSK, 32, "dmxsun_pw");
    cfg.wifi_AP_ip = (cfg.wifi_AP_ip & 0xff00ffff) | ((uint32_t)shortId << 16);

    // Patch the first 16 internal DMX buffers to the first 16 physical outputs
    // TODO: Needs to depend on boards connected!
    for (int i = 0; i < 16; i++) {
        cfg.patching[i].active = 1;
        cfg.patching[i].srcType = PatchType::buffer;
        cfg.patching[i].srcInstance = i;
        cfg.patching[i].dstType = PatchType::local;
        cfg.patching[i].dstInstance = i;
        //logPatching("Created initial ", cfg.patching[i]);
    }

    // Patch internal buffers 0 to 3 to the wireless OUTs as well
    // TODO: This needs to depend on the wireless module being present
    for (int i = 0; i < 4; i++) {
        cfg.patching[i+16].active = 1;
        cfg.patching[i+16].srcType = PatchType::buffer;
        cfg.patching[i+16].srcInstance = i;
        cfg.patching[i+16].dstType = PatchType::nrf24;
        cfg.patching[i+16].dstInstance = i;
        //logPatching("Created initial ", cfg.patching[i+16]);
    }

    // Patch the 4 wireless INs to buffers 4 to 7 (= 4 physical ports on second IO board)
    for (int i = 0; i < 4; i++) {
        cfg.patching[i+20].active = 1;
        cfg.patching[i+20].srcType = PatchType::nrf24;
        cfg.patching[i+20].srcInstance = i;
        cfg.patching[i+20].dstType = PatchType::buffer;
        cfg.patching[i+20].dstInstance = i + 4;
        //logPatching("Created initial ", cfg.patching[i+20]);
    }

    return cfg;
}

int BoardConfig::configureBoard(uint8_t slot, struct ConfigData* config) {
    int written = 0;

    LOG("Configure board %u: type: %u", slot, config->boardType);

    // configuring a board only makes sense for the IO boards, not for the baseboard
    if ((slot == 0) || (slot == 1) || (slot == 2) || (slot == 3)) {
        uint8_t i2cAddr = I2C_BASE_ADDR + slot;
        // The I2C EEPROM works with 16-byte pages. The configuration data
        // fits all in one page
        // Since a prepended 16 bit 0 (for the address to write in the EEPROM) is
        // required, do this in a new buffer
        uint8_t bufSize = 2 + ConfigData_ConfigOffset;
        uint8_t buffer[2 + ConfigData_ConfigOffset];
        memset(buffer, 0x00, bufSize);
        memcpy(buffer + 2, config, ConfigData_ConfigOffset);
        written = i2c_write_blocking(i2c0, i2cAddr, buffer, bufSize, false);
        sleep_ms(5); // Give the EEPROM some time to finish the operation
    }
    LOG("BoardConfig written: %u", written);
    return written;
}

int BoardConfig::loadConfig(uint8_t slot) {
    LOG("loading from slot %u. Responding: %u", slot, this->responding[slot]);

    if ((slot == 0) || (slot == 1) || (slot == 2) || (slot == 3)) {
        // Load from an IO board, so check if it's connected
        if (this->responding[slot]) {
            activeConfig = (ConfigData*)this->rawData[slot];
            return 0;
        } else {
            // IO board is not connected
            return 1;
        }
    } else if (slot == 4) {
        // Load from the base board
        activeConfig = (ConfigData*)this->rawData[slot];
        return 0;
    }

    // Slot unknown
    return 4;
}

int BoardConfig::saveConfig(uint8_t slot) {
    ConfigData* targetConfig = (ConfigData*)this->rawData[slot];
    uint16_t bytesToWrite;
    uint8_t bytesWritten;
    uint8_t writeSize;
    int actuallyWritten;
    uint8_t buffer[18];
    int retVal;

    LOG("saveConfig to slot %u. Responding: %u", slot, this->responding[slot]);

    if ((slot == 0) || (slot == 1) || (slot == 2) || (slot == 3)) {
        // Save to an IO board, so check if it's connected and configured
        if (
            (this->responding[slot]) &&
            (targetConfig->boardType > BoardType::invalid_00) &&
            (targetConfig->boardType < BoardType::invalid_ff)
        ) {
            // Save only the non-board-specific part
            uint8_t* dest = (uint8_t*)targetConfig + ConfigData_ConfigOffset;
            uint8_t* src = (uint8_t*)BoardConfig::activeConfig + ConfigData_ConfigOffset;
            uint16_t copySize = sizeof(ConfigData) - ConfigData_ConfigOffset;
            memcpy(dest, src, copySize);
            bytesToWrite = sizeof(struct ConfigData);
            bytesWritten = 0;
            LOG("START bytesToWrite: %u. ConfigVersion: %u", bytesToWrite, targetConfig->configVersion);
            while (bytesToWrite)
            {
                writeSize = bytesToWrite > 16 ? 16 : bytesToWrite;
                memset(buffer, 0x00, sizeof(buffer));
                buffer[0] = bytesWritten >> 8;
                buffer[1] = bytesWritten & 0xff;
                memcpy(buffer + 2, (uint8_t*)targetConfig + bytesWritten, writeSize);
                actuallyWritten = i2c_write_blocking(i2c0, I2C_BASE_ADDR + slot, buffer, writeSize + 2, false);
                if (actuallyWritten < 0) {
                    return 3;
                }
                sleep_ms(5); // Give the EEPROM some time to finish the operation
                LOG("actuallyWritten: %u", actuallyWritten);
                bytesWritten += writeSize;
                bytesToWrite -= writeSize;
                LOG("POST bytesToWrite: %u, writeSize: %u, bytesWritten: %u", bytesToWrite, writeSize, bytesWritten);
            }
            // TODO: Compare after writing
            return 0;
        } else if (!this->responding[slot]) {
            // IO board is not connected
            return 1;
        } else {
            // IO board is connected but not configured
            return 2;
        }
    } else if (slot == 4) {
        // Save to the base board
        memcpy(targetConfig, BoardConfig::activeConfig, sizeof(ConfigData));
        targetConfig->boardType = BoardType::config_only_dongle;

        // We need to make sure core1 is not running when writing to the flash
        multicore_reset_core1();

        // Also disables interrupts
        uint32_t saved = save_and_disable_interrupts();

        // Erase the flash sector
        // Note that a whole number of sectors must be erased at a time.
        flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);

        // Program the flash sector with the new values
        flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t*)targetConfig, sizeof(ConfigData));

        // Compare that what should have been written has been written
        if (memcmp(targetConfig, config_flash_contents, sizeof(ConfigData))) {
            // Comparison failed :-O
            retVal = 3;
        }

        // Restore and enable interrupts
        restore_interrupts(saved);

        // Restart core1
        multicore_launch_core1(core1_tasks);

        // All good :)
        retVal = 0;
        return retVal;
    }

    // Slot unknown
    return 4;
}

int BoardConfig::enableConfig(uint8_t slot) {
    ConfigData* targetConfig = (ConfigData*)this->rawData[slot];
    uint8_t buffer[18];
    int retVal;

    LOG("Enabling config in slot %u. Responding: %u", slot, this->responding[slot]);

    if ((slot == 0) || (slot == 1) || (slot == 2) || (slot == 3)) {
        // Save to an IO board, so check if it's connected and configured
        if (
            (this->responding[slot]) &&
            (targetConfig->boardType > BoardType::invalid_00) &&
            (targetConfig->boardType < BoardType::invalid_ff)
        ) {
            // Save only the non-board-specific part
            buffer[0] = ConfigData_ConfigOffset >> 8; // byte address in the eeprom - high byte
            buffer[1] = ConfigData_ConfigOffset & 0xff; // byte address in the eeprom - low byte
            buffer[2] = CONFIG_VERSION; // configVersion => valid
            i2c_write_blocking(i2c0, I2C_BASE_ADDR + slot, buffer, 3, false);
            sleep_ms(5); // Give the EEPROM some time to finish the operation
            return 0;
        } else if (!this->responding[slot]) {
            // IO board is not connected
            return 1;
        } else {
            // IO board is connected but not configured
            return 2;
        }
    } else if (slot == 4) {
        // Save to the base board
        memcpy(targetConfig, BoardConfig::activeConfig, sizeof(ConfigData));
        targetConfig->configVersion = CONFIG_VERSION; // configVersion => valid

        // We need to make sure core1 is not running when writing to the flash
        multicore_reset_core1();

        // Also disables interrupts
        uint32_t saved = save_and_disable_interrupts();

        // Erase the flash sector
        // Note that a whole number of sectors must be erased at a time.
        flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);

        // Program the flash sector with the new values
        flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t*)targetConfig, sizeof(ConfigData));

        // Compare that what should have been written has been written
        if (memcmp(targetConfig, config_flash_contents, sizeof(ConfigData))) {
            // Comparison failed :-O
            retVal = 3;
        }

        // Restore and enable interrupts
        restore_interrupts(saved);

        // Restart core1
        multicore_launch_core1(core1_tasks);

        // All good :)
        retVal = 0;
        return retVal;
    }

    // Slot unknown
    return 4;
}

int BoardConfig::disableConfig(uint8_t slot) {
    ConfigData* targetConfig = (ConfigData*)this->rawData[slot];
    uint8_t buffer[18];
    int retVal;

    LOG("Disabling config in slot %u. Responding: %u", slot, this->responding[slot]);

    if ((slot == 0) || (slot == 1) || (slot == 2) || (slot == 3)) {
        // Save to an IO board, so check if it's connected and configured
        if (
            (this->responding[slot]) &&
            (targetConfig->boardType > BoardType::invalid_00) &&
            (targetConfig->boardType < BoardType::invalid_ff)
        ) {
            // Save only the non-board-specific part
            buffer[0] = ConfigData_ConfigOffset >> 8; // byte address in the eeprom - high byte
            buffer[1] = ConfigData_ConfigOffset & 0xff; // byte address in the eeprom - low byte
            buffer[2] = 0; // configVersion = 0 => invalid / disabled
            i2c_write_blocking(i2c0, I2C_BASE_ADDR + slot, buffer, 3, false);
            sleep_ms(5); // Give the EEPROM some time to finish the operation
            return 0;
        } else if (!this->responding[slot]) {
            // IO board is not connected
            return 1;
        } else {
            // IO board is connected but not configured
            return 2;
        }
    } else if (slot == 4) {
        // Save to the base board
        memcpy(targetConfig, BoardConfig::activeConfig, sizeof(ConfigData));
        targetConfig->configVersion = 0; // invalid / disabled

        // We need to make sure core1 is not running when writing to the flash
        multicore_reset_core1();

        // Also disables interrupts
        uint32_t saved = save_and_disable_interrupts();

        // Erase the flash sector
        // Note that a whole number of sectors must be erased at a time.
        flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);

        // Program the flash sector with the new values
        flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t*)targetConfig, sizeof(ConfigData));

        // Compare that what should have been written has been written
        if (memcmp(targetConfig, config_flash_contents, sizeof(ConfigData))) {
            // Comparison failed :-O
            retVal = 3;
        }

        // Restore and enable interrupts
        restore_interrupts(saved);

        // Restart core1
        multicore_launch_core1(core1_tasks);

        // All good :)
        retVal = 0;
        return retVal;
    }

    // Slot unknown
    return 4;
}

void BoardConfig::logPatching(const char* prefix, Patching patching) {
    LOG("%s | Patching %d::%d -> %d::%d. Active: %d, EthParamsId: %d",
        prefix,
        patching.srcType,
        patching.srcInstance,
        patching.dstType,
        patching.dstInstance,
        patching.active,
        patching.ethDestParams
    );
}

uint8_t getUsbProtocol() {
    return BoardConfig::activeConfig->usbProtocol;
}

uint8_t getShortId() {
    return BoardConfig::shortId;
}

char* getBoardSerialString() {
    return BoardConfig::boardSerialString;
}

char* getBoardHostnameString() {
    return BoardConfig::boardHostnameString;
}

uint32_t getOwnIp() {
    return BoardConfig::activeConfig->ownIp;
}

uint32_t getOwnMask() {
    return BoardConfig::activeConfig->ownMask;
}

uint32_t getHostIp() {
    return BoardConfig::activeConfig->hostIp;
}
