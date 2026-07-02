#include "audio.h"

#include <FS.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "drivers/sdcard.h"

namespace audio {

namespace {

constexpr i2s_port_t  I2S_PORT       = I2S_NUM_0;
constexpr int         SAMPLE_RATE_HZ = 16000;
constexpr int         CHUNK_SAMPLES  = 256;

// What the driver is currently sourcing audio from.
enum class Source : uint8_t { None, Preset, File };

// Active playback state.
Source   g_source       = Source::None;
bool     g_playing      = false;
int      g_cur_rate     = SAMPLE_RATE_HZ;  // I²S clock as last configured

// Master output volume, 0.0..1.0. Set over BLE; a single aligned float store,
// so it's safe to write from the host task and read here on the loop task
// without locking. Scales every sample digitally (the MAX98357A has fixed gain).
volatile float g_volume = 1.0f;

// Volume persistence. set_volume() updates g_volume immediately (RAM) and marks
// it dirty; the NVS write is debounced in tick() so dragging the slider doesn't
// write flash on every step. Stored as a single percentage byte.
constexpr const char* NVS_NS  = "audio";
constexpr const char* NVS_KEY = "vol";
constexpr uint32_t    VOL_SAVE_DELAY_MS = 1500;
volatile uint8_t  g_vol_pct        = 100;   // last requested %, for the deferred save
volatile bool     g_vol_dirty      = false;
volatile uint32_t g_vol_changed_at = 0;

// Preset (synthesised) state.
uint8_t  g_preset       = 0;
uint32_t g_samples_done = 0;
uint32_t g_total_samples = 0;

// SD WAV-file state. The File handle is opened, read, and closed only on the
// main loop task (play_file + tick), so no locking is needed.
File     g_file;
uint8_t  g_channels      = 1;   // 1 = mono, 2 = stereo (downmixed to mono out)
uint32_t g_data_remaining = 0;  // bytes of PCM data left to stream

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void i2s_install(int rate);   // defined below

// Switches the I²S sample rate when it actually changes. Done by reinstalling
// the driver (not i2s_set_clk): with the ONLY_LEFT install, set_clk's channel
// argument disagrees with the framing and clocks the wrong rate, which played
// files back at the wrong speed. Reinstalling reuses the exact config the
// (correct-sounding) presets run under, just at a different rate. Output is
// always single-channel — stereo files are downmixed. Called only on the main
// loop task, after playback is stopped, so no write is in flight.
void set_rate(int rate) {
    if (rate == g_cur_rate) return;
    i2s_driver_uninstall(I2S_PORT);
    i2s_install(rate);
    g_cur_rate = rate;
}

// Stops playback, releases any open file, and silences the DMA buffer.
void stop_internal() {
    if (g_file) g_file.close();
    g_source         = Source::None;
    g_playing        = false;
    g_data_remaining = 0;
    i2s_zero_dma_buffer(I2S_PORT);
}

void i2s_install(int rate) {
    i2s_config_t cfg_i2s = {};
    cfg_i2s.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg_i2s.sample_rate = rate;
    cfg_i2s.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg_i2s.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // MAX98357A is mono
    cfg_i2s.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg_i2s.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    // 8 buffers × 256 frames ≈ 46 ms cushion at 44.1 kHz, ~128 ms at 16 kHz —
    // enough to ride out SD-read latency spikes without underrunning.
    cfg_i2s.dma_buf_count = 8;
    cfg_i2s.dma_buf_len = CHUNK_SAMPLES;
    cfg_i2s.use_apll = false;
    cfg_i2s.tx_desc_auto_clear = true;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = cfg::I2S_BCLK_PIN;
    pins.ws_io_num  = cfg::I2S_LRCLK_PIN;
    pins.data_out_num = cfg::I2S_DIN_PIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    i2s_driver_install(I2S_PORT, &cfg_i2s, 0, nullptr);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// Write one chunk of samples for the active preset and advance the cursor.
// Returns false when the preset has finished playing.
bool render_chunk() {
    if (g_samples_done >= g_total_samples) {
        return false;
    }

    int16_t buf[CHUNK_SAMPLES];
    const uint32_t to_render = min<uint32_t>(CHUNK_SAMPLES, g_total_samples - g_samples_done);

    for (uint32_t i = 0; i < to_render; ++i) {
        const uint32_t n = g_samples_done + i;
        const float    t = (float)n / SAMPLE_RATE_HZ;
        float          sample = 0.0f;
        const float    progress = (float)n / (float)g_total_samples;

        switch (g_preset) {
            case 1: {  // 1 kHz sine
                sample = sinf(2 * PI * 1000.0f * t);
                break;
            }
            case 2: {  // chirp 500 Hz → 4 kHz
                const float f = 500.0f + (4000.0f - 500.0f) * progress;
                sample = sinf(2 * PI * f * t);
                break;
            }
            case 3: {  // success jingle: C5 → E5 → G5, ~200 ms each
                float f = 523.0f;
                if (progress > 0.66f)      f = 784.0f;
                else if (progress > 0.33f) f = 659.0f;
                sample = sinf(2 * PI * f * t);
                break;
            }
            case 4: {  // fail tone: low square 200 Hz
                sample = (fmodf(200.0f * t, 1.0f) > 0.5f) ? -1.0f : 1.0f;
                break;
            }
            default:
                sample = 0.0f;
                break;
        }

        // Soft fade in/out (10 ms each) to avoid clicks at the speaker.
        constexpr float FADE_S = 0.010f;
        const float fade_in  = min(1.0f, t / FADE_S);
        const float time_to_end_s = (g_total_samples - n) / (float)SAMPLE_RATE_HZ;
        const float fade_out = min(1.0f, time_to_end_s / FADE_S);
        sample *= fade_in * fade_out * 0.5f * g_volume;  // ~50% headroom × master volume

        buf[i] = (int16_t)(sample * 32767.0f);
    }

    // Pad remainder of buffer with zeros if last chunk is short.
    for (uint32_t i = to_render; i < CHUNK_SAMPLES; ++i) buf[i] = 0;

    size_t written = 0;
    i2s_write(I2S_PORT, buf, CHUNK_SAMPLES * sizeof(int16_t), &written, portMAX_DELAY);
    g_samples_done += to_render;
    return true;
}

// --- WAV file playback -----------------------------------------------------

struct WavInfo {
    uint32_t sample_rate = 0;
    uint16_t channels    = 0;
    uint16_t bits        = 0;
    uint32_t data_size   = 0;   // bytes of PCM data
};

// Parses a canonical RIFF/WAVE header, walking subchunks until "data". Leaves
// the file cursor at the first PCM byte on success. Only validates structure;
// the caller checks bits/channels are supported.
bool parse_wav(File& f, WavInfo& out) {
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    while (true) {
        uint8_t ch[8];
        if (f.read(ch, 8) != 8) return false;
        const uint32_t csize      = le32(&ch[4]);
        const uint32_t data_start = f.position();

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (csize < 16 || f.read(fmt, 16) != 16) return false;
            const uint16_t audio_format = le16(&fmt[0]);
            out.channels    = le16(&fmt[2]);
            out.sample_rate = le32(&fmt[4]);
            out.bits        = le16(&fmt[14]);
            if (audio_format != 1) return false;   // PCM only
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) return false;
            out.data_size = csize;
            f.seek(data_start);   // PCM starts here
            return true;
        }
        // Advance to the next chunk (chunks are word-aligned).
        f.seek(data_start + csize + (csize & 1));
    }
}

// Streams one chunk of the open WAV file to I²S. Returns false at end-of-data.
bool stream_file_chunk() {
    const uint32_t bytes_per_frame = 2u * g_channels;   // 16-bit samples
    const uint32_t frames = min<uint32_t>(CHUNK_SAMPLES, g_data_remaining / bytes_per_frame);
    if (frames == 0) return false;

    int16_t raw[CHUNK_SAMPLES * 2];   // interleaved, worst case stereo
    const size_t want = frames * bytes_per_frame;
    const int    got  = g_file.read((uint8_t*)raw, want);
    if (got <= 0) return false;
    const uint32_t got_frames = (uint32_t)got / bytes_per_frame;
    if (got_frames == 0) return false;
    g_data_remaining -= got_frames * bytes_per_frame;

    int16_t mono[CHUNK_SAMPLES];
    constexpr float GAIN = 0.8f;        // headroom so loud files don't hard-clip
    const float vol = GAIN * g_volume;  // × master volume
    for (uint32_t i = 0; i < got_frames; ++i) {
        int32_t s = (g_channels == 2)
            ? ((int32_t)raw[2 * i] + (int32_t)raw[2 * i + 1]) / 2
            : (int32_t)raw[i];
        s = (int32_t)(s * vol);
        if (s > 32767)       s = 32767;
        else if (s < -32768) s = -32768;
        mono[i] = (int16_t)s;
    }

    size_t written = 0;
    i2s_write(I2S_PORT, mono, got_frames * sizeof(int16_t), &written, portMAX_DELAY);
    return true;
}

// Restore the saved volume on boot. Defaults to 100 % when the key was never
// written (getUChar returns the default). Called once from init().
void load_volume() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return;
    const uint8_t pct = prefs.getUChar(NVS_KEY, 100);
    prefs.end();
    if (pct <= 100) {
        g_vol_pct = pct;
        g_volume  = (float)pct / 100.0f;
    }
}

// Debounced NVS write: persist the volume once it has been stable for a short
// while, so a slider drag doesn't write flash on every intermediate value. Runs
// on the main loop task (from tick()).
void maybe_save_volume() {
    if (!g_vol_dirty) return;
    if (millis() - g_vol_changed_at < VOL_SAVE_DELAY_MS) return;
    g_vol_dirty = false;
    const uint8_t pct = g_vol_pct;
    Preferences prefs;
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[audio] save volume: NVS open failed");
        return;
    }
    if (prefs.getUChar(NVS_KEY, 255) != pct) {   // skip a redundant rewrite
        prefs.putUChar(NVS_KEY, pct);
        Serial.printf("[audio] volume %u%% saved to NVS\n", pct);
    }
    prefs.end();
}

}  // namespace

void init() {
    // MAX98357A SD_MODE / gain pin via 634 kΩ series resistor on this board.
    // Driving the GPIO high pulls SD_MODE high → amp is enabled.
    // Leave it asserted during init so playback works immediately on first preset.
    pinMode(cfg::AUDIO_MODE_PIN, OUTPUT);
    digitalWrite(cfg::AUDIO_MODE_PIN, HIGH);

    load_volume();   // restore last-saved master volume (defaults to 100 %)
    i2s_install(SAMPLE_RATE_HZ);
    Serial.printf("[audio] I²S initialised (BCLK=%u, LRCLK=%u, DIN=%u, SD=%u)\n",
                  cfg::I2S_BCLK_PIN, cfg::I2S_LRCLK_PIN, cfg::I2S_DIN_PIN, cfg::AUDIO_MODE_PIN);
}

void play_preset(uint8_t preset_id) {
    // Switching to a preset always tears down any in-progress file/preset.
    if (g_file) g_file.close();
    g_source = Source::None;

    g_preset = preset_id;
    g_samples_done = 0;
    if (preset_id == 0) {   // silence / stop
        g_playing = false;
        g_total_samples = 0;
        i2s_zero_dma_buffer(I2S_PORT);
        return;
    }

    // Presets are synthesised at the fixed preset rate; a prior file may have
    // retuned the I²S clock, so restore it.
    set_rate(SAMPLE_RATE_HZ);

    uint32_t duration_ms = 500;
    switch (preset_id) {
        case 1: duration_ms = 500; break;
        case 2: duration_ms = 700; break;
        case 3: duration_ms = 600; break;
        case 4: duration_ms = 400; break;
        default: duration_ms = 300; break;
    }
    g_total_samples = (uint32_t)((uint64_t)duration_ms * SAMPLE_RATE_HZ / 1000);
    g_source  = Source::Preset;
    g_playing = true;
    Serial.printf("[audio] play preset %u (%u ms)\n", preset_id, duration_ms);
}

bool play_file(const String& name) {
    stop_internal();   // release anything already playing

    File f = sdcard::open_audio(name);
    if (!f) return false;

    WavInfo wav;
    if (!parse_wav(f, wav) || wav.bits != 16 || wav.sample_rate == 0 ||
        (wav.channels != 1 && wav.channels != 2)) {
        Serial.printf("[audio] play_file: '%s' is not a supported 16-bit PCM WAV\n",
                      name.c_str());
        f.close();
        return false;
    }

    set_rate((int)wav.sample_rate);
    g_file           = f;
    g_channels       = (uint8_t)wav.channels;
    g_data_remaining = wav.data_size;
    g_source         = Source::File;
    g_playing        = true;
    Serial.printf("[audio] play_file: %s, %u Hz, %u ch, %u bytes PCM\n",
                  name.c_str(), (unsigned)wav.sample_rate, (unsigned)wav.channels,
                  (unsigned)wav.data_size);
    return true;
}

void tick() {
    maybe_save_volume();   // debounced; runs regardless of playback state
    if (!g_playing) return;
    // Feed several chunks per visit so the DMA stays well ahead of an SD-read
    // latency spike. i2s_write blocks once the buffers are full, so this paces
    // itself to real time and never overruns. Presets are cheap to synthesise,
    // so one chunk per tick is plenty for them.
    int budget = (g_source == Source::File) ? 4 : 1;
    bool more = true;
    while (budget-- > 0 && more) {
        more = (g_source == Source::File) ? stream_file_chunk() : render_chunk();
    }
    if (!more) stop_internal();
}

void set_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    g_volume = (float)pct / 100.0f;   // applies immediately
    g_vol_pct = pct;                  // remembered for the debounced NVS save
    g_vol_dirty = true;
    g_vol_changed_at = millis();
    Serial.printf("[audio] volume %u%%\n", pct);
}

bool is_playing() {
    return g_playing;
}

}  // namespace audio
