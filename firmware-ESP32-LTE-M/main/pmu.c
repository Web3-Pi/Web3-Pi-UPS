#include "pmu.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PMU_TAG "pmu"

/* I²C bus pins (board: LilyGo T-SIM7080G-S3) */
#define PMU_I2C_PORT    I2C_NUM_0
#define PMU_I2C_SDA     15
#define PMU_I2C_SCL     7
/* AXP2101 typically supports up to 400 kHz, but for first bring-up we use a
 * slower 100 kHz which is more tolerant of marginal pull-ups / wiring. */
#define PMU_I2C_FREQ_HZ 100000

/* AXP2101 7-bit address */
#define AXP2101_ADDR    0x34

/* AXP2101 registers we touch (from XPowersLib AXP2101Constants.h) */
#define REG_STATUS1            0x00   /* read-only: VBUS / battery presence flags */
#define REG_IC_TYPE            0x03   /* should read 0x4A on AXP2101 */
#define REG_ADC_CHANNEL_CTRL   0x30   /* bit1: TS-pin ADC enable */
#define REG_DC_ONOFF_DVM_CTRL  0x80   /* bit2: DC3 enable */
#define REG_DC_VOL2_CTRL       0x84   /* DC3 voltage code (low 7 bits) */
#define REG_LDO_ONOFF_CTRL0    0x90   /* bit4: BLDO1 enable */
#define REG_LDO_VOL4_CTRL      0x96   /* BLDO1 voltage code (low 5 bits) */

/*
 * DC3 voltage codes — rails 1.6V to 3.4V, 100 mV/step, base index 88:
 *   code = ((mV - 1600) / 100) + 88
 * For 3000 mV → code = 102 = 0x66.
 */
#define DC3_CODE_3V0           0x66
#define DC3_BIT_ENABLE         (1u << 2)

/*
 * BLDO1 voltage codes — 0.5V to 3.5V, 100 mV/step:
 *   code = (mV - 500) / 100
 * For 3300 mV → code = 28 = 0x1C.
 */
#define BLDO1_CODE_3V3         0x1C
#define BLDO1_BIT_ENABLE       (1u << 4)

/* TS-pin ADC enable lives at reg 0x30 bit 1; clear to disable measurement. */
#define TS_PIN_ADC_BIT         (1u << 1)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_axp;

/* -1 timeout = wait forever. The XPowersLib reference port_i2c.cpp uses the
 * same value; AXP2101 transactions are very short (a few hundred µs) so we
 * don't need a tight bound here. */
static esp_err_t pmu_read(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, out, 1, -1);
}

static esp_err_t pmu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_axp, buf, sizeof(buf), -1);
}

static esp_err_t pmu_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    esp_err_t err = pmu_read(reg, &v);
    if (err != ESP_OK) return err;
    return pmu_write(reg, v | mask);
}

static esp_err_t pmu_clr_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    esp_err_t err = pmu_read(reg, &v);
    if (err != ESP_OK) return err;
    return pmu_write(reg, v & (uint8_t)~mask);
}

/* DC3 voltage code is in the low 7 bits of REG_DC_VOL2_CTRL; bit 7 is reserved
 * (DVM-related on some chip variants) — preserve it via read-modify-write. */
static esp_err_t pmu_dc3_set_voltage_code(uint8_t code)
{
    uint8_t v;
    esp_err_t err = pmu_read(REG_DC_VOL2_CTRL, &v);
    if (err != ESP_OK) return err;
    v = (v & 0x80) | (code & 0x7F);
    return pmu_write(REG_DC_VOL2_CTRL, v);
}

esp_err_t pmu_init(void)
{
    /* I²C master bus. We enable internal pull-ups in addition to whatever the
     * board has externally — internal alone (~45 kΩ) is usually too weak,
     * but combined with a missing/marginal external they still help. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = PMU_I2C_PORT,
        .sda_io_num = PMU_I2C_SDA,
        .scl_io_num = PMU_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(PMU_TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(PMU_TAG, "I²C bus ready (SDA=GPIO%d, SCL=GPIO%d)",
             PMU_I2C_SDA, PMU_I2C_SCL);

    /* No bus scan: i2c_master_probe issues a 0-byte write-only transaction
     * that the AXP2101 doesn't ACK, which made the scan log "no devices
     * responded" even when the chip was healthy. The REG_IC_TYPE read below
     * is the real probe — it talks to the chip the same way every other
     * PMU op does, so a failure there means a real wiring/address issue. */

    /* AXP2101 device handle on that bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = PMU_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_axp);
    if (err != ESP_OK) {
        ESP_LOGE(PMU_TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Sanity-probe: read IC type. AXP2101 returns 0x4A in the low byte. */
    uint8_t ic = 0;
    err = pmu_read(REG_IC_TYPE, &ic);
    if (err != ESP_OK) {
        ESP_LOGE(PMU_TAG, "AXP2101 not responding on I²C 0x%02x: %s",
                 AXP2101_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(PMU_TAG, "AXP2101 detected (IC_TYPE reg=0x%02x)", ic);

    /* Disable TS-pin ADC measurement — required for charging without NTC. */
    err = pmu_clr_bits(REG_ADC_CHANNEL_CTRL, TS_PIN_ADC_BIT);
    if (err != ESP_OK) return err;
    ESP_LOGI(PMU_TAG, "TS-pin ADC measurement disabled");

    /* Force a clean DC3 power cycle: disable, wait, set voltage, enable.
     * Without the explicit disable, after an ESP32 chip reset the modem
     * may still have stale internal state from before the reset (DC3 stays
     * on across ESP32 resets, but the modem's PWRKEY state machine can be
     * left half-running). Pulling DC3 low for 200 ms forces a full modem
     * power cycle so the next PWRKEY pulse always triggers a clean boot. */
    err = pmu_clr_bits(REG_DC_ONOFF_DVM_CTRL, DC3_BIT_ENABLE);
    if (err != ESP_OK) return err;
    ESP_LOGI(PMU_TAG, "DC3 disabled — letting modem fully power down (200 ms)");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Set DC3 voltage code first, then enable. */
    err = pmu_dc3_set_voltage_code(DC3_CODE_3V0);
    if (err != ESP_OK) return err;
    err = pmu_set_bits(REG_DC_ONOFF_DVM_CTRL, DC3_BIT_ENABLE);
    if (err != ESP_OK) return err;
    ESP_LOGI(PMU_TAG, "DC3 = 3.0 V, enabled (modem main rail)");

    /* Set BLDO1 to 3.3 V then enable. This is the level-shifter rail —
     * if it's off, ESP32-S3 ↔ SIM7080G UART communication does not work. */
    err = pmu_write(REG_LDO_VOL4_CTRL, BLDO1_CODE_3V3);
    if (err != ESP_OK) return err;
    err = pmu_set_bits(REG_LDO_ONOFF_CTRL0, BLDO1_BIT_ENABLE);
    if (err != ESP_OK) return err;
    ESP_LOGI(PMU_TAG, "BLDO1 = 3.3 V, enabled (level shifter — DO NOT DISABLE)");

    return ESP_OK;
}
