# AquaPulse Smart Fish Feeder — ESP32 Standalone Wiring Guide

This replaces the Arduino Uno + ESP8266 two-board setup entirely. One ESP32 now
drives every peripheral directly and handles WiFi/Firestore. No Uno, no
SoftwareSerial link, no logic level converter, no resistor divider between
boards (that link no longer exists).

---

## ⚠️ Read This First — Why the Last Build's Regulators Burned

Root cause found: a broken jumper left **half of a power rail unpowered /
floating** while the other half was still live, which back-fed voltage into
the 3.3V regulator until it cooked. This was a wiring integrity issue, not a
component or design flaw — but it can happen again on any breadboard build if
you don't verify rails before powering up. Follow this checklist **every
time** you build or rebuild this circuit:

1. **Before connecting any board or module**, set a multimeter to
   resistance/continuity mode and, with nothing powered, check:
   - 3.3V rail to GND rail → should read very high resistance (open), not near-zero
   - 3.3V rail to 5V rail → should read very high resistance (open)
   - End-to-end continuity along the *entire length* of each rail strip
     (breadboards sometimes have internal strip breaks or a rail split by
     the center gap — checking only one point can miss a "half powered" rail)
2. **Never bridge two different voltage rails through the same breadboard row.**
3. **Common ground is mandatory** — ESP32 GND, external power supply GND, and
   every peripheral's GND must tie to the same node. Check this with continuity
   too, not just by assuming the wiring is right.
4. **Verify actual resistor values with a multimeter** before trusting the
   color-code — a mislabeled resistor in the divider changes the real voltage
   your ESP32 pin sees.
5. This build should not live on a breadboard long-term. Vibration from the
   stepper works connections loose over time — plan to move to perfboard/PCB
   once the design is confirmed working.

---

## Components

| Component | Notes |
|---|---|
| ESP32 DevKit (30 or 38-pin, e.g. "DOIT ESP32 DEVKIT V1") | Replaces Uno + ESP8266 entirely |
| External 5V power supply (fixed-output breadboard module or wall adapter) | Powers stepper driver + HC-SR04 directly — NOT through the ESP32's onboard regulator |
| 16x2 I2C LCD backpack | Address `0x27` |
| DS3231 RTC module | Physical RTC, I2C, shares bus with LCD |
| 28BYJ-48 stepper motor + ULN2003 driver board | Drives the feed screw |
| Piezo buzzer | Feed confirmation / fault alert |
| HC-SR04 ultrasonic sensor | Hopper level sensing — ECHO needs a divider |
| Push button | Manual feed trigger |
| Status LED | Fault/status indicator |
| 1kΩ + 2kΩ resistors | ECHO voltage divider (5V → ~3.3V) |

---

## ESP32 Pin Map

| ESP32 GPIO | Connects To | Notes |
|---|---|---|
| GPIO21 (SDA) | LCD SDA + RTC SDA | Shared I2C bus |
| GPIO22 (SCL) | LCD SCL + RTC SCL | Shared I2C bus |
| GPIO4 | Push button | `INPUT_PULLUP` — button to GND |
| GPIO5 | Buzzer (+) | |
| GPIO18 | HC-SR04 TRIG | Direct — ESP32 output at 3.3V is a valid trigger for the sensor |
| GPIO19 | HC-SR04 ECHO divider tap | **Not direct** — see divider section |
| GPIO25 | ULN2003 IN1 | |
| GPIO26 | ULN2003 IN3 | (order matches `Stepper` library init — see note below) |
| GPIO27 | ULN2003 IN2 | |
| GPIO14 | ULN2003 IN4 | |
| GPIO13 | Status LED (+) | Through ~220Ω resistor. **Not GPIO12** — that pin is a boot-strapping pin and can prevent the board from booting if pulled the wrong way at power-on |
| 3V3 | (not used for stepper/sensor — see power section) | |
| GND | Common ground rail | Shared with everything |

Avoid GPIO0, 2, 15 (boot-strapping pins) and GPIO34–39 (input-only, can't drive LEDs/outputs).

---

## Text Sketch — Overall System

```
                         ┌─────────────────────────┐
                         │         ESP32            │
                         │                           │
        Button ──────────┤ GPIO4               GPIO21├───── SDA ──┬── LCD
                         │                     GPIO22├───── SCL ──┤
        Buzzer ──────────┤ GPIO5                           │  DS3231 RTC
                         │                           │
   HC-SR04 TRIG ─────────┤ GPIO18                          │
   HC-SR04 ECHO ── divider ──GPIO19                          │
                         │                           │
   ULN2003 IN1 ──────────┤ GPIO25                          │
   ULN2003 IN3 ──────────┤ GPIO26                          │
   ULN2003 IN2 ──────────┤ GPIO27                          │
   ULN2003 IN4 ──────────┤ GPIO14                          │
                         │                           │
   Status LED ───────────┤ GPIO13                          │
                         │                           │
                         │ GND ─────────────────────┼───── common ground rail
                         │ VIN/5V ──────────────────┼───── external 5V supply (ESP32 power only)
                         └─────────────────────────┘

   External 5V supply ──┬── ULN2003 VCC (motor power)
                         └── HC-SR04 VCC
   (separate feed from the ESP32's own 5V — do NOT daisy-chain through
   the ESP32's onboard regulator pin)

   All GND (ESP32, external supply, ULN2003, HC-SR04, LCD, RTC) ── common node
```

---

## Text Sketch — HC-SR04 Voltage Divider (ECHO only)

TRIG connects directly (ESP32's 3.3V output is read fine by the sensor).
ECHO outputs 5V and MUST be divided down — same principle as before, just
now it feeds directly into the ESP32 instead of an ESP8266.

```
   HC-SR04 ECHO (5V pulse)
         │
         ├──[1kΩ]──┐
         │           │
         │           ├────────► ESP32 GPIO19
         │           │
         │         [2kΩ]
         │           │
         │           ▼
         │          GND
         │
   (divider output ≈ 5V × 2k/(1k+2k) ≈ 3.3V)
```

| Connection | Notes |
|---|---|
| HC-SR04 ECHO → 1kΩ resistor | First leg |
| 1kΩ/2kΩ junction → ESP32 GPIO19 | Tap point, ≈3.3V |
| 2kΩ resistor → GND | Second leg, common ground |
| HC-SR04 TRIG → ESP32 GPIO18 | Direct, no resistors |
| HC-SR04 VCC → external 5V supply | Not from ESP32's own regulator |
| HC-SR04 GND → common ground | |

---

## Text Sketch — Stepper Driver (28BYJ-48 + ULN2003)

```
   ESP32 GPIO25 ──────► ULN2003 IN1
   ESP32 GPIO26 ──────► ULN2003 IN3
   ESP32 GPIO27 ──────► ULN2003 IN2
   ESP32 GPIO14 ──────► ULN2003 IN4

   External 5V ───────► ULN2003 VCC (motor power pins — separate from IN1-4 logic)
   Common GND ─────────► ULN2003 GND

   ULN2003 motor socket ──► 28BYJ-48 5-wire connector (keyed, only fits one way)
```

The board's screw-terminal / header labeled IN1–IN4 connects straight to the
ESP32 pins listed above. The `Stepper` library is initialized with the order
`IN1, IN3, IN2, IN4` to match the motor's internal coil sequence — that
reordering is handled entirely in software, the physical wiring stays in
numeric IN1→IN4 order.

> ⚠️ **Power the ULN2003's VCC from the external 5V supply directly, not
> from the ESP32's 5V/3V3 pin.** The stepper draws up to ~240–300mA when
> moving — enough to brown out a small onboard regulator, which is part of
> what killed the previous power modules under load.

---

## Text Sketch — I2C Bus (LCD + RTC)

```
   ESP32 GPIO21 (SDA) ──┬── LCD SDA
                          └── RTC SDA

   ESP32 GPIO22 (SCL) ──┬── LCD SCL
                          └── RTC SCL

   5V ──┬── LCD VCC
         └── RTC VCC

   GND ──┬── LCD GND
          └── RTC GND
```

Pull-up resistors are typically already included on the LCD backpack and
DS3231 breakout boards — no external pull-ups usually needed.

---

## Power Distribution Summary

```
                 ┌───────────────────────────┐
   Wall/USB ────►│  External 5V Power Supply  │
                 └─────────┬─────────┬────────┘
                           │         │
                       5V rail    GND rail
                           │         │
              ┌────────────┼────┬────┼─────────────┐
              │            │    │    │             │
         ULN2003 VCC   HC-SR04 VCC  ESP32 VIN   LCD/RTC VCC
              │            │    │    │             │
              └────────────┴────┴────┴─────────────┘
                       (all GND commoned together)
```

- **ESP32 VIN/5V pin**: powers the ESP32 board itself only.
- **External 5V supply**: powers the stepper driver and HC-SR04 directly — this is the load that actually draws meaningful current, so it shouldn't route through the ESP32's small onboard regulator.
- **LCD/RTC**: low current, fine off either 5V source, but keep them on the same rail as everything else to avoid any ground-reference mismatch.

---

## Final Pre-Power Checklist

- [ ] Multimeter continuity check: 3.3V↔GND, 3.3V↔5V, both read open (no short)
- [ ] Multimeter check: every rail reads continuous end-to-end (no "half powered" rail)
- [ ] Divider resistors measured with multimeter, confirmed close to 1kΩ/2kΩ
- [ ] All GNDs (ESP32, supply, ULN2003, HC-SR04, LCD, RTC) confirmed common
- [ ] ULN2003 and HC-SR04 VCC wired to the *external* 5V supply, not the ESP32's own regulator pin
- [ ] Stepper motor connector fully seated in the ULN2003 socket
- [ ] LED on GPIO13, not GPIO12
- [ ] Power up the ESP32 alone first (nothing else wired) and confirm it boots normally before adding peripherals one at a time
