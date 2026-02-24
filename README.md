# ESP32-C6 WLED Protocol Shim for ESPHome

A drop-in C++ header that lets your ESP32-C6 receive LED colour data from ambilight and LED control software — without needing WLED firmware or NeoPixelBus.

**Confirmed working with:**
- ✅ [HyperHDR](https://github.com/awawa-dev/HyperHDR) — via WLED UDP protocol
- ✅ [Hyperion.ng](https://github.com/hyperion-project/hyperion.ng) — via DDP protocol

---

## The Problem

The **ESP32-C6** is an attractive microcontroller — it's affordable, capable, and supports WiFi 6. However, if you want to use it with ambilight software like HyperHDR or Hyperion.ng, you quickly run into a wall:

### WLED doesn't support the ESP32-C6

WLED firmware cannot be flashed to the C6. The chip architecture is incompatible with current WLED builds. This is a [known open issue](https://github.com/Aircoookie/WLED/issues/3078) with no official fix at time of writing.

### NeoPixelBus doesn't work with ESP-IDF

ESPHome on the ESP32-C6 requires the **esp-idf framework** (the Arduino framework is not supported on C6). Unfortunately:

- `neopixelbus` light platform **does not work** with esp-idf
- `fastled` light platform **does not work** with esp-idf

This means the typical ESPHome approach to driving addressable LED strips is unavailable on the C6.

### What does work on the C6

ESPHome's `esp32_rmt_led_strip` platform uses the ESP32's RMT peripheral directly and **does work** with esp-idf on the C6. So driving LEDs is actually fine — the gap is purely on the receiving side: getting colour data *into* the ESP32 from external software.

### The typical advice

Most forum answers to this problem fall into one of three camps:

1. **"Use a different board"** — swap your C6 for an ESP32, ESP32-S2, or similar. Unhelpful if you've already bought C6s.
2. **"Flash HyperSerialESP32"** — the HyperSerial project provides fast USB serial firmware for HyperHDR, but it also does not support the C6.
3. **"Wait for WLED C6 support"** — the issue has been open for years.

None of these help if you have a pile of C6s and want to use them *now*.

---

## The Solution

This project implements a **multi-protocol LED shim** as a single C++ header file (`wled_udp.h`) that you include in your ESPHome configuration.

It runs four things simultaneously:

1. **Serves a fake WLED `/json` HTTP endpoint on port 80** — just enough JSON to convince HyperHDR (and other WLED-aware software) that it's talking to a real WLED device and pass the LED count handshake.
2. **Listens for WLED realtime UDP packets on port 21324** — handles WARLS, DRGB, and DNRGB protocols used by HyperHDR.
3. **Listens for DDP packets on port 4048** — handles the Distributed Display Protocol used by Hyperion.ng and other software.
4. **Stores received colour data in a shared buffer** — which your ESPHome `addressable_lambda` light effect reads every 33ms (~30fps) and writes to the physical LED strip via `esp32_rmt_led_strip`.

Both WLED UDP and DDP write into the same shared buffer, so whichever software is active just works — no configuration changes needed to switch between them.

---

## Requirements

- ESP32-C6 (tested on `esp32-c6-devkitc-1`)
- ESPHome with **esp-idf framework** (required for C6)
- HyperHDR, Hyperion.ng, or other compatible software
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

**This is important.** The shim occupies **port 80** to serve the `/json` stub. If you use ESPHome's `web_server` component, it must be moved to a different port:

```yaml
web_server:
  port: 8080
```

If you don't use `web_server`, you can omit it entirely.

### Step 4 — Flash and configure your software

Flash your ESP32-C6 via ESPHome as normal. In your LED software, add a new WLED device pointing at your ESP32-C6's IP address and follow that software's usual setup instructions. The shim handles the rest transparently.

### Step 5 — Enable the WLED effect

In your ESPHome/Home Assistant light entity, activate the **WLED** effect. Without this, the LED strip won't output anything even if data is being received.

---

## How It Works (Technical Detail)

### The HTTP stub

HyperHDR's WLED driver performs a `GET /json` request before sending any LED data. It uses the response to verify the device is a WLED instance and to confirm the LED count.

The shim runs a minimal TCP server on port 80 that responds with just enough JSON to satisfy this check:

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

The LED count is driven by the same `WLED_NUM_LEDS` define as everything else, so it always stays in sync.

### The WLED UDP listener (port 21324)

Used by HyperHDR. The shim handles three WLED realtime protocols:

| Protocol | ID | Description |
|---|---|---|
| WARLS | `1` | Sparse updates — sends only changed LEDs by index |
| DRGB | `2` | Full frame — sends all LED colours in one packet |
| DNRGB | `4` | Full frame with start offset — supports >256 LEDs |

### The DDP listener (port 4048)

Used by Hyperion.ng and other DDP-capable software. DDP ([Distributed Display Protocol](http://www.3waylabs.com/ddp/)) uses a compact 10-byte header followed by raw RGB data:

| Bytes | Content |
|---|---|
| 0 | Flags (version, push, query, reply) |
| 1 | Sequence number |
| 2 | Data type |
| 3 | Destination ID |
| 4–7 | Byte offset (32-bit MSB first) |
| 8–9 | Data length (16-bit MSB first) |
| 10+ | RGB data |

The offset field allows large frames to be split across multiple packets, which the shim handles correctly by writing each chunk into the right position in the shared buffer.

### The shared buffer and ESPHome light effect

Both protocols write into the same shared buffer (`g_wled_buf`) protected by a FreeRTOS mutex. An `addressable_lambda` effect polls the buffer every 33ms and writes colour values to the LED strip:

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

The mutex `TryTake` with timeout `0` means if a UDP task is mid-write, the frame is simply skipped rather than blocking — keeping the LED output smooth. If no data has been received for 3 seconds, the strip blanks so LEDs don't freeze on the last frame if software disconnects.

---

## Caveats

**Tested with HyperHDR and Hyperion.ng only.**
HyperHDR (WLED UDP) and Hyperion.ng (DDP) are confirmed working. Other software (SignalRGB, LightFX, Prismatik, LedFX, etc.) may work, may partially work, or may require additional endpoint support. If you test another application and find issues, please open an issue or PR.

**Hyperion.ng on Wayland.**
Hyperion.ng's X11 screen grabber does not work under Wayland. LED output via DDP works fine — the grabber issue only affects screen capture, not the LED data path. You can still manually set colours or use other input sources.

**Linux development only.**
This was developed on Linux. It has not been tested on Windows or macOS ESPHome environments. It should work, but is unverified.

**No WLED effects.**
This shim only handles *receiving* realtime colour data from an external source. It does not implement WLED's built-in effects engine, segment support, or any other WLED features. It is purely a protocol bridge.

**esp-idf framework required.**
This will not compile under the Arduino framework. The ESP32-C6 requires esp-idf anyway, but if you attempt to use this on another board with the Arduino framework it won't work.

**Port 80 is consumed.**
The shim owns port 80 for the HTTP stub. Move any other services (ESPHome `web_server`, etc.) to alternative ports.

**Single LED output only.**
The current implementation drives one LED strip from one data pin. Multi-segment or multi-output configurations are not supported in this initial release.

**Remember to enable the WLED effect.**
The ESPHome light entity must have the WLED effect active to output anything. This is easy to forget — if your LEDs aren't responding despite data being received, check the effect is enabled.

---

## Why Not Just Use a Different Board?

You might have bought ESP32-C6s specifically for their WiFi 6 capability, their price point, or because they were available. Having a bag of perfectly good microcontrollers that "just don't work" with your software stack is frustrating — and "buy different hardware" is a rubbish answer when the hardware works fine for everything else.

This shim exists so you can use what you have.

---

## Contributing

If you test this with software other than HyperHDR and Hyperion.ng and it works (or doesn't), please open an issue or PR with your findings. Contributions are welcome.

---

## Acknowledgements

- [WLED Project](https://github.com/Aircoookie/WLED) for the UDP realtime protocol specification
- [HyperHDR](https://github.com/awawa-dev/HyperHDR) for the ambilight software this was developed and tested against
- [Hyperion.ng](https://github.com/hyperion-project/hyperion.ng) for DDP compatibility testing
- [3waylabs](http://www.3waylabs.com/ddp/) for the DDP protocol specification
- The ESP32-C6 WLED support issue thread, where the frustration that prompted this project lives

---

## Licence

MIT — do whatever you like with it.
