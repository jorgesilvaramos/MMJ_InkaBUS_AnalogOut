# MMJ_InkaBUS_AnalogOut Library for Arduino

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-green.svg)
![Framework](https://img.shields.io/badge/framework-Arduino-orange.svg)

An Arduino library for the Texas Instruments DAC161S997 16-bit SPI DAC, designed for industrial 4–20 mA current-loop outputs.
This driver provides error detection, timeout management, and reliable current scaling for ESP32-based Arduino projects.

---

## Features

- **16-bit resolution** — full 16-bit DAC control
- **4–20 mA industrial output** — designed for standard current-loop systems
- **Error detection** — SPI polling or ERRB GPIO monitoring
- **Timeout management** — built-in keepalive to prevent SPI timeout faults
- **Current scaling** — convert mA to DAC code with range clamping
- **ESP32 compatible** — works with Arduino SPI on ESP32 boards

---

## Hardware Support

### DAC161S997

- Resolution: **16-bit (65536 steps)**
- Output range: **0–23.95 mA**
- Standard analog loop: **4–20 mA**
- Interface: **SPI (up to 10 MHz)**
- Error detection: **ERRB pin (open-drain, active LOW)**
- Timeout window: **configurable, default 100 ms**

---

## Compatibility

This library is intended for:

- ESP32 boards
- Arduino-compatible boards with SPI support

> Note: only ESP32 has been verified during development.

---

## Installation

### Arduino IDE

1. Download or clone this repository.
2. Copy the library folder into your Arduino libraries directory:

   `Documents/Arduino/libraries/MMJ_InkaBUS_AnalogOut`

3. Restart the Arduino IDE.
4. Include the library in your sketch:

```cpp
#include <MMJ_InkaBUS_AnalogOut.h>
```

> Make sure your board supports SPI and call `SPI.begin()` in `setup()`.

---

## Library Structure

```
MMJ_InkaBUS_AnalogOut/
├── src/
│   ├── MMJ_InkaBUS_AnalogOut.h
│   └── MMJ_InkaBUS_AnalogOut.cpp
├── examples/
│   ├── AnalogWrite/
│   └── AnalogWrite_IRQ/
├── library.properties
└── README.md
```

---

## Usage

### Initialization

```cpp
#include <SPI.h>
#include <MMJ_InkaBUS_AnalogOut.h>

#define PIN_CS 13

void setup() {
    Serial.begin(115200);
    SPI.begin();

    if (!InkaBUS_AnalogOutInit(PIN_CS)) {
        Serial.println("DAC not detected!");
        while (true);
    }
}
```

### Writing Current Values

```cpp
void loop() {
    if (!InkaBUS_AnalogWriteMA(12.5f)) {
        uint16_t status = InkaBUS_ReadStatus();

        if (status & DAC161S997_STATUS_CURR_LOOP) {
            Serial.println("Open loop detected!");
        }
    }

    if (InkaBUS_TimeSinceLastWrite() > 50) {
        InkaBUS_Keepalive();
    }

    delay(100);
}
```

---

## API Reference

### Initialization

```cpp
bool InkaBUS_AnalogOutInit(
    uint8_t csPin,
    int8_t errorPin = -1,
    uint32_t clk = 1000000,
    uint8_t mode = SPI_MODE0,
    error_handler_t handler = SPI_POLLING
);
```

Initializes the DAC and verifies communication.

### Output Control

```cpp
bool InkaBUS_AnalogWrite(uint16_t dacCode);
bool InkaBUS_AnalogWriteMA(float mA);
```

- `InkaBUS_AnalogWrite()` writes a raw 16-bit DAC code.
- `InkaBUS_AnalogWriteMA()` writes a current value in milliamps (4.0–20.0 mA).

### Error Handling

```cpp
uint16_t InkaBUS_ReadStatus();
bool     InkaBUS_Keepalive();
uint32_t InkaBUS_TimeSinceLastWrite();
```

- `InkaBUS_ReadStatus()` reads the DAC STATUS register.
- `InkaBUS_Keepalive()` rewrites the last DAC value to prevent timeout.
- `InkaBUS_TimeSinceLastWrite()` returns milliseconds since the last valid write.

### Constants

- `DAC161S997_CODE_4MA`
- `DAC161S997_CODE_20MA`
- `DAC161S997_MA_MIN`
- `DAC161S997_MA_MAX`

### Status Bits

- `DAC161S997_STATUS_CURR_LOOP`
- `DAC161S997_STATUS_LOOP_HIST`
- `DAC161S997_STATUS_SPI_TOUT`
- `DAC161S997_STATUS_FERR`

### Error Handlers

- `SPI_POLLING`
- `IRQ_POLLING`

---

## Examples

Two example sketches are provided:

- `AnalogWrite` — 4–20 mA output with SPI polling error detection
- `AnalogWrite_IRQ` — 4–20 mA output with ERRB GPIO error detection

---

## Dependencies

- Arduino framework
- ESP32 board support
- SPI library

---

## License

This project is released under the MIT License.

---

## Repository

https://github.com/jorgesilvaramos/MMJ_InkaBUS_AnalogOut

---

## Contributing

Contributions, bug reports, and feature requests are welcome.
Please open an issue or submit a pull request on GitHub.
