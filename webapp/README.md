# webapp — PCB1 validation app

Minimal HTML/JS validation tool. Pairs with the firmware in `../firmware/` over
**Web Bluetooth** and gives one control panel per board subsystem plus a
rolling log.

## Browser compatibility

Web Bluetooth is supported on:

- **Chrome on Android** ✓ (the intended target)
- Chrome / Edge on Windows, macOS, ChromeOS, Linux ✓
- **Not** Safari (iOS or macOS) ✗
- **Not** Firefox ✗

The app exits early with a visible warning if `navigator.bluetooth` is missing.

## Run locally

Web Bluetooth requires a **secure context**: HTTPS or `localhost`. The easiest
local-dev path is a plain Python HTTP server, accessed via `localhost`:

```pwsh
cd webapp
python -m http.server 8080
```

Then open `http://localhost:8080/` in Chrome on the dev machine, or use Chrome
DevTools remote debugging to access localhost from a tethered Android phone.

For testing on a phone over Wi-Fi without tethering, the page must be served
from an HTTPS origin — see deployment below.

## Deploy (GitHub Pages)

The simplest HTTPS hosting for a static page.

1. **Automated (this repo)** — `.github/workflows/pages.yml` deploys `webapp/`
   to Pages on every push to `main` that touches `webapp/**`. One-time setup:
   go to **Settings → Pages** and set **Source = GitHub Actions**. The site
   appears at `https://<owner>.github.io/<repo>/`. Requires the repo to be
   public, or a paid GitHub plan for Pages on private repos. You can also
   trigger a redeploy manually from the **Actions** tab (`pages` →
   *Run workflow*).
2. **Separate public repo** — copy the contents of `webapp/` into a new
   repo dedicated to the page and enable Pages there. Useful if the thesis
   repo stays private.

After deploy, just open the Pages URL in Chrome on Android, tap **Connect**,
and pair with the board (advertising as `PCB1-Validator`).

## Files

```
index.html       single page; sections per subsystem
style.css        light + dark theme via prefers-color-scheme
js/ble.js        Web Bluetooth wrapper (connect, characteristic cache, writes, notifications)
js/log.js        rolling on-screen log
js/status.js     status-notify dashboard
js/servos.js     5 sliders + power toggle + sweep
js/buzzer.js     frequency + duration form
js/audio.js      preset-tone buttons + volume slider + SD file play
js/sd.js         self-test trigger + result decode + file upload (Wi-Fi or BLE)
js/wifi.js       scan list + STA-connect form + reconnect-saved + disconnect
js/net.js        shared board-IP state for the Wi-Fi upload path
js/latency.js    latency panel: BLE round-trip ping (+ Wi-Fi round-trip), hand-tracking inference time
js/main.js       wires everything to the DOM
```

UUIDs in `js/ble.js` must match those in `../firmware/src/ble.h` — keep them in
sync if either is touched.

## Fast SD upload over Wi-Fi

The chunk-acked BLE upload is reliable but slow. When the board has joined a
network (via the **Wi-Fi** panel), the **microSD** panel's upload instead POSTs
the file straight to the board's HTTP server over Wi-Fi (far faster), falling
back to BLE automatically if Wi-Fi isn't up or the request fails. BLE stays the
control channel and is how the app learns the board's IP. The hint under the
upload row shows which path will be used.

This works because **Chrome ≥142** ([Local Network Access](https://developer.chrome.com/blog/local-network-access))
lets this HTTPS page reach the board's plain-HTTP endpoint at its private IP,
after a **one-time "access devices on your local network" permission** prompt.
If that permission is denied (or the board is unreachable), the upload silently
falls back to BLE. iOS/Firefox don't support Web Bluetooth, so the whole app is
Chrome-on-Android only regardless. Design rationale:
`../journal/decisions/002-wifi-file-transfer-lna.md`.
