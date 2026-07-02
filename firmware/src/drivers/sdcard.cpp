#include "sdcard.h"

#include <SD.h>
#include <SPI.h>
#include <string.h>

#include "config.h"

namespace sdcard {

namespace {
SPIClass g_spi(HSPI);
bool     g_mounted = false;

// Dedicated audio folder. The single source of the folder name; list_audio()
// and open_audio() are the only paths that reach the card for playback, and
// both hard-code this prefix so nothing outside it is ever reachable.
constexpr const char* kAudioDir = "/audio";

// Dedicated logs folder. Same idea as the audio folder: open_log() and
// list_logs() are the only paths that reach it, both hard-coding this prefix.
constexpr const char* kLogDir = "/logs";

// Upload state. Kept in the driver so a single open File handle survives
// across BLE callbacks while the host streams chunks.
File     g_up_file;
bool     g_up_active   = false;
uint32_t g_up_received = 0;
uint32_t g_up_total    = 0;

// CD pin behaviour: most push-push microSD sockets short CD to GND when a card
// is fully inserted. Driving the pin as INPUT_PULLUP and reading LOW = present.
constexpr int CD_PRESENT_LEVEL = LOW;

// SD-over-SPI clock ladder. ensure_mounted() keeps the first clock whose
// single-block (CMD24) write probe verifies — the path the upload uses. 20 MHz is
// confirmed good for single-block writes on PCB1; the original upload failure was
// the *multi-block* (CMD25) path, not the clock (see decision 011). The lower
// rungs are a fallback for a card that needs a slower clock. (Library caps 25 MHz.)
constexpr uint32_t SD_CLOCK_LADDER[] = { 20000000, 10000000, 4000000, 1000000 };
uint32_t g_mount_hz = 0;  // clock the card actually mounted + write-verified at

// One-sector write + read-back probe. SD.begin() only does the 400 kHz init
// handshake and a CSD read, so it can mount a card that then can't take writes
// (full / bad / needs a slower clock). Prove a real single-block write at the
// chosen clock — the path the upload uses — before trusting the mount.
bool sd_write_probe() {
    const char* path = "/.wrprobe";
    // static (BSS, not stack): mount can run from the NimBLE host task (a BLE
    // upload or the SD self-test call sdcard from inside the GATT callback),
    // whose task stack is small (~4 KB). 1 KB of stack buffers here overflows it
    // and resets the board. SD access is serialized, so one shared pair is fine.
    static uint8_t out[512], in[512];
    for (int i = 0; i < 512; ++i) out[i] = (uint8_t)(i * 7 + 1);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    const size_t w = f.write(out, sizeof(out));
    f.flush();
    f.close();
    if (w != sizeof(out)) { SD.remove(path); return false; }

    f = SD.open(path, FILE_READ);
    if (!f) { SD.remove(path); return false; }
    const size_t r = f.read(in, sizeof(in));
    f.close();
    SD.remove(path);
    return r == sizeof(in) && memcmp(out, in, sizeof(out)) == 0;
}

bool ensure_mounted() {
    if (g_mounted) return true;
    for (uint32_t hz : SD_CLOCK_LADDER) {
        g_spi.begin(cfg::SD_SCLK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
        if (SD.begin(cfg::SD_CS_PIN, g_spi, hz) && sd_write_probe()) {
            g_mounted  = true;
            g_mount_hz = hz;
            Serial.printf("[sdcard] mounted, write-verified at %u kHz\n",
                          (unsigned)(g_mount_hz / 1000));
            return true;
        }
        Serial.printf("[sdcard] mount/write probe failed at %u kHz\n",
                      (unsigned)(hz / 1000));
        SD.end();
        g_spi.end();
    }
    Serial.println("[sdcard] mount failed at every clock in the ladder");
    return false;
}

// Creates every intermediate directory of an absolute file path (mkdir -p).
// FatFS does not auto-create parents on SD.open(FILE_WRITE), so an upload to
// e.g. "/audio/clip.wav" would fail to open whenever "/audio" doesn't already
// exist. Walk each "/"-separated prefix and mkdir it if missing. The final
// path component (the file name) is left for SD.open to create.
void ensure_parent_dirs(const String& full) {
    int slash = full.indexOf('/', 1);     // skip the leading root slash
    while (slash > 0) {
        const String dir = full.substring(0, slash);
        if (dir.length() > 0 && !SD.exists(dir)) SD.mkdir(dir);
        slash = full.indexOf('/', slash + 1);
    }
}
}

void init() {
    pinMode(cfg::SD_CD_PIN, INPUT_PULLUP);
    pinMode(cfg::SD_CS_PIN, OUTPUT);
    digitalWrite(cfg::SD_CS_PIN, HIGH);  // deselect at boot
    Serial.printf("[sdcard] init done; card %s\n",
                  is_card_present() ? "PRESENT" : "ABSENT");
}

bool is_card_present() {
    return digitalRead(cfg::SD_CD_PIN) == CD_PRESENT_LEVEL;
}

TestResult run_self_test() {
    TestResult out = { Result::OK, 0, 0 };

    if (!is_card_present()) {
        out.result = Result::NoCard;
        Serial.println("[sdcard] self_test: no card inserted");
        return out;
    }

    // Tear down any previous mount before remounting — safe to call repeatedly.
    if (g_up_active) {
        g_up_file.close();
        g_up_active = false;
    }
    if (g_mounted) {
        SD.end();
        g_mounted = false;
    }

    if (!ensure_mounted()) {
        out.result = Result::MountFail;
        Serial.println("[sdcard] self_test: SD.begin() failed");
        return out;
    }

    const uint8_t card_type = SD.cardType();
    if (card_type == CARD_NONE) {
        out.result = Result::NoCard;
        Serial.println("[sdcard] self_test: cardType == NONE post-mount");
        return out;
    }

    // Write + read a small marker file. Failure = filesystem unreadable.
    const char* path = "/pcb1_validator_test.txt";
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        out.result = Result::FsUnreadable;
        Serial.println("[sdcard] self_test: open(W) failed");
        return out;
    }
    f.printf("PCB1 validator self-test, uptime_ms=%lu\n", (unsigned long)millis());
    f.close();

    f = SD.open(path, FILE_READ);
    if (!f) {
        out.result = Result::FsUnreadable;
        Serial.println("[sdcard] self_test: open(R) failed");
        return out;
    }
    const String contents = f.readString();
    f.close();
    if (contents.length() == 0) {
        out.result = Result::FsUnreadable;
        Serial.println("[sdcard] self_test: empty read-back");
        return out;
    }

    out.size_mb = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
    out.free_mb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
    Serial.printf("[sdcard] self_test OK; %u MB total, %u MB free\n",
                  out.size_mb, out.free_mb);
    return out;
}

// --- Upload state machine --------------------------------------------------

UploadProgress upload_begin(const String& path, uint32_t total_size) {
    UploadProgress p = { UploadStatus::Ready, 0 };

    // Close any prior in-flight upload before starting a new one.
    if (g_up_active) {
        g_up_file.close();
        g_up_active = false;
    }
    g_up_received = 0;
    g_up_total    = 0;

    if (!is_card_present()) {
        p.status = UploadStatus::NoCard;
        Serial.println("[sdcard] upload_begin: no card");
        return p;
    }
    if (!ensure_mounted()) {
        p.status = UploadStatus::MountFail;
        Serial.println("[sdcard] upload_begin: mount failed");
        return p;
    }

    String full = path;
    if (full.length() == 0) {
        p.status = UploadStatus::OpenFail;
        return p;
    }
    if (full[0] != '/') full = "/" + full;

    // Make sure the destination folder exists, else FatFS open(W) just fails.
    ensure_parent_dirs(full);

    g_up_file = SD.open(full.c_str(), FILE_WRITE);
    if (!g_up_file) {
        p.status = UploadStatus::OpenFail;
        Serial.printf("[sdcard] upload_begin: open(W) failed for %s\n", full.c_str());
        return p;
    }
    g_up_active = true;
    g_up_total  = total_size;
    Serial.printf("[sdcard] upload_begin: %s (%u bytes)\n", full.c_str(),
                  (unsigned)total_size);
    return p;
}

UploadProgress upload_chunk(const uint8_t* data, size_t len) {
    UploadProgress p = { UploadStatus::ChunkOk, g_up_received };
    if (!g_up_active) {
        p.status = UploadStatus::ProtocolErr;
        return p;
    }
    // Write one 512-byte sector at a time. A single f.write() of the whole 4 KB
    // chunk makes FatFS issue an SD *multi-block* write (CMD25), whose post-write
    // SEND_STATUS (CMD13) token-errors on this card regardless of clock, while the
    // *single-block* path (CMD24) is solid (verified up to 20 MHz). Feeding the FS
    // one aligned sector keeps every write on that working path. Each sector
    // retries a couple of times on a transient short write.
    size_t off = 0;
    while (off < len) {
        const size_t piece = (len - off) < 512 ? (len - off) : 512;
        size_t w = 0;
        for (int attempt = 0; attempt < 3 && w < piece; ++attempt) {
            if (attempt > 0) delay(2);
            w += g_up_file.write(data + off + w, piece - w);
        }
        if (w != piece) {
            g_up_file.close();
            g_up_active = false;
            p.status = UploadStatus::WriteFail;
            Serial.printf("[sdcard] upload_chunk: short write %u/%u at +%u\n",
                          (unsigned)w, (unsigned)piece,
                          (unsigned)(g_up_received + off));
            return p;
        }
        off += piece;
    }
    g_up_received += (uint32_t)off;
    p.bytes_received = g_up_received;
    return p;
}

UploadProgress upload_end() {
    UploadProgress p = { UploadStatus::Complete, g_up_received };
    if (!g_up_active) {
        p.status = UploadStatus::ProtocolErr;
        return p;
    }
    g_up_file.flush();
    g_up_file.close();
    g_up_active = false;
    Serial.printf("[sdcard] upload_end: %u bytes committed\n",
                  (unsigned)g_up_received);
    return p;
}

UploadProgress upload_abort() {
    UploadProgress p = { UploadStatus::Aborted, g_up_received };
    if (g_up_active) {
        g_up_file.close();
        g_up_active = false;
        Serial.println("[sdcard] upload_abort: handle closed");
    }
    return p;
}

// --- Dedicated audio folder ------------------------------------------------

namespace {
// Strips any directory part so only the base name leaves the driver. Different
// arduino-esp32 versions return File::name() as either the full path or just
// the base name, so normalise here.
String base_name(const char* n) {
    String s(n);
    int slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.substring(slash + 1);
    return s;
}

// Bare-filename guard shared by every dedicated-folder accessor: a name must be
// non-empty and free of path separators and parent-dir refs, so it can never
// escape the folder it is joined onto.
bool name_is_safe(const String& name) {
    return name.length() > 0 &&
           name.indexOf('/')  < 0 &&
           name.indexOf('\\') < 0 &&
           name.indexOf("..") < 0;
}

// Non-recursive list of the regular files directly inside one folder, creating
// it if absent. Never recurses, never returns directories, never escapes the
// folder. Empty on no-card / unmountable / not-a-directory.
std::vector<AudioFile> list_dir(const char* dir) {
    std::vector<AudioFile> out;
    if (!is_card_present() || !ensure_mounted()) return out;

    // Create the folder if it doesn't exist yet so an empty list is clean
    // (mkdir is a no-op / harmless if it already exists).
    if (!SD.exists(dir)) SD.mkdir(dir);

    File d = SD.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return out;
    }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory()) {
            out.push_back({ base_name(f.name()), (uint32_t)f.size() });
        }
        f.close();
    }
    d.close();
    return out;
}
}  // namespace

std::vector<AudioFile> list_audio() {
    std::vector<AudioFile> out = list_dir(kAudioDir);
    Serial.printf("[sdcard] list_audio: %u file(s) in %s\n",
                  (unsigned)out.size(), kAudioDir);
    return out;
}

File open_audio(const String& name) {
    // Reject anything that could escape the dedicated folder. name must be a
    // bare filename: no separators, no parent-dir refs, non-empty.
    if (!name_is_safe(name)) {
        Serial.printf("[sdcard] open_audio: rejected name '%s'\n", name.c_str());
        return File();
    }
    if (!is_card_present() || !ensure_mounted()) return File();

    String full = String(kAudioDir) + "/" + name;
    File f = SD.open(full.c_str(), FILE_READ);
    if (!f) {
        Serial.printf("[sdcard] open_audio: open failed for %s\n", full.c_str());
        return File();
    }
    Serial.printf("[sdcard] open_audio: %s (%u bytes)\n", full.c_str(),
                  (unsigned)f.size());
    return f;
}

// --- Dedicated logs folder -------------------------------------------------

File open_log(const String& name, bool append) {
    if (!name_is_safe(name)) {
        Serial.printf("[sdcard] open_log: rejected name '%s'\n", name.c_str());
        return File();
    }
    if (!is_card_present() || !ensure_mounted()) return File();

    if (!SD.exists(kLogDir)) SD.mkdir(kLogDir);

    String full = String(kLogDir) + "/" + name;
    File f = SD.open(full.c_str(), append ? FILE_APPEND : FILE_WRITE);
    if (!f) {
        Serial.printf("[sdcard] open_log: open(%s) failed for %s\n",
                      append ? "A" : "W", full.c_str());
        return File();
    }
    return f;
}

std::vector<AudioFile> list_logs() {
    std::vector<AudioFile> out = list_dir(kLogDir);
    Serial.printf("[sdcard] list_logs: %u file(s) in %s\n",
                  (unsigned)out.size(), kLogDir);
    return out;
}

uint32_t free_mb() {
    if (!is_card_present() || !ensure_mounted()) return 0;
    return (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
}

}  // namespace sdcard
