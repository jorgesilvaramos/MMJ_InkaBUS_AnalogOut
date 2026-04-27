/**
 * @file    AnalogWrite_Polling.ino
 * @brief   DAC161S997 — 4–20 mA output with error detection via SPI_POLLING.
 *
 * ─── Mode: SPI_POLLING ────────────────────────────────────────────────────────
 *
 *  The driver reads the STATUS register from the DAC over SPI after each write.
 *  No connection to the ERRB pin is required on the microcontroller.
 *
 *  Advantage:   Works without additional GPIO.
 *  Limitation: Adds one extra SPI transaction per call to
 *              InkaBUS_AnalogWrite() / InkaBUS_AnalogWriteMA().
 *
 * ─── Hardware ────────────────────────────────────────────────────────────────
 *
 *  MCU (ESP32)    DAC161S997
 *  ───────────────────────────
 *  GPIO 17 (CLK)  → SCLK
 *  GPIO  4 (MOSI) → SDI
 *  GPIO 16 (MISO) → SDO
 *  GPIO 13 (CS)   → /CS
 *
 *  ERRB: not connected in this example (error_handler = SPI_POLLING)
 *
 * ─── Behavior ────────────────────────────────────────────────────────────────
 *
 *  • Increases the current from 4 mA to 20 mA in 1 mA steps every second.
 *  • InkaBUS_AnalogWriteMA() is called every 50 ms — it also acts as keepalive
 *    (write period < timeout of 100 ms; InkaBUS_Keepalive() is not necessary).
 *  • If an error is detected, prints the STATUS register in hexadecimal.
 */
 
#include <MMJ_InkaBUS_AnalogOut.h>

// ─── Pin assignments ──────────────────────────────────────────────────────────
#define PIN_SCK         17  // SPI clock
#define PIN_MOSI         4  // SPI data out (MCU → DAC)
#define PIN_MISO        16  // SPI data in  (DAC → MCU)
#define PIN_CS          13  // Chip select (active LOW)

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

    // Initialize with SPI_POLLING (no ERRB pin, no extra arguments).
    if (!InkaBUS_AnalogOutInit(PIN_CS)) {
        Serial.println("[ERROR] DAC161S997 not responding. Check wiring.");
        while (true);   // Stop execution — no DAC, no safe output
    }

    Serial.println("[OK] DAC161S997 ready — SPI_POLLING mode");
}

// ─── Main loop ───────────────────────────────────────────────────────────────

static uint32_t lastStepMs = 0;
static float    outputMA   = 4.0f;

void loop()
{
    // Advance 1 mA every second; return to 4 mA after exceeding 20 mA
    if (millis() - lastStepMs >= 1000) {
        lastStepMs = millis();
        outputMA  += 1.0f;
        if (outputMA > DAC161S997_MA_MAX) outputMA = DAC161S997_MA_MIN;
        Serial.print("[OUT] "); Serial.print(outputMA, 1); Serial.println(" mA");
    }

    // Write current every 50 ms — also maintains SPI Timeout < 100ms
    if (!InkaBUS_AnalogWriteMA(outputMA)) {
        uint16_t st = InkaBUS_ReadStatus();
        Serial.print("[ERROR] STATUS = 0x"); Serial.println(st, HEX);
        if (st & DAC161S997_STATUS_CURR_LOOP) Serial.println("  → Open loop (current < threshold)");
        if (st & DAC161S997_STATUS_SPI_TOUT)  Serial.println("  → SPI Timeout");
        if (st & DAC161S997_STATUS_FERR)      Serial.println("  → SPI Frame Error");
    }

    delay(50);
}
