#pragma once

/*
 * MMJ_InkaBUS_AnalogOut.h
 *
 *  Author: Jorge A. Silva
 *
 * ─── Description ─────────────────────────────────────────────────────────────
 *
 *  Driver for the DAC161S997 16-bit SPI DAC used in Inka analog output modules.
 *  Produces a standard 4–20 mA industrial current-loop signal.
 *
 *  Two error-detection strategies are supported:
 *
 *    • SPI_POLLING  — reads the STATUS register over SPI after each write.
 *                     Requires no extra GPIO; adds one SPI transaction per write.
 *    • IRQ_POLLING  — samples the ERRB pin (open-drain, active LOW) in software.
 *                     Zero SPI overhead for error detection; requires one GPIO.
 *
 * ─── Signal Conditioning ─────────────────────────────────────────────────────
 *
 *  The DAC161S997 drives the 4–20 mA loop directly. Its full digital range
 *  (0x0000–0xFFFF) covers 0–23.95 mA. The standard 4–20 mA window maps to:
 *
 *  ┌──────────┬────────────┬─────────────────────────────────────────────────┐
 *  │ Current  │ DACCODE    │ Note                                            │
 *  ├──────────┼────────────┼─────────────────────────────────────────────────┤
 *  │  4.0 mA  │ 0x2AAA     │ Lower end of standard range (1/6 of FSR)       │
 *  │ 20.0 mA  │ 0xD555     │ Upper end of standard range (5/6 of FSR)       │
 *  └──────────┴────────────┴─────────────────────────────────────────────────┘
 *
 *  Formula: I_out = (DACCODE / 65536) × 24 mA
 *  INL is only guaranteed within 0x2AAA ≤ DACCODE ≤ 0xD555.
 *
 * ─── SPI Protocol ────────────────────────────────────────────────────────────
 *
 *  The DAC161S997 uses a fixed 24-bit frame: 8-bit command + 16-bit data.
 *  Each CS assertion must span exactly 24 SCLK cycles. Shorter or longer
 *  frames cause a Frame Error (FERR) in the device.
 *
 *  Supported modes: SPI_MODE0 and SPI_MODE3, MSBFIRST, up to 10 MHz.
 *
 * ─── SPI Timeout ─────────────────────────────────────────────────────────────
 *
 *  The DAC161S997 expects a valid register write within the period configured
 *  in ERR_CONFIG (default: 100 ms). If no write arrives in time, the device
 *  forces the output to the programmed error current and asserts ERRB.
 *
 *  A write to address 0x00 (NOP) does NOT reset the timer (datasheet §8.3.1.2).
 *  Use InkaBUS_Keepalive() to periodically rewrite the last DACCODE. Recommended
 *  call period: ≤ 50 ms when using the default 100 ms timeout.
 *
 * ─── Dependencies ────────────────────────────────────────────────────────────
 *
 *  Arduino.h, SPI.h
 *
 * ─── Thread safety ───────────────────────────────────────────────────────────
 *
 *  This driver is NOT re-entrant. Do NOT call its functions from multiple
 *  RTOS tasks or from an ISR simultaneously without an external mutex.
 */

#include <Arduino.h>
#include <SPI.h>

// ─── Register Addresses ──────────────────────────────────────────────────────
// (DAC161S997 datasheet §8.6)
#define DAC161S997_REG_NOP          0x00  // No Operation — NOT valid for SPI timeout reset
#define DAC161S997_REG_XFER         0x01  // Transfer (protected SPI write latch)
#define DAC161S997_REG_NOP2         0x02  // Alternate NOP
#define DAC161S997_REG_WR_MODE      0x03  // Write-protection mode (PROTECT_REG_WR)
#define DAC161S997_REG_DACCODE      0x04  // DAC output code (R/W)
#define DAC161S997_REG_ERR_CONFIG   0x05  // Error configuration (R/W) — reset value: 0x5004
#define DAC161S997_REG_ERR_LOW      0x06  // Low-side error current (R/W)
#define DAC161S997_REG_ERR_HIGH     0x07  // High-side error current (R/W)
#define DAC161S997_REG_RESET        0x08  // Device reset (W)
#define DAC161S997_REG_STATUS       0x09  // Status flags (R/O)

// ─── STATUS Register Bits (0x09) ─────────────────────────────────────────────
#define DAC161S997_STATUS_CURR_LOOP (1U << 0)  // Current loop error (active fault)
#define DAC161S997_STATUS_LOOP_HIST (1U << 1)  // Loop error history — cleared on read
#define DAC161S997_STATUS_SPI_TOUT  (1U << 2)  // SPI timeout error
#define DAC161S997_STATUS_FERR      (1U << 3)  // SPI frame error (wrong bit count)

// Mask of all error bits in STATUS
#define DAC161S997_STATUS_ALL_ERRORS \
    (DAC161S997_STATUS_CURR_LOOP | DAC161S997_STATUS_LOOP_HIST | \
     DAC161S997_STATUS_SPI_TOUT  | DAC161S997_STATUS_FERR)

// ─── DACCODE ↔ Current Mapping ───────────────────────────────────────────────
// (DAC161S997 datasheet §7.5)
//
//  Full range (0x0000–0xFFFF) → 0–23.95 mA  (I = DACCODE / 65536 × 24 mA)
//  Standard 4–20 mA window:
//
//    4  mA → 0x2AAA  = 10922  (1/6 of full scale)
//   20  mA → 0xD555  = 54613  (5/6 of full scale)
//
//  INL is guaranteed by the datasheet only within 0x2AAA ≤ CODE ≤ 0xD555.
#define DAC161S997_CODE_4MA     0x2AAA  // DACCODE for  4 mA (lower standard limit)
#define DAC161S997_CODE_20MA    0xD555  // DACCODE for 20 mA (upper standard limit)

#define DAC161S997_MA_MIN       4.0f    // Minimum current — standard 4–20 mA
#define DAC161S997_MA_MAX       20.0f   // Maximum current — standard 4–20 mA

// ─── Error Detection Strategy ────────────────────────────────────────────────

/**
 * Selects how the driver detects faults after each DAC write.
 *
 *  SPI_POLLING  Reads STATUS register (0x09) over SPI after every write.
 *               Does not require a dedicated GPIO pin (errorPin = -1 is valid).
 *               Adds one extra SPI round-trip per InkaBUS_AnalogWrite() call.
 *               Reading STATUS clears the LOOP_STS history bit (bit 1).
 *
 *  IRQ_POLLING  Samples the ERRB GPIO directly with digitalRead().
 *               No additional SPI traffic; errorPin must be connected.
 *               ERRB is open-drain, active LOW — configure with INPUT_PULLUP
 *               or an external pull-up resistor to VCC.
 *               Does NOT clear any STATUS bits.
 */
typedef enum
{
    SPI_POLLING,    ///< Poll STATUS register via SPI (no GPIO required)
    IRQ_POLLING     ///< Poll ERRB pin via digitalRead() (requires errorPin)
} error_handler_t;

// ─── API ─────────────────────────────────────────────────────────────────────

/**
 * @brief  Initialize the DAC161S997 and verify SPI communication.
 *
 *  Configures the CS and ERRB GPIO pins, starts the SPI bus, waits for the
 *  device power-up sequence to complete (≥ 10 ms), then performs a presence
 *  check by reading ERR_CONFIG (reset value: 0x5004). If the register returns
 *  0x0000 or 0xFFFF the bus is considered floating or the device absent.
 *
 *  On success, the output is set to 4 mA (DACCODE = 0x2AAA) as a safe
 *  initial state. DACCODE = 0x0000 is NOT 4 mA — it corresponds to ~0 mA,
 *  outside the 4–20 mA standard window.
 *
 * @param  csPin          GPIO connected to the DAC CS pin (active LOW).
 * @param  errorPin       GPIO connected to ERRB (-1 = not used).
 *                        Required for IRQ_POLLING; ignored by SPI_POLLING.
 *                        ERRB is open-drain — use INPUT_PULLUP or an
 *                        external resistor.
 * @param  clk_spi_bus    SPI clock frequency in Hz. Maximum: 10 MHz (per datasheet).
 *                        Default: 1 000 000 Hz (1 MHz).
 * @param  spi_mode       SPI mode: SPI_MODE0 or SPI_MODE3.
 *                        Default: SPI_MODE0.
 * @param  error_handler  Fault-detection strategy. Default: SPI_POLLING.
 *
 * @return true   DAC responded correctly; output is driving 4 mA.
 * @return false  DAC not detected or SPI bus problem; check wiring.
 *
 * @par Example — SPI_POLLING (no ERRB pin)
 * @code
 *   SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
 *   if (!InkaBUS_AnalogOutInit(CS_PIN)) {
 *       Serial.println("DAC not found");
 *       while (true);
 *   }
 * @endcode
 *
 * @par Example — IRQ_POLLING (ERRB pin connected)
 * @code
 *   SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
 *   if (!InkaBUS_AnalogOutInit(CS_PIN, ERROR_PIN, 1000000, SPI_MODE0, IRQ_POLLING)) {
 *       Serial.println("DAC not found");
 *       while (true);
 *   }
 * @endcode
 */
bool InkaBUS_AnalogOutInit(
    uint8_t         csPin,
    int8_t          errorPin       = -1,
    uint32_t        clk_spi_bus    = 1000000,
    uint8_t         spi_mode       = SPI_MODE0,
    error_handler_t error_handler  = SPI_POLLING
);

/**
 * @brief  Write a raw 16-bit DACCODE to the output register.
 *
 *  Sends the value directly to REG_DACCODE (0x04) without scaling.
 *  After the write, checks for errors using the configured strategy.
 *
 *  The DAC accepts the full range 0x0000–0xFFFF (0–23.95 mA), but INL is
 *  only guaranteed within 0x2AAA (4 mA) to 0xD555 (20 mA).
 *
 * @param  dataOut  16-bit DAC code. Recommended range: DAC161S997_CODE_4MA
 *                  to DAC161S997_CODE_20MA.
 *
 * @return true   Write succeeded; no errors detected.
 * @return false  Error detected via STATUS register or ERRB pin.
 */
bool InkaBUS_AnalogWrite(uint16_t dataOut);

/**
 * @brief  Scale a current in mA to a DACCODE and write it to the DAC.
 *
 *  Linearly maps the 4–20 mA range to DACCODE values 0x2AAA–0xD555 using
 *  the calibration endpoints from the datasheet (§7.5). Values outside the
 *  4–20 mA range are clamped before conversion; the clamped value is sent,
 *  and the return value reflects communication or loop errors only.
 *
 *  Mapping formula:
 *  @code
 *    code = CODE_4MA + (mA - 4.0) / 16.0 × (CODE_20MA - CODE_4MA)
 *  @endcode
 *
 * @param  mA  Desired output current in milliamps (4.0–20.0).
 *             Values below 4.0 are clamped to 4.0 mA.
 *             Values above 20.0 are clamped to 20.0 mA.
 *
 * @return true   Write succeeded; no errors detected.
 * @return false  Communication or loop error detected.
 *
 * @par Example
 * @code
 *   if (!InkaBUS_AnalogWriteMA(12.0f)) {
 *       uint16_t st = InkaBUS_ReadStatus();
 *       Serial.print("STATUS: 0x"); Serial.println(st, HEX);
 *   }
 * @endcode
 */
bool InkaBUS_AnalogWriteMA(float mA);

/**
 * @brief  Keepalive: rewrite the last DACCODE to prevent a SPI Timeout Error.
 *
 *  The DAC161S997 requires a valid register write within the window configured
 *  in ERR_CONFIG (default: 100 ms). If no write occurs, the device forces the
 *  output to the programmed error current and asserts ERRB.
 *
 *  A write to address 0x00 (NOP) is explicitly NOT valid for resetting the
 *  timeout counter (datasheet §8.3.1.2). This function reissues the last
 *  DACCODE written by InkaBUS_AnalogWrite() or InkaBUS_AnalogWriteMA(), which is
 *  always a valid write to REG_DACCODE (0x04).
 *
 *  Call this function periodically from the main loop whenever
 *  InkaBUS_AnalogWrite() or InkaBUS_AnalogWriteMA() is not called at least every
 *  50 ms (half the default 100 ms timeout, as a safety margin).
 *
 *  If the application writes to the DAC more frequently than the timeout
 *  (e.g., a fast PID loop), this function is not needed.
 *
 * @return true   Keepalive write succeeded; no errors detected.
 * @return false  Error detected after the write.
 *
 * @par Typical usage
 * @code
 *   void loop() {
 *       // ... other logic ...
 *       if (InkaBUS_TimeSinceLastWrite() >= 50) {
 *           InkaBUS_Keepalive();
 *       }
 *   }
 * @endcode
 */
bool InkaBUS_Keepalive();

/**
 * @brief  Returns the elapsed time in ms since the last valid DAC write.
 *
 *  The timer is reset by any successful call to InkaBUS_AnalogWrite(),
 *  InkaBUS_AnalogWriteMA(), or InkaBUS_Keepalive().
 *
 *  Useful for deciding when to call InkaBUS_Keepalive() without maintaining a
 *  separate timer in application code, or for diagnosing whether the system
 *  is approaching the SPI Timeout threshold.
 *
 * @return Milliseconds elapsed since the last valid write.
 */
uint32_t InkaBUS_TimeSinceLastWrite();

/**
 * @brief  Read the raw STATUS register (0x09) from the DAC.
 *
 *  Returns the current value of all status and error flags. Check against
 *  the DAC161S997_STATUS_* bitmasks defined in this header.
 *
 *  @warning Reading STATUS clears the LOOP_STS history bit (bit 1,
 *           DAC161S997_STATUS_LOOP_HIST). Do not call this function if
 *           the application needs to preserve that bit for later inspection.
 *
 * @return 16-bit STATUS register value.
 *         Bits: [3] FERR | [2] SPI_TOUT | [1] LOOP_HIST | [0] CURR_LOOP
 *         0xFFFF indicates a bus or communication failure.
 *
 * @par Example
 * @code
 *   uint16_t st = InkaBUS_ReadStatus();
 *   if (st & DAC161S997_STATUS_SPI_TOUT) Serial.println("SPI timeout!");
 *   if (st & DAC161S997_STATUS_CURR_LOOP) Serial.println("Loop open!");
 * @endcode
 */
uint16_t InkaBUS_ReadStatus();
