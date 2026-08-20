# Lander

A small solar-powered desk toy built as a gift. It looks like a tiny planetary
lander that has settled onto a shelf and kept doing its job. An ESP32-S3 drives
a 240x240 TFT that shows outdoor temperature and weather, local time, sunrise
and sunset, and indoor temp and humidity. The screen dims itself along a
sun-bell curve through the day and slips into a calm dark night screen in the
small hours. Every so often the lander takes over the display to run a little
"doing science" animation, then hands the screen back to the clock.

![lander image](lander.heic)

## Getting started

You need [PlatformIO](https://platformio.org/) and a WiFi network. Copy
`include/secrets.example.h` to `include/secrets.h` and list one or more
networks. The strongest one in range is the one it joins.

```cpp
#define WIFI_NETWORKS \
  WIFI_AP("home-ssid",  "home-pass") \
  WIFI_AP("phone-ssid", "phone-pass")
```

Then build and flash. Use the ESP32's own USB-C port, since the TP4056 charging
port has no data lines.

```sh
pio run              # build
pio run -t upload    # flash
pio device monitor   # serial monitor (115200)
```

Nothing else needs configuring. Location comes from the IP address via
[ipinfo.io](https://ipinfo.io/), weather and timezone come from
[Open-Meteo](https://open-meteo.com/), and the clock syncs over NTP. Neither
service needs an API key.

Pure logic such as the battery discharge curve and the weather-code mapping has
host-side unit tests that run without hardware.

```sh
pio test -e native
```

## Features

- **Live weather and time.** Outdoor temperature, a condition icon, sunrise and
  sunset, and a 12-hour clock, refreshed every 30 minutes while awake. The radio
  stays off overnight.
- **Indoor readout.** Temperature and humidity from an SHT31 sitting behind the
  screen.
- **Science activities.** Roughly every 16 to 32 minutes the lander runs a short
  animation such as excavating rocks, cataloging stars, or transmitting to
  Earth. A couple more fire on their own when the panel is charging or when it
  is running on USB power.
- **Day and night behavior.** Backlight and rear LED follow the sun, and a
  dark-mode night screen with a wind-down and wake-up animation covers the quiet
  hours.

## Hardware

| Part | Detail |
|------|--------|
| MCU | ESP32-S3 SuperMini (HW-747), run at 80 MHz |
| Display | ST7789 240x240 IPS, 8-pin SPI |
| Sensor | SHT31 temp/humidity (I2C 0x44) |
| LED | Single WS2812B (NeoPixel) |
| Battery | LiPo 3.7 V, 2000 mAh |
| Solar | 0.6 W / 5 V mini panel, about 120 mA peak in full sun |
| Charger | TP4056 (solar to battery), separate boost (5 V) to the ESP32 5V pin |
| Power switch | SPST inline on the boost output (VOUT+ to switch to 5V pin) |

## Wiring

Everything hangs off the ESP32-S3 over SPI for the display and I2C for the
sensor, with the battery voltage read through a 2x100k divider. The diagram in
[docs/wiring.svg](docs/wiring.svg) shows how it goes together, and the exact pin
numbers live at the top of `src/main.cpp`.

The display is driven with Adafruit_ST7789 and Adafruit_GFX. TFT_eSPI is the
usual choice for these panels but it drew nothing at all on this bare module, so
it is not worth the detour.

## Power and battery life

Continuous draw is dominated by the always-awake CPU at about 23 mA, and the
backlight, display, and LED are each single-digit mA. Through the boost and LDO
supply chain that works out to roughly 46 mA from the battery, so a 2000 mAh
pack lasts about 1.8 days with no light at all.

There is no deep sleep, which is the obvious thing to reach for and would help a
lot. It was traded away on purpose so the night screen and the activities keep
running while nobody is watching, which is most of the point of the thing.

The 0.6 W panel charges the LiPo through the TP4056. In full direct sun it
delivers 100 to 120 mA, which is more than the device draws, so it runs and
recharges at the same time and can stay topped up indefinitely. Staying
net-neutral across a full day takes roughly 10 full-sun-equivalent hours.

In practice, on a windowsill in a cloudy climate, the panel is a life-extender
rather than a full power source. Expect a recharge every two or three days,
longer in a bright south or west window, with the battery buffering through
gloomy stretches. Where you put it matters more than anything else.

One known inefficiency is that the supply path boosts the LiPo to 5 V and then
drops it back to 3.3 V, wasting 30 to 40 percent in conversion. A single 3.3 V
buck off the LiPo would be the largest win left, but it is a hardware change.
