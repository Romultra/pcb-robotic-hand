# 008: Cutting control latency — BLE interval tuning + a Wi-Fi motor path

**Date:** 2026-06-23
**Status:** Decided

## Context

The latency panel (decision 007) showed the BLE round-trip between the webapp
and the board was higher than expected for a hand that is supposed to mirror a
patient in real time. The question was whether that delay was firmware
inefficiency we could remove, or the underlying BLE transport.

Reading the path end to end:

- The echo (`PING`) callback is already as tight as it can be. `CbPing::onWrite`
  calls `notify(value, len)` immediately inside the GATT callback with no
  deferral, so none of our own loop scheduling is folded into the number
  (decision 007). The MTU is already raised to 247 in `ble::init()`.
- What was missing is connection-parameter control. The central (the Android
  phone) picks the connection interval at connect, and Android's default is
  conservative (tens of ms). A BLE write and the echoed notify each wait for the
  next connection event, so the round-trip is roughly one to two connection
  intervals. The firmware was accepting whatever interval Android chose and
  never asking for a faster one.

So the latency is dominated by the connection interval, which is partly a
firmware lever (we can request a tighter interval) and partly a hard floor of
the BLE link once the interval is as low as the phone will allow. That floor is
the reason for also offering Wi-Fi as an alternative transport for the streamed
motor frames, which is the lower-latency, higher-rate path the board already has
the radio for.

## Options Considered

1. **BLE connection interval — leave it to the central vs request a tight one.**
   Leaving it means whatever Android defaults to, which is tuned for power, not
   responsiveness. NimBLE lets the peripheral request a connection-parameter
   update after connect (`NimBLEServer::updateConnParams`). Requesting a tight
   interval (7.5–15 ms) is the standard, low-risk way to pull the round-trip
   down; the central can still clamp the request to its own minimum, so we never
   force an interval the phone won't honour. Chose to request it on connect.
2. **2M PHY / TX power.** The 2M PHY mostly helps bulk throughput, not the
   round-trip of a few small bytes, which is set by the interval, not on-air
   time. TX power affects range and jitter at distance, not the base interval.
   Both were left alone to keep the change focused and clearly attributable.
3. **Wi-Fi transport — WebSocket vs plain HTTP fetch.** A WebSocket would hold
   one connection open and stream frames with the least per-message overhead,
   but Chrome's Local Network Access (the thing that lets the HTTPS app reach
   the board's plain-HTTP endpoint at all) exempts only `fetch`/XHR, not
   WebSocket/WebTransport (decision 002). So a WebSocket from the HTTPS page to
   the board is a non-starter without a device-hosted page, which we deliberately
   avoid. Plain `fetch` it is.
4. **Where the Wi-Fi command endpoint lives.** Reuse the existing synchronous
   `WebServer` in `drivers/webfs.cpp` (already up after a STA join for SD
   uploads) rather than stand up a second server. One more route, `POST /servo`,
   plus a `GET /lat` echo for the latency test. No new BLE characteristic, so the
   firmware/webapp UUID-sync check stays green.
5. **Frame encoding over HTTP — raw body vs query string.** A raw 5-byte body
   can contain `0x00` (a fully-open finger), which the Arduino `WebServer`
   handles awkwardly as a String arg, and a typed-array body risks a CORS
   preflight. Encoding the five normalized positions as ten hex chars in a `?f=`
   query arg keeps the request a body-less, preflight-free POST with no NUL
   bytes. The firmware decodes the hex and calls the same `servos::set_finger`
   remap and `logging::note_exercise_frame` as the BLE `SERVO_FRAME` path, so the
   two transports are interchangeable and logs don't depend on which was used.
6. **Connection reuse for streaming.** Ideally the TCP connection stays open so a
   stream of frames doesn't pay a handshake each time. The bundled `WebServer`
   (arduino-esp32 2.0.x) has its HTTP keep-alive path disabled upstream for a
   Chrome bug and exposes no `keepAlive()` setter, so every request is its own
   connection. The coalescing senders keep only one frame in flight, so
   connections never pile up; the per-frame handshake is simply folded into the
   measured Wi-Fi round-trip, which the panel reports as-is. Switching to an
   async server with real keep-alive (or WebSockets behind a device-hosted page)
   is noted as future work if the Wi-Fi path is adopted beyond validation.

## Decision

**Firmware.** On connect, `ServerCallbacks::onConnect` requests a tight
connection interval via `updateConnParams` (`cfg::BLE_CONN_ITVL_MIN`=6 → 7.5 ms,
`MAX`=12 → 15 ms, latency 0, 4.0 s supervision timeout, all in `config.h`). The
long supervision timeout keeps Wi-Fi/BLE coexistence airtime stealing from
tripping a spurious disconnect. `drivers/webfs.cpp` gains `POST /servo?f=<hex>`
(apply a 5-channel normalized frame, same remap + logging as `SERVO_FRAME`) and
`GET /lat?seq=N` (echo the sequence for the latency test), both with the existing
CORS header.

**Webapp.** `net.js` holds a `preferWifi` flag and `wifiControlActive()` (true
only when the user asked for Wi-Fi and the board has reported an IP). The single
frame chokepoint `servos.sendServoFrame` routes over `POST /servo` when active,
otherwise over BLE, so live mirror and clip playback switch transport with no
other change. A toggle in the Servos panel drives the flag; it is disabled until
a Wi-Fi link exists and forced off on BLE disconnect. The Latency panel gains a
**Wi-Fi round-trip** test that pings `/lat` and reports into the same stats table
as the BLE tests (mode = `wifi`), so the two transports compare directly.

**Measurement note.** First bench run (50 pings, gap 0): BLE echo min 11.9 ms /
median 22.6 / avg 28.8 / p95 45.3 / max 56.4 / σ 11.4, which matches a 7.5–15 ms
interval (round trip ≈ 1–2 intervals) — the connection-interval request did its
job. Wi-Fi over a STA-via-home-router link came back min 11.6 / median 26.8 /
avg 44.9 / p95 88.4 / max 116.9 / σ 29.5. The Wi-Fi **minimum** is essentially
tied with BLE, so the link is fast; the cost is all in the tail and jitter, which
is avoidable overhead rather than a Wi-Fi floor.

**Wi-Fi tuning attempt — tried and reverted.** Two firmware changes were tried to
pull the tail down, and **both were backed out** because they made things worse
on the bench:
1. **Wi-Fi modem sleep off** (`WiFi.setSleep(false)` in `connect_sta`). On this
   board it **reset the board on Wi-Fi connect** every time; with the line removed
   the join is reliable again. Most likely the sustained radio-awake current under
   BLE/Wi-Fi coexistence browns out the power path, or the PM-mode change faults
   the coexistence stack. Either way it is not usable here, so default modem sleep
   stays.
2. **Shorter idle yield** (`delay(1)` instead of `delay(5)` while the HTTP server
   is up). This made the latency **worse** (many dropped pings, much higher
   latency), not better — spinning the main loop harder appears to hurt the
   single-radio BLE/Wi-Fi coexistence rather than help servicing. Reverted to
   `delay(5)`.

So the *default* configuration (modem sleep on, `delay(5)`) is the one kept, and
it is what produced the acceptable first run above (Wi-Fi median 26.8 ms, 0
drops). Within this STA-via-router topology the Wi-Fi tail is therefore close to
expected: it is the cost of per-request TCP (no keep-alive in this `WebServer`),
the extra router hop, and single-radio coexistence with the always-on BLE link.

**The coexistence finding (the important one).** Re-running the BLE echo with
Wi-Fi *disconnected* gave min 12.1 / median 22.5 / avg 22.5 / p95 23.0 / max 33.9
/ σ 3.1, 0 drops — same median (the interval) but the jitter collapsed (σ 11.4 →
3.1) and the tail tightened (p95 45 → 23, max 56 → 34). That is the cost of
BLE/Wi-Fi coexistence laid bare: the S3 has one 2.4 GHz radio and one antenna
time-shared between the stacks, so each degrades the other while both are active.
This single number explains the whole picture — the Wi-Fi tail, the worse
BLE-with-Wi-Fi run, and why `setSleep(false)` (radio fully awake under
coexistence) reset the board. It is also a clean result for §5.9: the live-mirror
path pays a measurable coexistence tax because control (BLE) and any Wi-Fi data
share the radio.

**Could Wi-Fi reach "gaming-grade" latency here?** In principle yes, but not in
the validator's architecture — the things that cap it are deliberate choices, not
bugs. To actually get there on this chip you would need, together: (1) **drop the
BLE coexistence** while streaming — use Wi-Fi for control too and turn BLE off, so
one radio isn't split; (2) a **SoftAP** topology so the phone associates straight
to the board (no router hop, and a SoftAP doesn't modem-sleep), with a fixed IP so
no BLE is needed to discover it; (3) a **persistent / datagram transport** —
WebSocket (no per-frame TCP handshake) or a WebRTC DataChannel / WebTransport in
unreliable mode (UDP-like, the actual "gaming" transport), instead of one HTTP
request per frame; and (4) because browsers block WebSocket/WebRTC from an HTTPS
page to a plain-HTTP LAN device (mixed content; LNA covers only fetch/XHR), the
control page would have to be **served by the board itself** over HTTP (same
origin), which is exactly the device-hosted-page design this project chose *not*
to use (decision 002). That combination can put a 2.4 GHz ESP32 into the
single-digit-ms range with low jitter, but it is a different product: BLE-off,
SoftAP, device-hosted page, WebSocket/WebRTC. It is the right thing to write up as
**future work**, not to retrofit into the validator. The two cheap mode flips were
tried and did not pay off; the real win needs the topology + transport rework
above.

**What was built: a SoftAP latency experiment.** Rather than guess the ceiling, a
scoped measurement was added that takes the first two of those four steps (drop
the BLE coexistence + go SoftAP) while leaving the transport as the same
per-request HTTP `/lat` echo, so it stays directly comparable to the other latency
tests and isolates the coexistence + router-hop contribution from the per-frame
TCP cost (which it keeps). It is **not** the full gaming-grade product, just a
clean way to put a number on the floor.

- *Entry:* a new `SYS` BLE characteristic (discriminator `0011`, WRITE). Writing
  `0x01` latches a one-shot NVS flag (`sys`/`softap_lat`) and reboots. `setup()`
  reads and clears the flag, and on that boot brings the board up as an **open
  SoftAP** (`cfg::SOFTAP_SSID` = `PCB1-LatencyAP`, fixed IP 192.168.4.1) with the
  HTTP server running and **`ble::init()` skipped entirely** — so BLE never
  touches the radio. A plain RESET returns to normal BLE mode (flag already
  cleared). Entry-via-reboot was chosen over a runtime radio reconfig precisely
  because the runtime `setSleep(false)` flip reset the board; bringing SoftAP up
  cleanly at boot with no BLE avoids that whole class of instability.
- *Running it:* a **SoftAP round-trip** test in the Latency panel, same ping-pong
  and stats table as the others (mode `softap`). The webapp pings
  `http://192.168.4.1/lat`; no BLE or learned-IP guard, since BLE is off and the
  AP IP is fixed. The one manual step is switching the phone's Wi-Fi to the
  board's AP (inherent to SoftAP), and the page must not be reloaded while off the
  internet.

## Results and verdict

All runs are 50 sequential pings, gap 0, 8-byte payload, same panel:

| Path | min | median | avg | p95 | max | σ (jitter) | drops |
|------|----:|-------:|----:|----:|----:|-----------:|------:|
| BLE echo, Wi-Fi connected   | 11.9 | 22.6 | 28.8 | 45.3 | 56.4 | 11.4 | 0 |
| BLE echo, Wi-Fi off         | 12.1 | 22.5 | 22.5 | 23.0 | 33.9 |  3.1 | 0 |
| Wi-Fi STA (via router), BLE on | 11.6 | 26.8 | 44.9 | 88.4 | 116.9 | 29.5 | 0 |
| SoftAP (BLE off, direct)    | 12.3 | 23.4 | 24.7 | 40.8 | 60.8 |  9.3 | 0 |

What the numbers say:

- The **connection-interval request worked**: BLE sits at a ~22.5 ms median, and
  with Wi-Fi off the jitter is tiny (σ 3.1) and the tail tight (p95 23, max 34).
  Standalone BLE is excellent for this small-payload round trip.
- **Coexistence is the dominant jitter source** (BLE σ 11.4 with Wi-Fi up vs 3.1
  with it off), confirmed independently.
- **SoftAP removed most of the Wi-Fi penalty** but not enough to win. Dropping the
  router hop and the coexistence took the STA result (median 26.8, p95 88.4, σ
  29.5) down to median 23.4, p95 40.8, σ 9.3. That is a big improvement over STA,
  yet it only **ties** BLE's median and is still ~3× BLE's standalone jitter. The
  floor that remains is the per-request TCP handshake (no keep-alive), which a
  small HTTP request/response cannot get under.
- On top of being no faster, SoftAP is **far less practical** than BLE: it needs
  BLE switched off, a manual Wi-Fi switch to the board's AP, a reboot to enter and
  a RESET to leave, no phone internet meanwhile, and no page reload.

**Verdict: BLE stays the real-time control path.** The connection-interval fix is
the worthwhile change. Wi-Fi earns its place as the **bulk-transfer** channel (SD
uploads), where throughput matters and per-request setup is amortised, not as a
low-latency control path. Beating BLE over Wi-Fi would require the persistent /
datagram transport on a device-hosted page described above (WebSocket / WebRTC) —
recorded as future work, not pursued. The SoftAP experiment lives on the
`feat/low-latency-control` branch as the evidence; it is not part of the
mainline validator.

## Sources

- BLE round-trip bounded by the connection interval; NimBLE connection-parameter
  update model — `journal/topics/wireless.md`, `journal/decisions/007-latency-test-suite.md`.
- Local Network Access exempts only `fetch`/XHR (not WebSocket), and the existing
  Wi-Fi data-path design — `journal/decisions/002-wifi-file-transfer-lna.md`,
  `journal/topics/wireless.md` (Wi-Fi file transfer section).
- `NimBLEServer::updateConnParams` (interval units of 1.25 ms, timeout units of
  10 ms) — NimBLE-Arduino 2.x API, the version pinned in `firmware/platformio.ini`.
- Bundled `WebServer` has no keep-alive (HC_WAIT_CLOSE branch disabled for
  espressif/arduino-esp32 issue #3652) — confirmed in the installed
  `libraries/WebServer/src/WebServer.cpp`.
