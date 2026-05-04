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
| 22   | 29          | Phone hook switch    | Hook switch           |
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

## 3 – Analogue microphone input (GPIO 26, ADC0) (with anti-aliasing / noise filter)

`record_wav_adc()` reads from **GPIO 26 (ADC0, pin 31)** by default.

The build is designed around the **MAX4466 electret microphone amplifier**
module.  It already includes the electret capsule, biasing, and gain stage, and
provides an analogue output centred near half the supply voltage for direct ADC use.

Adding a simple first-order RC low-pass filter between the MAX4466 output and
GPIO 26 provides two benefits

1. **Anti-aliasing** – any signal above the Nyquist frequency (8 kHz at a 16 kHz
   sample rate) is attenuated *before* it reaches the ADC.  Once noise aliases
   into the audio band during sampling it cannot be removed in software.
2. **ADC charge-injection reduction** – the RP2040's ADC has a switched-capacitor
   input that briefly draws charge from the source during each conversion.  A
   series resistor limits the resulting glitch and the shunt capacitor helps
   absorb it, reducing the broadband noise floor.

Use **R = 1 kΩ** and **C = 10 nF** (ceramic) for a −3 dB cutoff of ≈ 15.9 kHz.
This follows the RP2040 hardware design guidelines, passes the full voice band
(300–3400 Hz), and provides meaningful attenuation of ADC switching noise at
higher frequencies.  If you want more aggressive HF rolloff you can increase C
to 47 nF (≈ 3.4 kHz cutoff) – suitable for telephone-quality speech recording
but will slightly soften the top of the voice band.

```
Pico 3V3  (pin 36) ──── MAX4466 VCC
Pico GND  (pin 38) ──── MAX4466 GND

MAX4466 OUT ──┤ 1 kΩ ├──┬──── GPIO 26 / ADC0  (pin 31)
                        │
                       ═╧═  10 nF  (ceramic, e.g. 0.01 µF)
                       ═╤═
                        │
Pico GND  (pin 38) ─────┘
```

> Set the MAX4466 gain trimmer low-to-mid to start, then increase gradually
> while monitoring recordings to avoid clipping.

> The firmware also applies software oversampling, DC removal, and a 3.7 kHz
> IIR low-pass filter after each buffer is captured.  The physical RC filter
> and the software filter together give significantly better noise rejection
> than either one alone.

---

## 4 – Phone hook switch (GPIO 22)

The telephone handset rests on a spring-loaded hook switch.  The switch is
**open** when the handset is in the cradle (on hook) and **closed** when the
handset is lifted (off hook).

Wire one terminal of the switch to **GPIO 22 (physical pin 29)** and the other
terminal to **GND**.  The firmware enables the internal pull-up resistor, so:

- **On hook** (circuit open) → GPIO reads **HIGH** (pulled to 3.3 V)
- **Off hook** (circuit closed) → GPIO reads **LOW**

```
GPIO 22 (pin 29) ──── HOOK SWITCH ──── GND (pin 38)
```

No additional components are required.  The firmware registers an interrupt on
both edges:

- **Falling edge** (HIGH → LOW): handset lifted – start greeting playback then recording.
- **Rising edge** (LOW → HIGH): handset replaced – stop the active recording.

```
                 ┌─────────────────────────────────────┐
                 │         Raspberry Pi Pico            │
  3V3 (pin 36) ──┤ internal                             │
                 │ pull-up ──── GPIO 22 (pin 29) ────────┼──── HOOK SWITCH terminal A
                 │                                      │
  GND (pin 38) ──┤────────────────────────────────────────────── HOOK SWITCH terminal B
                 └─────────────────────────────────────┘

  On hook  (handset in cradle): switch OPEN   → GPIO 22 reads HIGH (3.3 V via pull-up)
  Off hook (handset lifted)   : switch CLOSED → GPIO 22 reads LOW  (0 V via GND)
```

---
