# jwatch

A tiny 16×2 character-LCD "watch":

- **3 seconds** of a dithered 2D animation (drawn with an ASCII density ramp — plasma / bands / ripple / metaballs)
- **3 seconds** of a real-time clock (`HH:MM:SS` + date), synced over Wi-Fi via NTP
- 6-second full cycle, no flicker (no CGRAM swapping)

Hardware: **Seeed Studio XIAO ESP32-C6** + **ZETTLER AQM1602Y-RN-GBW** (Sitronix **ST7032i** I²C LCD module).

## Hardware

### LCD (AQM1602Y-RN-GBW, 9-pin module)

| Pin | Name  | Connect to |
|----:|-------|------------|
| 1 | V0    | (open) |
| 2 | VOUT  | `[1 µF]` → pin 5 |
| 3 | CAP1N | `[1 µF]` → pin 4 |
| 4 | CAP1P | `[1 µF]` → pin 3 |
| 5 | VDD   | **3V3** (module is 3.3 V only) |
| 6 | VSS   | GND |
| 7 | SDA   | GPIO22 (XIAO **D4**) |
| 8 | SCL   | GPIO23 (XIAO **D5**) |
| 9 | /RES  | **3V3** (must be tied high, otherwise the controller never ACKs) |

The two `1 µF` capacitors are **required** — they are the charge-pump for the
internal voltage booster. Without them / without `/RES` tied high you get no
display and no I²C ACK.

I²C slave address: **0x3E**.

### Notes

- The module is **3.3 V only** (VDD 3.1–3.5 V). Never feed it 5 V.
- No external I²C pull-ups are used here: the sketch enables the ESP32-C6's
  **internal pull-ups**. These are weak (~45 kΩ), so the firmware probes
  100 kHz / 50 kHz / 20 kHz / 10 kHz at boot and locks onto the first speed
  that gets an ACK (usually 50–100 kHz). For a rock-solid 100 kHz bus, add
  external 4.7–10 kΩ resistors from SDA/SCL to 3V3.

## Software

### Arduino CLI

```sh
arduino-cli core install esp32:esp32
cp secrets.h.example secrets.h      # then edit secrets.h with your Wi-Fi
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 .
arduino-cli upload  -p /dev/cu.usbmodemXXXX --fqbn esp32:esp32:XIAO_ESP32C6 .
```

### Arduino IDE

Open `jwatch.ino`, select board **Seeed XIAO ESP32C6**, and upload.

### Wi-Fi

Copy `secrets.h.example` to `secrets.h` and fill in:

```c
#define WIFI_SSID "your-2.4ghz-ssid"
#define WIFI_PASS "your-password"
```

`secrets.h` is git-ignored. **Never commit it.**

If `WIFI_SSID` is left empty the clock still runs, seeded from the sketch
build time (it will drift).

## Layout

```
clock (3 s)                animation (3 s)
    12:34:56               .:=+*#@
   2026/10/09              @#*+=:.   (plasma / bands / ripple / metaballs)
```

## License

MIT (or whatever you prefer).
