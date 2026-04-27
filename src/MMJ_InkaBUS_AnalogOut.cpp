#include "MMJ_InkaBUS_AnalogOut.h"

// ============================================================
// Internal driver state (single instance)
// ============================================================
static uint8_t         _cs_pin;
static int8_t          _error_pin;
static error_handler_t _error_handler;
static SPISettings     _spi_cfg;
static uint16_t        _last_daccode  = DAC161S997_CODE_4MA;
static uint32_t        _last_write_ms = 0;

// ============================================================
// Private functions
// ============================================================

/**
 * Sends exactly 24 bits (8-bit command + 16-bit data) in a single SPI
 * transaction. The DAC161S997 requires an exact multiple of 24 SCLK cycles
 * between CS edges; any deviation causes a Frame Error in the device.
 */
static void _write_register(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF)
    };

    SPI.beginTransaction(_spi_cfg);
    digitalWrite(_cs_pin, LOW);
    SPI.transfer(buf, 3);           // Atomic transfer: exactly 24 bits
    digitalWrite(_cs_pin, HIGH);
    SPI.endTransaction();
}

/**
 * Reads an internal DAC register.
 *
 * The DAC161S997 protocol requires TWO separate SPI transactions
 * (datasheet §8.5.1.2):
 *
 *   Transaction 1: cmd = (reg | 0x80), data = 0x0000
 *     → DAC loads the register value into its output FIFO.
 *
 *   Transaction 2: any valid 24-bit command (re-read works fine)
 *     → DAC flushes the FIFO onto SDO; we capture bytes [1] and [2].
 *
 * Each transaction must be exactly 24 bits (3 bytes). Using transfer16()
 * for the second transaction would send only 16 bits and trigger a Frame Error.
 */
static uint16_t _read_register(uint8_t reg)
{
    uint8_t buf[3];

    // Transaction 1: request register read
    buf[0] = reg | 0x80;
    buf[1] = 0x00;
    buf[2] = 0x00;

    SPI.beginTransaction(_spi_cfg);
    digitalWrite(_cs_pin, LOW);
    SPI.transfer(buf, 3);
    digitalWrite(_cs_pin, HIGH);
    SPI.endTransaction();

    // Minimum CS high time (tCSB ≥ 5 ns per datasheet).
    // In most MCUs the digitalWrite overhead is sufficient, but kept
    // explicit for high-frequency environments.
    delayMicroseconds(1);

    // Transaction 2: capture the data
    buf[0] = reg | 0x80;            // Re-read the same register (NOP 0xFF also valid)
    buf[1] = 0x00;
    buf[2] = 0x00;

    SPI.beginTransaction(_spi_cfg);
    digitalWrite(_cs_pin, LOW);
    SPI.transfer(buf, 3);           // SDO returns [cmd_echo][data_hi][data_lo]
    digitalWrite(_cs_pin, HIGH);
    SPI.endTransaction();

    return ((uint16_t)buf[1] << 8) | buf[2];
}

/**
 * Checks whether the DAC is reporting an error, using the configured strategy.
 *
 * SPI_POLLING: reads REG_STATUS — clears the LOOP_HIST bit (bit 1) as a side
 * effect. If the application must preserve that bit, call InkaBUS_ReadStatus()
 * directly before this function.
 *
 * IRQ_POLLING: samples ERRB pin level (open-drain, active LOW). Does not
 * affect any STATUS bits.
 */
static bool _check_error()
{
    switch (_error_handler)
    {
        case SPI_POLLING:
        {
            uint16_t status = _read_register(DAC161S997_REG_STATUS);
            return (status & DAC161S997_STATUS_ALL_ERRORS) != 0;
        }

        case IRQ_POLLING:
        {
            if (_error_pin < 0)
                return false;                        // Pin not configured → assume no error
            return (digitalRead(_error_pin) == LOW); // ERRB asserts LOW (open-drain)
        }

        default:
            return false;
    }
}

// ============================================================
// Public API
// ============================================================

bool InkaBUS_AnalogOutInit(
    uint8_t         csPin,
    int8_t          errorPin,
    uint32_t        clk_spi_bus,
    uint8_t         spi_mode,
    error_handler_t error_handler
)
{
    _cs_pin        = csPin;
    _error_pin     = errorPin;
    _error_handler = error_handler;
    _spi_cfg       = SPISettings(clk_spi_bus, MSBFIRST, spi_mode);

    // Configure GPIO pins
    pinMode(_cs_pin, OUTPUT);
    digitalWrite(_cs_pin, HIGH);    // CS idle (inactive)

    if (_error_pin >= 0)
        pinMode(_error_pin, INPUT); // ERRB is open-drain; use INPUT_PULLUP if no external pull-up

    // Wait for the DAC to complete its internal power-up sequence.
    // Datasheet recommends > 10 ms; we use 15 ms for margin.
    delay(15);

    // Presence check: read ERR_CONFIG (0x05).
    // Reset value per datasheet (§8.6, Table 3): 0x5004.
    // 0x0000 or 0xFFFF indicates a floating bus or absent device.
    uint16_t err_cfg = _read_register(DAC161S997_REG_ERR_CONFIG);

    if (err_cfg == 0x0000 || err_cfg == 0xFFFF)
        return false;

    // Set initial output to 4 mA (the lower end of the 4–20 mA standard).
    // DACCODE = 0x0000 is NOT 4 mA — it corresponds to ~0 mA, outside the
    // standard window and with unspecified linearity.
    _write_register(DAC161S997_REG_DACCODE, DAC161S997_CODE_4MA);

    return true;
}

bool InkaBUS_AnalogWrite(uint16_t dataOut)
{
    _write_register(DAC161S997_REG_DACCODE, dataOut);
    _last_daccode  = dataOut;
    _last_write_ms = millis();

    return !_check_error();
}

bool InkaBUS_AnalogWriteMA(float mA)
{
    if (mA < DAC161S997_MA_MIN) mA = DAC161S997_MA_MIN;
    if (mA > DAC161S997_MA_MAX) mA = DAC161S997_MA_MAX;

    const float span_code = (float)(DAC161S997_CODE_20MA - DAC161S997_CODE_4MA);
    const float span_ma   = DAC161S997_MA_MAX - DAC161S997_MA_MIN;
    float normalized      = (mA - DAC161S997_MA_MIN) / span_ma;
    uint16_t code         = (uint16_t)((float)DAC161S997_CODE_4MA + normalized * span_code + 0.5f);

    return InkaBUS_AnalogWrite(code);
}

bool InkaBUS_Keepalive()
{
    _write_register(DAC161S997_REG_DACCODE, _last_daccode);
    _last_write_ms = millis();

    return !_check_error();
}

uint32_t InkaBUS_TimeSinceLastWrite()
{
    return millis() - _last_write_ms;
}

uint16_t InkaBUS_ReadStatus()
{
    return _read_register(DAC161S997_REG_STATUS);
}
