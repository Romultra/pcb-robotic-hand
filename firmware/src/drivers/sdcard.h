#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

namespace sdcard {

enum class Result : uint8_t {
    OK            = 0,
    NoCard        = 1,
    SpiFail       = 2,
    MountFail     = 3,
    FsUnreadable  = 4,
};

struct TestResult {
    Result   result;
    uint32_t size_mb;    // 0 if not OK
    uint32_t free_mb;    // 0 if not OK
};

void init();

// Reads GPIO48 (CD): true if a card is mechanically inserted, false otherwise.
// Does NOT mount the FS.
bool is_card_present();

// Mounts the card, writes a small marker file, reads it back, and reports
// success or a specific failure mode. Single-shot — no retry loops.
TestResult run_self_test();

// --- File upload (chunked over BLE) ----------------------------------------
// The host streams a file in three phases: begin → many chunks → end. Each
// call returns the current status and total bytes written so far. The driver
// keeps a single File handle open between calls; calling begin again, abort,
// or end closes it.

enum class UploadStatus : uint8_t {
    Ready        = 0x00,  // begin accepted, ready for chunks
    ChunkOk      = 0x01,  // chunk written
    Complete     = 0x02,  // end accepted, file closed
    NoCard       = 0x80,
    MountFail    = 0x81,
    OpenFail     = 0x82,
    WriteFail    = 0x83,
    ProtocolErr  = 0x84,  // chunk/end with no upload in progress, etc.
    Aborted      = 0x85,
};

struct UploadProgress {
    UploadStatus status;
    uint32_t     bytes_received;  // running total since begin
};

// path must be non-empty; leading '/' added if missing. Overwrites any
// existing file at that path. total_size is informational (logged but not
// enforced — short or long uploads still succeed at end()).
UploadProgress upload_begin(const String& path, uint32_t total_size);
UploadProgress upload_chunk(const uint8_t* data, size_t len);
UploadProgress upload_end();
UploadProgress upload_abort();

// --- Dedicated audio folder ------------------------------------------------
// Audio playback is confined to a single folder on the card. Both functions
// below are the ONLY way the audio driver reaches the filesystem, and neither
// can ever name a path outside that folder — this is what keeps the file list
// and playback "strictly only from the dedicated folder".

struct AudioFile {
    String   name;   // base name only, no path
    uint32_t size;   // bytes
};

// Lists the regular files directly inside the dedicated audio folder (creating
// the folder if absent). Never recurses, never returns directories, and never
// exposes anything outside the folder. Returns empty if the card is absent or
// unmountable.
std::vector<AudioFile> list_audio();

// Opens a file by bare name inside the dedicated audio folder for reading.
// Rejects empty names and any name containing '/', '\\', or "..", so playback
// can never escape the folder. Returns a falsy File on reject/missing/no-card.
File open_audio(const String& name);

// --- Dedicated logs folder -------------------------------------------------
// Exercise position logs live in their own folder, reached only through the
// two functions below. They use the same bare-filename guard as the audio
// folder, so a log file can never escape /logs either.

// Opens a file by bare name inside the dedicated logs folder for writing
// (append = true keeps existing contents, e.g. the manifest; append = false
// truncates / creates a fresh per-session file). Creates the folder if absent.
// Rejects path-escaping names. Returns a falsy File on reject/no-card.
File open_log(const String& name, bool append);

// Lists the regular files directly inside the dedicated logs folder. Same
// non-recursive, folder-locked behaviour as list_audio(). For the retrieval UI.
std::vector<AudioFile> list_logs();

// Free space on the mounted card in MiB (0 if no card / unmountable). Used by
// the logging status notification.
uint32_t free_mb();

}  // namespace sdcard
