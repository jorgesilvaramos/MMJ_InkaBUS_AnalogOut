/**
 * @file    AnalogWrite_IRQ_Polling.ino
 * @brief   DAC161S997 — 4–20 mA output with error detection via IRQ_POLLING.
 *
 * ─── Mode: IRQ_POLLING ───────────────────────────────────────────────────────
 *
 *  The driver samples the ERRB pin level with digitalRead() after each
 *  write. ERRB is open-drain and active LOW: it asserts when the DAC
 *  detects a loop error, SPI timeout, or frame error.
 *
 *  Advantage:   No additional SPI transactions for error detection.
 *  Limitation: Requires a GPIO connected to ERRB with pull-up (internal or external).
 *              Does not clear the LOOP_HIST bit in the STATUS register; to read it
 *              explicitly use InkaBUS_ReadStatus().
 *
 * ─── Hardware ────────────────────────────────────────────────────────────────
 *
 *  MCU (ESP32)    DAC161S997
 *  ───────────────────────────
 *  GPIO 17 (CLK)  → SCLK
 *  GPIO  4 (MOSI) → SDI
 *  GPIO 16 (MISO) → SDO
 *  GPIO 13 (CS)   → /CS
 *  GPIO 14 (ERR)  → ERRB  ← needs pull-up to VCC (or INPUT_PULLUP on MCU)
 *  GPIO 15        → Strapping pin — pull LOW to release SPI pins
 *
 * ─── Behavior ────────────────────────────────────────────────────────────────
 *
 *  • Increases the current from 4 mA to 20 mA in 1 mA steps every 5 seconds.
 *  • InkaBUS_AnalogWriteMA() is called every 50 ms — it also acts as keepalive.
 *  • If ERRB asserts (pin LOW), prints STATUS in hexadecimal for diagnosis.
 */

#include "MMJ_InkaBUS_AnalogOut.h"

// ─── Pin assignments ──────────────────────────────────────────────────────────
#define PIN_SCK         17  // SPI clock
#define PIN_MOSI         4  // SPI data out (MCU → DAC)
#define PIN_MISO        16  // SPI data in  (DAC → MCU)
#define PIN_CS          13  // Chip select (active LOW)
#define PIN_ERRB        14  // DAC ERRB output (open-drain, active LOW)
#define PIN_STRAPPING   15  // Allows reading ERRB pin by keeping it LOW (strapping) — do not use for other purposes, specific to InkaLogic Basic

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_STRAPPING, OUTPUT);
    digitalWrite(PIN_STRAPPING, LOW);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

    // Initialize with IRQ_POLLING: the driver samples PIN_ERRB on each write.
    if (!InkaBUS_AnalogOutInit(
            PIN_CS,
            PIN_ERRB,        // ERRB pin connected → detection without extra SPI
            1000000,         // 1 MHz — safe for most industrial cables
            SPI_MODE0,
            IRQ_POLLING))
    {
        Serial.println("[ERROR] DAC161S997 not responding. Check wiring.");
        while (true);
    }

    Serial.println("[OK] DAC161S997 ready — IRQ_POLLING mode");
}

// ─── Main loop ───────────────────────────────────────────────────────────────

static uint32_t lastStepMs = 0;
static float    outputMA   = 4.0f;

void loop()
{
    // Advance 1 mA every 5 seconds; return to 4 mA after exceeding 20 mA
    if (millis() - lastStepMs >= 5000) {
        lastStepMs = millis();
        outputMA  += 1.0f;
        if (outputMA > DAC161S997_MA_MAX) outputMA = DAC161S997_MA_MIN;
        Serial.print("[OUT] "); Serial.print(outputMA, 1); Serial.println(" mA");
    }

    // Write current every 50 ms — also maintains SPI Timeout timer.
    // If ERRB is LOW (active error), _check_error() detects it without extra SPI.
    if (!InkaBUS_AnalogWriteMA(outputMA)) {
        // ERRB asserted: read STATUS for more detailed diagnosis
        uint16_t st = InkaBUS_ReadStatus();
        Serial.print("[ERROR] ERRB asserted — STATUS = 0x"); Serial.println(st, HEX);

        if (st & DAC161S997_STATUS_CURR_LOOP) Serial.println("  → Open loop (current < threshold)");
        if (st & DAC161S997_STATUS_SPI_TOUT)  Serial.println("  → SPI Timeout");
        if (st & DAC161S997_STATUS_FERR)      Serial.println("  → SPI Frame Error");
    }

    delay(50);
}
