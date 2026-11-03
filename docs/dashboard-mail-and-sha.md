# Dashboard history, Gmail reports and classic ESP32 SHA coordination

This contribution adds bounded in-memory hashrate history, optional email reports,
and diagnostics for separating mining pauses from WLAN and pool reconnects.

## Dashboard

- Select 1 h, 6 h or 24 h; sample every 30 seconds.
- The 6 h and 24 h views average samples into 2-minute and 5-minute buckets.
- Missing buckets are `null`; an actual zero remains zero. History resets on reboot.
- History sample arrays use 1,176 bytes. No history is written to flash.
- Show minimum, maximum and mean for the selected range, and inspect chart values
  using mouse, touch or keyboard. CSV export runs in the browser, not on the ESP32.
- Extended statistics include share success, median and dips below 90% of the median.
- `GET /api/history?range=1h|6h|24h` exposes interval, age and ordered samples.

## Optional Gmail reports

Mail is disabled by default. Sender, recipient and app password defaults are empty.
Configure them on the authenticated settings page; a blank password field preserves
the saved password. Enter a 16-character Google app password, not the account password.
Do not commit configuration files, flash dumps or app passwords to GitHub.

The current schedule is **20:00 Europe/Berlin**, including EU summer time. It is
independent of the display timezone. This is intentionally the implemented schedule,
not a claim of configurable timezone support. A daily report still pending after
20:00 can be sent later when mail and the network become available.

Pool warnings require a 120-second outage; recovery requires 30 seconds of stable
connection. A bounded queue holds a report, a test and four incident entries.
Failures back off 15 minutes, with at most three attempts per queued event. A failed
daily report keeps its date/retry budget in NVS across restart; the next day has a
new report budget. An exhausted old report can therefore still appear in status
after a later test mail succeeds.

SMTP uses `smtp.gmail.com:465`, certificate/hostname verification and bundled GTS
root certificates. Certificate maintenance remains necessary. The secret is stored
with device settings, not encrypted by this feature. Configuration logging redacts
the new email fields. No authentication payload or server reply body is logged by
the mail worker. A final SMTP 250 response is treated as Gmail acceptance; it does
not prove inbox delivery. The status is finalized after cleanup.

TLS temporarily shares the miner's SHA resources. A cooperative request lets the
classic hardware miner yield between chunks; acquisition has a 20-second limit.
The worker copies its input before starting. A failed test must not be retriggered
repeatedly while it is already queued.

## Classic ESP32 SHA coordination fix

The direct-register miner resets the shared SHA peripheral and bypasses the SDK's
shared text-memory lock. Reserving only `SHA2_256` does not exclude SDK `SHA1` or
`SHA2_384/512` users: their engine semaphores are separate. WPA's mbedTLS SHA-1
operations can consequently overlap the direct-register miner.

The classic worker and its startup self-test now reserve all three SDK engine
slots before direct register access. Failed nonblocking acquisition releases all
partial reservations before yielding. Successful acquisition releases all slots
after the chunk. SHA-384 shares SHA-512's slot. The SDK SHA-1 implementation falls
back to software when its engine cannot be acquired. No long interrupt-disabled
section is added; assembly and nonce calculations are unchanged.

Source references for the Arduino-ESP32 2.0.14 / ESP-IDF 4.4 generation:

- https://github.com/espressif/esp-idf/blob/v4.4.6/components/mbedtls/port/sha/parallel_engine/sha.c
- https://github.com/espressif/esp-idf/blob/v4.4.6/components/mbedtls/port/sha/parallel_engine/esp_sha1.c

## Diagnostics

Existing `/api/status` remains available. Additional read-only endpoints contain
bounded operational data, not email addresses, passwords or wallet configuration:

| Endpoint | Purpose |
| --- | --- |
| `/api/pauses` | Last 32 background GETs, SHA wait time vs held time, HTTP outcomes |
| `/api/rate-diagnostics` | Independent completed HW work and last 16 zero/stale samples |
| `/api/pool-diagnostics` | Last 32 local pool/reconnect events and cumulative counters |
| `/api/wifi-diagnostics` | Last 32 WLAN events, SDK reason codes, IP/association and test state |

Counters reset on reboot. An HTTP wait is not the same as time holding SHA. A
30-second HW-completion window is not directly identical to the displayed rolling
10-second total rate, which also includes software mining. WLAN reason codes are
observations and do not by themselves assign fault to the router.

The authenticated `/hardware-test` page can pause only the classic hardware miner
for 12 minutes. It starts only on an explicit authenticated, origin-checked POST,
ends automatically, and can be cancelled. WLAN and the software miner continue.
Diagnostic test windows must be excluded from normal performance comparisons.

The comparison firmware also disables WLAN modem sleep and exposes the actual
driver mode. This experiment did **not** eliminate reason 16 on the tested setup;
it is not the claimed SHA fix and increases radio power consumption.

Pool retries are paced at approximately 1, 2, 4, 8, 16 and at most 30 seconds;
short intervals have up to 250 ms jitter. TCP establishment is limited to 5 seconds,
but DNS and subscribe are additional steps. Negative cached WLAN status is checked
against station association and a nonzero IP before forcing a local reconnect.

## Build and tests

```sh
pio run -e ESP32_2432S028_2USB
python tools/check_classic_sha_guard.py --cxx /path/to/g++
```

The compile-time test runner accepts a native C++14 compiler or the PlatformIO
Xtensa compiler; it does not execute firmware or use attached devices. It checks
each contention branch, release, retry, the report boundary and opt-in defaults.
Upstream's independent native tests are still available with `pio test -e native`.

The source was ported to upstream `a1ee14f484bac8a284503d97dd29a6b4a5ac4b69`.
The earlier device-tested implementation used upstream `37bc1a5` as its base.
The port retains upstream's monotonic hash counter, atomic configuration save and
recent changes for other boards. Do not equate a successful port build with a
hardware test of the new combined revision.

## Reported device observations and limits

Two classic ESP32 dual-USB CYD units were used with a FRITZ!Box 6660 Cable,
FRITZ!OS 8.25 and WPA2. Repeated SDK reason 16 events occurred roughly every
600 seconds with hardware mining active. A confirmed 12-minute hardware-only
pause had no disconnect; the disconnects returned after mining resumed. Disabling
modem sleep alone did not resolve them.

After the all-engine reservation fix, a captured 73-minute normal-mining interval
had zero WLAN disconnects and zero pool reconnects. Its 1-hour sampled mean was
993.2 KH/s, with 958 accepted and zero rejected pool shares since boot. This is a
short observed interval, not a guaranteed hashrate or long-term stability result.
Gmail test acceptance and actual inbox receipt were confirmed on both units after
their individual mail settings were corrected. The automatic 20:00 report still
requires a separate end-to-end daily-schedule check.

The exact port to current upstream has been build-tested on the dual-USB CYD
environment only. Other boards and long-term operation have not been validated.

## Follow-up: RAM pressure and mail-worker cleanup

See [RAM diagnostics and mail-task cleanup](memory-diagnostics.md) for bounded heap measurements, streaming HTTP parsing, the software-miner IDLE scheduling fix and hardware-test limits.
