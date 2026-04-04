# Wiring Guide

This document covers all hardware connections required by the audio guestbook:
the SD card reader, the speaker/amplifier output, and the analogue microphone input.

---

## Pin summary

| GPIO | Physical pin | Function             | Connected to          |
|------|-------------|----------------------|-----------------------|
| 8    | 11          | SPI1 MISO            | SD card DAT0          |
| 9    | 12          | SPI1 CS (SS)         | SD card CS            |
| 10   | 14          | SPI1 SCK             | SD card CLK           |
| 11   | 15          | SPI1 MOSI            | SD card CMD/DI        |
| 18   | 24          | PWM audio out        | Speaker / amplifier   |
| 26   | 31          | ADC0 – microphone in | Microphone            |

---

## 1 – SD card reader (SPI1)

A standard SPI-interface micro-SD breakout board is required.  Power the module
from the Pico's **3V3** rail (pin 36); do **not** use VSYS/5V — most breakout boards
have an on-board 3.3 V regulator that will conflict.

```
Pico 3V3  (pin 36) ──── VCC   (SD module)
Pico GND  (pin 38) ──── GND   (SD module)
GPIO  8   (pin 11) ──── MISO  (SD module DAT0)
GPIO  9   (pin 12) ──── CS    (SD module CS)
GPIO 10   (pin 14) ──── SCK   (SD module CLK)
GPIO 11   (pin 15) ──── MOSI  (SD module CMD/DI)
```

> Many cheap SD breakout boards include 10 kΩ pull-ups on MISO/MOSI/SCK.
> These are compatible with the Pico and no additional pull-ups are needed.

---

## 2 – Speaker / audio output (GPIO 18, PWM)

The Pico outputs a 12-bit PWM signal (~30.5 kHz carrier) on **GPIO 18**.  An RC
low-pass filter removes the carrier and leaves only the audio component.

```
GPIO 18 (pin 24) ──┤ 1 kΩ ├──┬──────────── AUDIO IN (+)  (amplifier or speaker)
                              │
                             ═╧═  47–100 nF  (ceramic capacitor)
                             ═╤═
                              │
GND     (pin 38) ─────────────┴──────────── AUDIO IN (−) / GND
```

For best results feed the filtered output into a small class-D amplifier module
(e.g. MAX98357, PAM8302) before connecting a speaker.  A raw 8 Ω speaker can be
connected directly for monitoring but will be very quiet.

---

## 3 – Analogue microphone input (GPIO 26, ADC0)

`record_wav_adc()` reads from **GPIO 26 (ADC0, pin 31)** by default.

### Option A – Microphone amplifier breakout board (recommended)

Pre-built amplifier modules such as the **MAX4466**, **MAX9814**, or **KY-037/KY-038**
include the bias resistor, decoupling capacitor, and gain stage.  They present a
0 V – 3.3 V centred output that feeds directly into the Pico ADC.

```
Pico 3V3  (pin 36) ──── VCC
Pico GND  (pin 38) ──── GND
Module AOUT         ──── GPIO 26 / ADC0  (pin 31)
```

No extra components are required.

### Option B – Bare electret microphone capsule (2-pin)

A raw electret capsule needs a pull-up resistor (bias) and an AC-coupling
capacitor to correctly drive the ADC input.

| Component | Value    | Purpose                                   |
|-----------|----------|-------------------------------------------|
| R1        | 10 kΩ    | Drain pull-up / bias current limiter      |
| R2        | 10 kΩ    | Voltage-divider top half (sets mid-point) |
| R3        | 10 kΩ    | Voltage-divider bottom half               |
| C1        | 100 nF   | AC coupling (blocks DC bias from ADC pin) |

> R2/R3 create a 1.65 V mid-point so the microphone signal is centred in the
> ADC input range (0–3.3 V).

```
3V3 (pin 36) ──┬── R1 (10 kΩ) ──── MIC+  (capsule drain / signal pin)
               │
               R2 (10 kΩ)
               │
               ├── (mid-point node) ──── C1 (100 nF) ──── GPIO 26 (pin 31)
               │
               R3 (10 kΩ)
               │
              GND                        MIC−  (capsule source) ──── GND (pin 38)
```

Step-by-step:

1. Connect **3V3** through **R1** to the **+** (signal) pin of the capsule.
2. Connect the capsule **−** pin to **GND**.
3. Wire **R2** from 3V3 to a mid-point node, and **R3** from that node to GND.
4. Connect the mid-point through **C1** (100 nF) to **GPIO 26**.  The capacitor
   passes only the AC audio component to the ADC while blocking the DC bias.

> Electret capsules are polarised.  The pin connected to the internal FET gate
> (usually the longer leg or the one marked +) is the signal output.

---

## Software usage

```cpp
#include "audio.h"

// Record for 5 seconds from GPIO 26 (ADC0) at 16 000 Hz, save to SD card.
add_alarm_in_ms(5000, [](alarm_id_t, void*) -> int64_t {
    stop_recording(); return 0;
}, nullptr, true);
record_wav_adc("0:/recordings/guestbook.wav", 26, 16000);

// Play back the recording through GPIO 18 (PWM).
WavFile wav = /* load from SD */;
play_wav_pwm(wav, 18);
```

