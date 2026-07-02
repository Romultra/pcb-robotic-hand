// Rolling on-screen log. Kept tiny — appends to <pre id="log"> with timestamps.

const MAX_LINES = 400;
const buffer = [];

function el() { return document.getElementById("log"); }

export function log(...parts) {
    const ts = new Date().toISOString().substring(11, 23);
    const msg = parts.map(p => typeof p === "string" ? p : JSON.stringify(p)).join(" ");
    const line = `[${ts}] ${msg}`;
    buffer.push(line);
    if (buffer.length > MAX_LINES) buffer.shift();
    const e = el();
    if (e) {
        e.textContent = buffer.join("\n");
        e.scrollTop = e.scrollHeight;
    }
    // Also forward to console for desktop debugging.
    console.log(line);
}

export function clearLog() {
    buffer.length = 0;
    const e = el();
    if (e) e.textContent = "";
}
