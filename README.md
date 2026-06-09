# LIU Attendance Tracker

An RFID-based attendance tracking system built for the Department of Computer and Information Science (IDA) at Linköping University. The system uses an ESP32-C3-Zero microcontroller and an MFRC522 RFID reader communicating over SPI to map student card UIDs to unique identifiers.

This repository also contains emulation environments (QEMU and Wokwi) used to evaluate cycle count accuracy as part of a bachelor's thesis in computer engineering.

---

## Table of Contents

- [Hardware](#hardware)
- [Prerequisites](#prerequisites)
- [Repository Structure](#repository-structure)
- [Running on Physical Hardware](#running-on-physical-hardware)
- [Running with QEMU Emulation](#running-with-qemu-emulation)
- [Running with Wokwi Emulation](#running-with-wokwi-emulation)
- [Known Limitations](#known-limitations)

---

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | ESP32-C3-Zero (RISC-V, 160 MHz) |
| RFID Reader | MFRC522 (RC522), 13.56 MHz, ISO 14443-A |
| Communication | SPI |

**Wiring (SPI connections from ESP32-C3-Zero to MFRC522):**

| MFRC522 Pin | ESP32-C3-Zero Pin |
|-------------|-------------------|
| VCC | 5V |
| GND | GND |
| RST | GP0 |
| MISO | GP5 |
| MOSI | GP3 |
| SCK | GP4 |
| SDA (CS) | GP7 |

---

## Prerequisites

Install the following tools before proceeding:

- [PlatformIO](https://platformio.org/) — either as a CLI tool or as a VS Code extension
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/) — installed automatically by PlatformIO when targeting ESP32
- [esptool](https://github.com/espressif/esptool) — required for flash image preparation (QEMU only)
- [Espressif's QEMU fork](https://github.com/espressif/qemu) (v9.2.2 or later) — required for QEMU emulation
- [Wokwi VS Code extension](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode) — required for Wokwi emulation (paid license required for local simulation)

---

## Repository Structure

```
LIU-Attendance-Tracker/
├── Attendance-Tracker/   # Firmware source for the physical hardware
├── Emulation/            # Emulation environments
│   ├── qemu/             # QEMU-specific build environment and scripts
│   └── wokwi/            # Wokwi configuration (diagram.json, wokwi.toml)
└── README.md
```

---

## Running on Physical Hardware

### 1. Connect the hardware

Wire the MFRC522 to the ESP32-C3-Zero according to the table above.

### 2. Open the project

Open the `Attendance-Tracker/` directory in VS Code with the PlatformIO extension, or navigate to it in a terminal.

### 3. Build and upload

```bash
pio run -e hardware --target upload
```

PlatformIO will compile the firmware using ESP-IDF and the RISC-V GCC cross-compiler, then flash it to the connected device over USB.

### 4. Monitor serial output

```bash
pio device monitor --baud 115200
```

Cycle counts and UID mappings are logged to the serial output via `ESP_LOGI`.

---

## Running with QEMU Emulation

QEMU emulates the ESP32-C3's RISC-V core but **does not model the SPI peripheral**. The MFRC522 RFID read is therefore replaced by a mock function that injects a fixed byte sequence.

### 1. Build the firmware

```bash
cd Emulation/qemu
~/.platformio/penv/bin/pio run -e qemu
```

This produces three binaries in `.pio/build/qemu/`:
- `bootloader.bin`
- `partitions.bin`
- `firmware.bin`

### 2. Merge the binaries into a flash image

```bash
esptool --chip esp32c3 merge-bin -o merged.bin \
  --flash-mode dio --flash-size 4MB \
  0x0      bootloader.bin \
  0x8000   partitions.bin \
  0x10000  firmware.bin
```

### 3. Pad the image to 4 MiB

QEMU requires a flash image whose size is a power-of-two multiple of 1 MiB. Unwritten regions are filled with `0xFF` to represent erased flash:

```bash
dd if=/dev/zero bs=1M count=4 | tr '\000' '\377' > flash.bin
dd if=merged.bin of=flash.bin bs=1 conv=notrunc
```

### 4. Launch QEMU

```bash
qemu-system-riscv32 \
  -nographic \
  -machine esp32c3 \
  -drive file=flash.bin,if=mtd,format=raw
```

- `-nographic` routes all serial output to your terminal's stdout/stdin.
- `-machine esp32c3` selects the ESP32-C3 board model from Espressif's QEMU fork.
- The flash image is attached via the `mtd` interface, matching the internal SPI flash bus.

QEMU will boot the ESP-IDF second-stage bootloader, load the application from the factory partition at offset `0x10000`, and start executing under FreeRTOS. Cycle counts are printed to the terminal.

> **Note:** The Espressif QEMU fork must be used — upstream QEMU does not include the ESP32-C3 machine model. Build it from [github.com/espressif/qemu](https://github.com/espressif/qemu).

---

## Running with Wokwi Emulation

Wokwi emulates the full system including an MFRC522 model, allowing end-to-end SPI communication in the simulation. It runs as a VS Code extension integrated directly into the PlatformIO project.

### 1. Build the firmware

```bash
cd Emulation/wokwi
~/.platformio/penv/bin/pio run -e esp32-c3-devkitm-1
```

The firmware ELF file is placed at:
```
.pio/build/esp32-c3-devkitm-1/firmware.elf
```

### 2. Configuration files

Two configuration files are required in the project root (already included in `Emulation/wokwi/`):

**`diagram.json`** — defines the hardware components and their connections:
```json
{
  "version": 1,
  "parts": [
    { "type": "board-esp32-c3-devkitm-1", "id": "esp" },
    { "type": "board-mfrc522",            "id": "rfid" }
  ],
  "connections": [
    [ "esp:GND",  "rfid:GND",   "black",  [] ],
    [ "esp:3V3",  "rfid:3.3V",  "red",    [] ],
    [ "esp:3",    "rfid:MOSI",  "blue",   [] ],
    [ "esp:4",    "rfid:SCK",   "yellow", [] ],
    [ "esp:5",    "rfid:MISO",  "green",  [] ],
    [ "esp:6",    "rfid:RST",   "orange", [] ],
    [ "esp:7",    "rfid:SDA",   "purple", [] ]
  ]
}
```

**`wokwi.toml`** — points Wokwi at the compiled firmware:
```toml
[wokwi]
version = 1
firmware = ".pio/build/esp32-c3-devkitm-1/firmware.elf"
```

### 3. Start the simulation

With the Wokwi VS Code extension installed:

1. Open `diagram.json` in VS Code.
2. Click the **Play** button in the Wokwi diagram view, or run **Wokwi: Start Simulator** from the command palette.

Cycle counts are printed to the integrated serial monitor.

> **Note:** Wokwi's MFRC522 model does not support anti-collision beyond CASCADE level 1, meaning it cannot read cards with a 7-byte UID (such as LiU student cards). Testing in Wokwi therefore uses a simulated 4-byte UID card instead.

> **Note:** Wokwi's cycle counter register is hard-coded to increment at 10 MHz regardless of the configured CPU frequency (160 MHz). Raw cycle count readings from Wokwi must be multiplied by **16** to be comparable with physical hardware or QEMU results.

---

## Known Limitations

| Environment | Limitation |
|-------------|-----------|
| **QEMU** | No SPI peripheral support in the Espressif fork; RFID reads are replaced by a mock injection function. Expect ~22–26% cycle count deviation from physical hardware. |
| **Wokwi** | MFRC522 model limited to 4-byte UIDs; cycle counter hard-coded to 10 MHz (apply ×16 correction). Expect ~17–50% cycle count deviation from physical hardware after correction. |
| **Both** | Neither emulator achieves reliable cycle count accuracy for this workload. Do not rely on either for cycle-accurate timing validation. |

For full methodology and results, see the accompanying bachelor's thesis: *Embedded Systems Emulation — An Evaluation of Cycle Count Accuracy* (LIU-IDA/LITH-EX-G--26/028--SE).
