# Wiring Guide – Analogue Microphone Input

This document describes how to connect a two-wire analogue (electret) microphone to
the Raspberry Pi Pico so that `record_wav_adc()` can capture audio.

---

## Pin assignment

The recording function uses **GPIO 26 (ADC0, physical pin 31)** by default.
This pin does **not** clash with any other peripheral already in use:

| GPIO | Function      | Peripheral |
|------|---------------|------------|
| 8    | SPI1 MISO     | SD card    |
| 9    | SPI1 CS       | SD card    |
| 10   | SPI1 SCK      | SD card    |
| 11   | SPI1 MOSI     | SD card    |
| 18   | PWM audio out | Speaker    |
| **26** | **ADC0 – microphone in** | **Microphone** |

---

## Option A – Microphone amplifier breakout board (recommended)

Pre-built amplifier modules such as the **MAX4466**, **MAX9814**, or **KY-037/KY-038**
already include the bias resistor, decoupling capacitor, and gain stage.  They present
a 0 V – 3.3 V centred output that feeds directly into the Pico ADC.

```
Pico 3V3 (pin 36) ──────── VCC
Pico GND  (pin 38) ──────── GND
Module AOUT        ──────── GPIO 26 / ADC0 (pin 31)
```

No extra components are required.

---

## Option B – Bare electret microphone capsule (2-pin)

A raw electret capsule needs a pull-up resistor (bias) and an AC-coupling
capacitor to correctly drive the ADC input.

### Components required

| Component | Value | Purpose |
|-----------|-------|---------|
| R1        | 10 kΩ | Drain pull-up / current limiter |
| C1        | 100 nF ceramic | AC coupling (blocks DC bias) |
| R2        | 10 kΩ | Voltage-divider top half |
| R3        | 10 kΩ | Voltage-divider bottom half |

> The R2/R3 voltage divider sets the ADC mid-point to ~1.65 V (half of 3.3 V)
> so the microphone signal is centred inside the ADC input range.

### Schematic

```
3V3 (pin 36) ──┬── R1 (10 kΩ) ──┬── MIC+ (capsule drain)
               │                 │
               R2 (10 kΩ)        │  (electret capsule)
               │                 │
               ├── (mid-point)   MIC– (capsule source) ── GND (pin 38)
               │        │
               R3        C1 (100 nF)
               │        │
              GND       GPIO 26 / ADC0 (pin 31)
```

Step-by-step:

1. Connect **3V3** (pin 36) through **R1** (10 kΩ) to the **+** pin of the microphone capsule.
2. Connect the **–** pin of the capsule to **GND** (pin 38).
3. Create a 1.65 V bias by wiring **R2** (10 kΩ) from 3V3 to a mid-point node, and **R3** (10 kΩ) from that same node to GND.
4. Connect the mid-point node through **C1** (100 nF) to **GPIO 26** (pin 31).  The capacitor passes only the AC audio component to the ADC input while blocking the DC bias voltage.

> **Note:** Electret capsules are polarised.  The pin connected to the internal
> FET gate (usually labelled + or marked with a longer leg) is the signal output.

---

## Software usage

```cpp
#include "audio.h"

// Record until stop_recording() is called.
// GPIO 26 = ADC0; sample rate 16 000 Hz; output saved to SD card.
record_wav_adc(pSD, "0:/recordings/guestbook.wav", 26, 16000);

// --- Call stop_recording() from a timer, button ISR, or second core ---
stop_recording();
```

The output file is a standard 16-bit mono PCM WAV that can be played back
with `play_wav_pwm()` or any WAV-capable audio player.
