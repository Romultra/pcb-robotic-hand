# 011: Making the Wi-Fi SD upload fast and reliable

**Date:** 2026-06-24
**Status:** Decided

## Context

The SD file upload was both slow and failing most of the time. Tracing the path
end to end (`webapp/js/sd.js` → board HTTP server `firmware/src/drivers/webfs.cpp`
→ `sdcard::upload_*`) turned up several independent problems, not one:

1. **BLE/Wi-Fi coexistence starved the transfer.** The S3 has a single 2.4 GHz
   radio time-shared between BLE and Wi-Fi. On connect the firmware requests a
   tight 7.5–15 ms BLE interval with slave latency 0 (decision 008) for control
   responsiveness, which means BLE seizes a radio slot every 7.5 ms whether or
   not it has data. Decision 008 already measured the cost on a single ping
   (Wi-Fi p95 23 ms → 88 ms, jitter σ 3.1 → 29.5 with BLE up). For a *sustained*
   upload it is far worse: fragmented airtime plus delayed TCP ACKs collapse
   throughput, and a long enough stall trips the bundled `WebServer`'s 5 s body
   read timeout, which fails the whole upload.

2. **A deterministic open failure.** `sdcard::upload_begin` opened the
   destination with `SD.open(path, FILE_WRITE)` but never created the parent
   folder, and FatFS does not auto-create parents. So an upload to
   `/audio/clip.wav` (the actual use case, since playback is locked to `/audio`)
   failed to open whenever `/audio` did not already exist, returning OpenFail →
   HTTP 500 → fall back to the slow BLE path.

3. **Brittle write + opaque errors.** A single short SD write failed the whole
   transfer with no retry, and both the HTTP 500 body and the webapp surfaced a
   generic "upload failed" / "network error" with no reason, so a failure was
   undiagnosable from the app.

## Options Considered

1. **Swap the bundled `WebServer` for `ESPAsyncWebServer`.** This is the
   "proper" fix flagged as the upgrade path in decisions 002 and 008: real async
   streaming, backpressure, no main-loop blocking. Rejected *for now*: it adds
   `AsyncTCP` + the library as dependencies and moves the SD writes into the
   AsyncTCP task, none of which can be validated without bench time, and the
   bring-up window had no hardware access. The synchronous server's only real
   problem during a bulk upload is that it blocks the main loop (already deemed
   acceptable, BLE just pauses) and that it times out when the radio is starved
   — and the radio starvation has a cheaper, verifiable fix. Kept as future work.

2. **Relax the BLE link during the upload (chosen for coexistence).** While a
   Wi-Fi upload runs, BLE carries no traffic (control is paused), so there is no
   reason to hold the tight control interval. Request a relaxed interval
   (30–50 ms) with a slave latency of 4 via `NimBLEServer::updateConnParams`,
   then restore the tight 7.5–15 ms set when the upload finishes. This hands
   almost all radio airtime to Wi-Fi for the transfer and reverts to a responsive
   control link immediately after. It is strictly safe: best case a large Wi-Fi
   speed-up, worst case the central declines the request and nothing changes. The
   link stays well inside the 4.0 s supervision timeout: (1+4) × 50 ms × 2 ≈
   0.5 s. Preferred over `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` because
   that API is deprecated, and decision 008 already showed runtime radio
   reconfiguration on this board is risky (`setSleep(false)` reset it); a
   connection-parameter request touches only the link, not the radio/PM state.

3. **Create parent directories on upload (chosen).** `upload_begin` now walks the
   path and `mkdir`s each missing prefix (mkdir -p) before opening the file, so
   uploads into `/audio`, `/logs`, or any nested folder just work.

4. **Retry transient failures instead of failing hard (chosen).** A short SD
   write is retried a couple of times (a busy card often takes the rest on a
   second try) before giving up. The webapp retries the whole Wi-Fi upload up to
   three times with a short backoff before dropping to BLE, so one coexistence or
   card hiccup no longer banishes the transfer to the slow path.

## Decision

A package of verifiable changes, keeping the synchronous `WebServer`:

**Firmware.**
- `config.h`: relaxed bulk-transfer connection parameters
  (`BLE_CONN_ITVL_MIN_BULK`=24 → 30 ms, `MAX_BULK`=40 → 50 ms,
  `LATENCY_BULK`=4), alongside the existing tight set.
- `ble.{h,cpp}`: `set_bulk_transfer(bool)` requests the relaxed params while a
  Wi-Fi upload is active and restores the tight set afterward, tracking the
  connection handle from `onConnect`. A deadline backstop in `ble::loop()`
  restores the tight interval if an upload never signals completion, and a
  disconnect resets the state.
- `webfs.cpp`: the upload callback calls `ble::set_bulk_transfer(true)` on
  `UPLOAD_FILE_START` and `false` from `handle_upload_done` (and on abort). The
  500 response now names the actual `sdcard::UploadStatus` (no card, mount,
  open, write, …) instead of a generic string.
- `sdcard.cpp`: `ensure_parent_dirs()` (mkdir -p) before `SD.open` in
  `upload_begin`; `upload_chunk` retries a short write up to three times.
- `platformio.ini`: `-DHTTP_UPLOAD_BUFLEN=4096` (8 × 512-byte sectors), so the
  body is handed to SD in larger writes — fewer SD round trips, better
  throughput. The macro is `#ifndef`-guarded upstream, so the override is clean.

**Webapp (`sd.js`).** The Wi-Fi upload retries up to three times with a short
backoff before falling back to BLE, and a failed POST now surfaces the firmware's
`{"error":"<reason>"}` body (or the HTTP status) instead of a bare "network
error".

Firmware builds clean (`pio run -e pcb1`, flash 31.2 %, RAM 15.9 %). UUID-sync
unaffected (no new characteristic).

## Bring-up findings (2026-06-25)

First bench run on PCB1 after the changes above. The mkdir-p fix was confirmed
working (`upload_begin: /audio/crab-v2.wav` now opens cleanly), but the upload
still failed — and the serial log showed the **real** root cause, which was not
what the first round assumed:

```
[W][sd_diskio.cpp:186] sdCommand(): token error [13] 0x23
[sdcard] upload_chunk: short write 0/4096 after retries
```

`0x23` is not a valid SD SPI data-response token, so this is a write-path
failure, not a full or locked card. The card mounts fine because mounting only
does the 400 kHz init handshake and a CSD read. The first guess was a clock /
signal-integrity margin, but that proved wrong — the passes below pin it on the
multi-block write path, independent of clock. The per-chunk write retry could not
help.

Two more things the run exposed: the bundled `WebServer` read the **entire** body
(a 28 MB WAV) before sending the 500, so the first attempt hit the 120 s XHR
timeout and a real write error took ~2 minutes to surface; and the upload ran at
only ~200 kB/s, which is the `WebServer`'s byte-at-a-time `_uploadReadByte`
ceiling (it was discarding the body here since writes failed).

**Follow-up fixes (this revision of the decision):**
- **SD clock is now self-tuning with a write probe.** `ensure_mounted()` walks a
  clock ladder (10 → 4 → 1 MHz) and keeps the first clock that passes a real
  one-sector write + read-back probe, since `SD.begin()` succeeding only proves
  reads work. (At this point 20 MHz was dropped from the ladder on the wrong
  assumption it was a clock problem; a later pass put it back — see below.) The
  verified clock is logged (`mounted, write-verified at N kHz`), and a card that
  fails the probe at every clock now reports "mount failure" rather than a
  misleading "write failure".
- **Fast-fail.** On a write error the upload callback now drops the BLE bulk mode
  and closes the TCP connection immediately instead of draining the rest of the
  body, so a genuine failure surfaces in seconds, not minutes.
- **Timeout sized to the file.** The webapp's XHR timeout scales to a
  conservative ~50 kB/s floor (2 min minimum), so a large but healthy upload is
  not killed mid-flight.

**Second pass — the clock ladder was necessary but not sufficient.** Re-flashed:
the card now write-verifies at 10 MHz, but the upload still failed with the same
`token error [13] 0x23`. Decoding it against the driver source (`sd_diskio.cpp`):
the `[13]` is the *command*, CMD13 (`SEND_STATUS`), the status poll the driver
runs after a write, and `0x23` is the bad R1 byte read back. The mount probe
writes a single sector, which routes to `WRITE_BLOCK_SINGLE` (CMD24,
`sdWriteSector`) and passes; a 4 KB upload chunk written in one `f.write()` makes
FatFS issue a *multi-block* write (CMD25, `sdWriteSectors`, 8 sectors), and it is
that path's `SEND_STATUS` that token-errors. So the single-block path is solid at
10 MHz and the multi-block path is not. Fix: `upload_chunk` now writes one aligned
512-byte sector per `f.write()`, so every write stays on the working single-block
path and FatFS never issues CMD25. Throughput is unaffected — the `WebServer`
read, not SD, is the ceiling, and single-block at 10 MHz still clears > 200 kB/s.

**Third pass — a reset regression, now fixed.** With single-block writes the
Wi-Fi upload completed end to end (the full 28 MB committed, ~117 kB/s), but the
**SD self-test and the BLE upload then reset the board**. Cause: the mount
write-probe declared two 512-byte buffers on the *stack*, and both of those paths
call the SD driver from inside the NimBLE GATT callback (the host task, ~4 KB
stack), so 1 KB of probe buffers overflowed it. The Wi-Fi upload never tripped
this because it mounts from the main loop task (large stack). Fix: the probe
buffers are now `static` (BSS, off the stack); SD access is serialized so sharing
one pair is safe.

**Resolution — the clock was a red herring.** Re-tested with 20 MHz back at the
top of the ladder: the probe write-verified at 20 MHz and a full 28 MB upload
completed clean (~122 kB/s, essentially the same as 10 MHz). So single-block
writes are reliable at 20 MHz, which confirms the original failure was purely the
multi-block (CMD25) `SEND_STATUS` path, not signal integrity and not the clock.
PCB1 now runs the card at 20 MHz on the single-block path; the earlier
signal-integrity / future-revision-layout framing was wrong and has been dropped.

**Still open / future work.** Throughput is ~120 kB/s, set by single-block SD
write overhead plus the bundled `WebServer`'s byte-at-a-time read — not the radio
or the SPI clock (20 vs 10 MHz barely moved it). Fine for realistic short nudge
clips (well under 1 MB, a second or two); a multi-MB file is slow (the 28 MB
stress WAV takes ~4 min). The two levers are the `ESPAsyncWebServer` path (read
throughput) and getting multi-block (CMD25) writes working (recovers the
per-sector overhead) — the latter a card/library SPI multi-block quirk to
investigate, not a PCB hardware issue. Both are future work; single-block at
20 MHz is the reliable validator behaviour.

## Sources

- Coexistence cost and the tight-interval rationale —
  `journal/decisions/008-low-latency-control-path.md`.
- Wi-Fi data-path design, bundled `WebServer` choice, and `ESPAsyncWebServer`
  noted as the upgrade path — `journal/decisions/002-wifi-file-transfer-lna.md`.
- `NimBLEServer::updateConnParams` (interval units 1.25 ms, timeout units 10 ms)
  — NimBLE-Arduino 2.x, pinned in `firmware/platformio.ini`.
- FatFS does not create parent directories on open; `HTTP_UPLOAD_BUFLEN` is
  `#ifndef`-guarded — confirmed in the installed arduino-esp32 2.0.16
  `libraries/WebServer/src/WebServer.h` and SD library.
