// coop/player/sleeper_snore_sound.cpp -- see coop/player/sleeper_snore_sound.h.

#include "coop/player/sleeper_snore_sound.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <miniaudio.h>

#include "../../../resources/snore_resource_ids.h"

namespace coop::sleeper_snore_sound {
namespace {

constexpr uint32_t kRate = 22050;            // low on purpose: the game's own audio is lo-fi
constexpr int kCrushLevels = 1024;           // ~10 bit
constexpr float kPeak = 0.7f;                // of full scale, before the config volume
constexpr float kPi = 3.14159265f;
constexpr uint64_t kMaxWavSeconds = 15;      // a longer snore.wav is cut here

std::vector<Clip> g_clips;
bool g_prepared = false;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    float Next() {   // [-1, 1]
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.0f - 1.0f;
    }
};

// A Chamberlin state-variable filter, band-pass output.
struct Bandpass {
    float low = 0.f, band = 0.f;
    float Run(float in, float fc, float q) {
        const float f = 2.0f * std::sin(kPi * fc / static_cast<float>(kRate));
        low += f * band;
        const float high = in - low - q * band;
        band += f * high;
        return band;
    }
};

std::wstring ModuleDir() {
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&ModuleDir), &mod) || !mod)
        return {};
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    const size_t cut = buf.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring{} : buf.substr(0, cut);
}

bool ExistsFile(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Drain an opened decoder into `out` (mono s16 at the file's own rate), then close it.
bool ReadDecoder(ma_decoder& dec, Clip& out) {
    ma_format fmt;
    ma_uint32 ch = 0, rate = 0;
    ma_decoder_get_data_format(&dec, &fmt, &ch, &rate, nullptr, 0);
    const ma_uint64 cap = static_cast<ma_uint64>(rate) * kMaxWavSeconds;
    std::vector<int16_t> pcm;
    int16_t chunk[4096];
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, chunk, 4096, &got);
        pcm.insert(pcm.end(), chunk, chunk + got);
        if (r != MA_SUCCESS || got == 0 || pcm.size() >= cap) break;
    }
    ma_decoder_uninit(&dec);
    if (pcm.size() < 64 || rate == 0) return false;
    if (pcm.size() > cap) pcm.resize(static_cast<size_t>(cap));
    out.pcm = std::move(pcm);
    out.rate = rate;
    return true;
}

// miniaudio's built-in WAV decoder, down-mixed to mono s16 at the file's own rate.
bool LoadWav(const std::wstring& path, Clip& out) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &cfg, &dec) != MA_SUCCESS) return false;
    return ReadDecoder(dec, out);
}

// The snore embedded in this DLL (resources/snore.rc), decoded straight from the mapped image.
bool LoadEmbedded(Clip& out) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&LoadEmbedded), &self);
    if (!self) return false;
    HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_SNORE_WAV), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return false;
    HGLOBAL glob = ::LoadResource(self, res);
    const DWORD size = ::SizeofResource(self, res);
    const void* p = glob ? ::LockResource(glob) : nullptr;
    if (!p || size == 0) return false;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(p, size, &cfg, &dec) != MA_SUCCESS) return false;
    return ReadDecoder(dec, out);
}

}  // namespace

// A soft, lo-fi snore: an inhale (band-passed noise whose centre climbs, fluttering at the soft palate's ~28 Hz,
// over a low raspy buzz), a gap, then a quieter low-passed breath out. Quantized, then peak-normalized.
Clip Synthesize(uint32_t seed, float lengthS) {
    Rng rng(seed);
    const size_t n = static_cast<size_t>(lengthS * static_cast<float>(kRate));
    std::vector<float> buf(n, 0.0f);
    const size_t inhaleN = static_cast<size_t>(0.42f * static_cast<float>(n));
    const size_t exhaleAt = static_cast<size_t>(0.50f * static_cast<float>(n));
    const size_t exhaleN = static_cast<size_t>(0.38f * static_cast<float>(n));
    const float flutterHz = 24.0f + 6.0f * (rng.Next() * 0.5f + 0.5f);
    Bandpass bp;
    float rumble = 0.f, phase = 0.f, lp = 0.f;
    for (size_t i = 0; i < inhaleN; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(inhaleN);
        const float env = std::pow(std::sin(kPi * std::pow(x, 0.8f)), 1.5f) * (0.55f + 0.45f * x);
        const float t = static_cast<float>(i) / static_cast<float>(kRate);
        const float flutter = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(2.0f * kPi * flutterHz * t));
        const float rasp = bp.Run(rng.Next(), 380.0f + 420.0f * x, 0.45f) * 1.6f;
        phase += (70.0f + 30.0f * x) / static_cast<float>(kRate);
        if (phase >= 1.0f) phase -= 1.0f;
        rumble += 0.045f * ((2.0f * phase - 1.0f) - rumble);   // a saw into a one-pole low-pass
        buf[i] += env * flutter * (rasp * 0.9f + rumble * 2.2f);
    }
    for (size_t j = 0; j < exhaleN && exhaleAt + j < n; ++j) {
        const float x = static_cast<float>(j) / static_cast<float>(exhaleN);
        const float env = std::pow(std::sin(kPi * x), 2.0f) * 0.42f;
        lp += 0.09f * (rng.Next() - lp);   // ~300 Hz low-passed breath
        buf[exhaleAt + j] += env * lp * 4.0f;
    }
    float peak = 1e-6f;
    for (float v : buf) peak = (std::max)(peak, std::fabs(v));
    Clip c;
    c.rate = kRate;
    c.pcm.resize(n);
    const size_t tail = static_cast<size_t>(0.02f * static_cast<float>(kRate));
    for (size_t i = 0; i < n; ++i) {
        float v = buf[i] / peak * kPeak;
        if (i + tail > n) v *= static_cast<float>(n - i) / static_cast<float>(tail);   // no click at the end
        v = std::round(v * (kCrushLevels / 2)) / (kCrushLevels / 2);                   // the bit-crush
        c.pcm[i] = static_cast<int16_t>((std::min)(1.0f, (std::max)(-1.0f, v)) * 32767.0f);
    }
    return c;
}

void Prepare() {
    if (g_prepared) return;
    g_prepared = true;
    std::vector<std::wstring> dirs;
    const std::wstring beside = ModuleDir();
    if (!beside.empty()) dirs.push_back(beside);
    const std::wstring data = ue_wrap::paths::ModDataDir();
    if (!data.empty() && data != beside) dirs.push_back(data);
    for (const std::wstring& d : dirs) {
        const std::wstring path = d + L"\\snore.wav";
        if (!ExistsFile(path)) continue;
        Clip c;
        if (LoadWav(path, c)) {
            UE_LOGI("sleeper_snore: using %ls (%zu frames at %u Hz, %.1f s)", path.c_str(), c.pcm.size(), c.rate,
                    static_cast<float>(c.pcm.size()) / static_cast<float>(c.rate));
            g_clips.push_back(std::move(c));
            return;
        }
        UE_LOGW("sleeper_snore: %ls could not be decoded as a wav -- using the built-in snore", path.c_str());
    }
    {
        Clip c;
        if (LoadEmbedded(c)) {
            UE_LOGI("sleeper_snore: using the built-in snore (%zu frames at %u Hz, %.1f s)", c.pcm.size(), c.rate,
                    static_cast<float>(c.pcm.size()) / static_cast<float>(c.rate));
            g_clips.push_back(std::move(c));
            return;
        }
        UE_LOGW("sleeper_snore: the built-in snore could not be read -- generating one");
    }
    static const struct { uint32_t seed; float len; } kVariants[] = {{0x51CE01u, 1.7f}, {0x51CE02u, 2.1f}, {0x51CE03u, 2.4f}};
    for (const auto& v : kVariants) g_clips.push_back(Synthesize(v.seed, v.len));
    UE_LOGI("sleeper_snore: no snore.wav and no built-in snore -- generated %zu snores (%u Hz mono, %.1f to %.1f s)",
            g_clips.size(), kRate, kVariants[0].len, kVariants[2].len);
}

const std::vector<Clip>& Clips() { return g_clips; }

}  // namespace coop::sleeper_snore_sound
