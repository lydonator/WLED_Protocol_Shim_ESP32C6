# ESP32-C6 WLED Protocol Shim for ESPHome + HyperHDR (and potentially others)

A drop-in C++ header that lets your ESP32-C6 receive WLED realtime UDP data from HyperHDR (and potentially other WLED-compatible software) — without needing WLED firmware or NeoPixelBus.

---

## The Problem

The **ESP32-C6** is an attractive microcontroller — it's affordable, capable, and supports WiFi 6. However, if you want to use it with **HyperHDR** (or any ambilight/LED control software that speaks the WLED protocol), you quickly run into a wall:

### WLED doesn't support the ESP32-C6

WLED firmware cannot be flashed to the C6. The chip architecture is incompatible with current WLED builds. This is a [known open issue](https://github.com/Aircoookie/WLED/issues/3078) with no official fix at time of writing.

### NeoPixelBus doesn't work with ESP-IDF

ESPHome on the ESP32-C6 requires the **esp-idf framework** (the Arduino framework is not supported on C6). Unfortunately:

- `neopixelbus` light platform **does not work** with esp-idf
- `fastled` light platform **does not work** with esp-idf

This means the typical ESPHome approach to driving addressable LED strips is unavailable on the C6.

### What does work on the C6

ESPHome's `esp32_rmt_led_strip` platform uses the ESP32's RMT peripheral directly and **does work** with esp-idf on the C6. So driving LEDs is actually fine — the gap is purely on the receiving side: getting colour data *into* the ESP32 from software like HyperHDR.

### The typical advice

Most forum answers to this problem fall into one of three camps:

1. **"Use a different board"** — swap your C6 for an ESP32, ESP32-S2, or similar. Unhelpful if you've already bought C6s.
2. **"Flash HyperSerialESP32"** — the HyperSerial project provides fast USB serial firmware for HyperHDR, but it also does not support the C6.
3. **"Wait for WLED C6 support"** — the issue has been open for years.

None of these help if you have a pile of C6s and want to use them *now*.

---

## The Solution

This project implements a **minimal WLED protocol shim** as a single C++ header file (`wled_udp.h`) that you include in your ESPHome configuration.

It does three things:

1. **Serves a fake WLED `/json` HTTP endpoint on port 80** — just enough JSON to convince HyperHDR that it's talking to a real WLED device and pass the LED count handshake.
2. **Listens for WLED realtime UDP packets on port 21324** — handles the three main realtime protocols (WARLS, DRGB, DNRGB).
3. **Stores received colour data in a shared buffer** — which your ESPHome `addressable_lambda` light effect reads every 33ms (~30fps) and writes to the physical LED strip via `esp32_rmt_led_strip`.

The result: HyperHDR thinks it's talking to WLED. Your LEDs do exactly what HyperHDR tells them to. Your ESP32-C6 stays in ESPHome where you can manage it alongside the rest of your home automation.

---

## Requirements

- ESP32-C6 (tested on `esp32-c6-devkitc-1`)
- ESPHome with **esp-idf framework** (required for C6)
- HyperHDR configured to use the **WLED** LED controller type
- Addressable LED strip compatible with `esp32_rmt_led_strip` (WS2812B, WS2815, etc.)

---

## Files

| File | Description |
|---|---|
| `wled_udp.h` | The shim — copy this to your ESPHome config directory |
| `wled_udp_example.yaml` | Minimal working ESPHome YAML to get started |

---

## Installation

### Step 1 — Copy the header

Copy `wled_udp.h` into your ESPHome configuration directory (the same folder where your `.yaml` files live).

### Step 2 — Configure your YAML

Use `wled_udp_example.yaml` as your starting point. The only values you need to change are at the top:

```yaml
substitutions:
  led_count: "66"   # ← Set this to your actual LED count
```

And further down:

```yaml
pin: GPIO3          # ← Set this to your LED data pin
rgb_order: GRB      # ← Adjust if colours appear wrong (WS2812B/WS2815 = GRB)
```

The `led_count` substitution flows through automatically to the header via a build flag and to the light config — you only need to set it once.

### Step 3 — Move your web_server to port 8080

**This is important.** The shim occupies **port 80** to serve the `/json` stub that HyperHDR expects. If you use ESPHome's `web_server` component, it must be moved to a different port:

```yaml
web_server:
  port: 8080
```

If you don't use `web_server`, you can omit it entirely.

### Step 4 — Flash and configure HyperHDR

Flash your ESP32-C6 via ESPHome as normal. Then in HyperHDR:

1. Go to **LED Hardware**
2. Set **Controller type** to `WLED`
3. Enter your ESP32-C6's IP address
4. HyperHDR will discover the device, check `/json`, and begin sending UDP data

Once connected, enable the **WLED** effect on your ESPHome light entity and you're done.

---

## How It Works (Technical Detail)

### The HTTP stub

HyperHDR's WLED driver performs a `GET /json` request before sending any LED data. It uses the response to verify the device is a WLED instance and to confirm the LED count.

The shim runs a minimal TCP server on port 80 that responds to `GET` requests with just enough JSON to satisfy this check:

```json
{
  "state": {"on": true, "bri": 255},
  "info": {
    "ver": "0.14.0",
    "leds": {"count": 66, "rgbw": false},
    "name": "WLED",
    "udpport": 21324
  }
}
```

The LED count in this response is driven by the same `WLED_NUM_LEDS` define as everything else, so it always stays in sync.

`POST /json` requests (brightness overrides) are acknowledged with a 200 OK and ignored — the shim defers all control to HyperHDR.

### The UDP listener

Once HyperHDR is satisfied with the handshake, it begins sending realtime colour data as UDP packets to port 21324. The shim handles three WLED realtime protocols:

| Protocol | ID | Description |
|---|---|---|
| WARLS | `1` | Sparse updates — sends only changed LEDs by index |
| DRGB | `2` | Full frame — sends all LED colours in one packet |
| DNRGB | `4` | Full frame with start offset — supports >256 LEDs |

Received data is written into a shared buffer (`g_wled_buf`) protected by a FreeRTOS mutex.

### The ESPHome light effect

An `addressable_lambda` effect polls the shared buffer every 33ms and writes the colour values to the physical LED strip:

```cpp
if (xSemaphoreTake(g_wled_mutex, 0) == pdTRUE) {
  for (int i = 0; i < 66; i++) {
    it[i] = Color(
      g_wled_buf[i * 3 + 0],
      g_wled_buf[i * 3 + 1],
      g_wled_buf[i * 3 + 2]
    );
  }
  xSemaphoreGive(g_wled_mutex);
}
```

The mutex `TryTake` with timeout `0` means if the UDP task is mid-write, the frame is simply skipped rather than blocking — this keeps the LED output smooth.

If no WLED data has been received in the last 3 seconds, the strip is blanked, so your LEDs won't freeze on the last frame if HyperHDR disconnects.

---

## Caveats

**Tested with HyperHDR only.**
This shim was developed and verified against HyperHDR's WLED controller implementation. Other software that uses the WLED protocol (SignalRGB, LightFX, Prismatik, etc.) may work, may partially work, or may query additional endpoints that the stub doesn't handle. Your mileage may vary. If you test another application and find issues, the HTTP handler in `wled_udp.h` is the place to look — additional endpoint handlers can be added to the `wled_http_task` function.

**Linux development only.**
This was developed on Linux. It has not been tested on Windows or macOS ESPHome environments. It should work, but is unverified.

**No WLED effects.**
This shim only handles *receiving* realtime colour data from an external source. It does not implement WLED's built-in effects engine, segment support, or any other WLED features. It is purely a protocol bridge.

**esp-idf framework required.**
This will not compile under the Arduino framework. The ESP32-C6 requires esp-idf anyway, but if you attempt to use this on another board with Arduino framework it won't work.

**Port 80 is consumed.**
The shim owns port 80 for the HTTP stub. Move any other services (ESPHome `web_server`, etc.) to alternative ports.

**Single LED output only.**
The current implementation drives one LED strip from one data pin. Multi-segment or multi-output configurations are not supported in this initial release.

---

## Why Not Just Use a Different Board?

You might have bought ESP32-C6s specifically for their WiFi 6 capability, their price point, or because they were available. Having a bag of perfectly good microcontrollers that "just don't work" with your software stack is frustrating — and "buy different hardware" is a rubbish answer when the hardware works fine for everything else.

This shim exists so you can use what you have.

---

## Contributing

If you test this with software other than HyperHDR and it works (or doesn't), please open an issue or PR with your findings. Expanding the HTTP stub to handle additional endpoints is straightforward and contributions are welcome.

---

## Acknowledgements

- [WLED Project](https://github.com/Aircoookie/WLED) for the UDP realtime protocol specification
- [HyperHDR](https://github.com/awawa-dev/HyperHDR) for the ambilight software this was developed against
- The ESP32-C6 WLED support issue thread, where the frustration that prompted this project lives

---

## Licence

MIT — do whatever you like with it.
