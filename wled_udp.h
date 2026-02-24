#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_timer.h>
#include <mdns.h>
#include <string.h>
#include <stdio.h>

static const uint16_t WLED_UDP_PORT      = 21324;
static const uint16_t WLED_HTTP_PORT     = 80;
static const int64_t  WLED_ACTIVE_WINDOW = 3LL * 1000000LL;  // 3 seconds in µs

// WLED_NUM_LEDS can be overridden via build_flags in your ESPHome YAML:
//   platformio_options:
//     build_flags:
//       - "-DWLED_NUM_LEDS=66"
// If not set, defaults to 66.
#ifndef WLED_NUM_LEDS
#define WLED_NUM_LEDS 66
#endif

static const size_t WLED_BUF_SIZE = WLED_NUM_LEDS * 3;

volatile uint8_t  g_wled_buf[WLED_NUM_LEDS * 3];
volatile int64_t  g_wled_last_us = 0;
SemaphoreHandle_t g_wled_mutex   = nullptr;

// Returns true if a WLED packet was received within the last 3 seconds.
inline bool wled_is_active() {
    if (g_wled_last_us == 0) return false;
    return (esp_timer_get_time() - g_wled_last_us) < WLED_ACTIVE_WINDOW;
}

// ── Minimal WLED /json HTTP stub ──────────────────────────────────────────────
// HyperHDR's WLED driver calls GET http://<host>/json before sending UDP data.
// We serve just enough JSON to pass the LED count check.
// ESPHome's web_server is moved to port 8080 to free port 80 for this task.

static void wled_http_task(void *pvParam) {
    char req_buf[256];
    char resp_buf[512];
    int server_fd = -1;

    for (;;) {
        server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        int reuse = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(WLED_HTTP_PORT);

        if (::bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ::close(server_fd); server_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(2000)); continue;
        }
        if (::listen(server_fd, 2) == 0) break;
        ::close(server_fd); server_fd = -1;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        // 2-second recv timeout — prevents task locking up on stalled connections
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int len = ::recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (len > 0) {
            req_buf[len] = '\0';
            if (strncmp(req_buf, "GET", 3) == 0) {
                char json_body[256];
                snprintf(json_body, sizeof(json_body),
                    "{\"state\":{\"on\":true,\"bri\":255},"
                    "\"info\":{\"ver\":\"0.14.0\","
                    "\"leds\":{\"count\":%d,\"rgbw\":false},"
                    "\"name\":\"WLED\","
                    "\"udpport\":21324}}",
                    WLED_NUM_LEDS);
                int json_len = (int)strlen(json_body);
                int resp_len = snprintf(resp_buf, sizeof(resp_buf),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    json_len, json_body);
                ::send(client_fd, resp_buf, resp_len, 0);
            } else {
                // POST /json (brightness override) — acknowledge and ignore
                const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                ::send(client_fd, ok, strlen(ok), 0);
            }
        }
        ::close(client_fd);
    }
}

// ── UDP realtime listener (WARLS / DRGB / DNRGB) ─────────────────────────────

static void wled_udp_task(void *pvParam) {
    uint8_t recv_buf[512];
    int sock = -1;

    for (;;) {
        sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        int reuse = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port        = htons(WLED_UDP_PORT);

        if (::bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0) break;

        ::close(sock);
        sock = -1;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // mDNS — safe to call now as WiFi is up (bind succeeded)
    mdns_service_add("WLED", "_wled", "_tcp", WLED_UDP_PORT, nullptr, 0);

    for (;;) {
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t len = ::recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                 (struct sockaddr *)&sender, &sender_len);
        if (len < 2) continue;

        uint8_t protocol = recv_buf[0];

        if (xSemaphoreTake(g_wled_mutex, portMAX_DELAY) == pdTRUE) {
            bool valid = false;

            if (protocol == 1) {
                // WARLS: [1, timeout, idx, R, G, B, ...]
                const uint8_t *p   = recv_buf + 2;
                const uint8_t *end = recv_buf + len;
                while (p + 3 < end) {
                    uint8_t idx = p[0];
                    if (idx < WLED_NUM_LEDS) {
                        g_wled_buf[idx * 3 + 0] = p[1];
                        g_wled_buf[idx * 3 + 1] = p[2];
                        g_wled_buf[idx * 3 + 2] = p[3];
                    }
                    p += 4;
                }
                valid = true;

            } else if (protocol == 2) {
                // DRGB: [2, timeout, R0, G0, B0, ...]
                if (len >= (ssize_t)(2 + WLED_BUF_SIZE)) {
                    memcpy((void *)g_wled_buf, recv_buf + 2, WLED_BUF_SIZE);
                    valid = true;
                }

            } else if (protocol == 4 && len >= 4) {
                // DNRGB: [4, timeout, start_hi, start_lo, R, G, B, ...]
                uint16_t start_idx = ((uint16_t)recv_buf[2] << 8) | recv_buf[3];
                const uint8_t *p   = recv_buf + 4;
                const uint8_t *end = recv_buf + len;
                uint16_t led_idx   = start_idx;
                while (p + 2 < end && led_idx < (uint16_t)WLED_NUM_LEDS) {
                    g_wled_buf[led_idx * 3 + 0] = p[0];
                    g_wled_buf[led_idx * 3 + 1] = p[1];
                    g_wled_buf[led_idx * 3 + 2] = p[2];
                    p += 3;
                    led_idx++;
                }
                valid = true;
            }

            if (valid) g_wled_last_us = esp_timer_get_time();
            xSemaphoreGive(g_wled_mutex);
        }
    }
}

// Call once from on_boot (priority -100) — starts HTTP stub + UDP listener.
inline void wled_udp_start() {
    g_wled_mutex = xSemaphoreCreateMutex();
    if (!g_wled_mutex) return;
    memset((void *)g_wled_buf, 0, WLED_BUF_SIZE);
    xTaskCreate(wled_http_task, "wled_http", 8192, nullptr, 1, nullptr);
    xTaskCreate(wled_udp_task,  "wled_udp",  4096, nullptr, 1, nullptr);
}
