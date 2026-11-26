#include "music.h"

#include <SDL2/SDL.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
extern const unsigned char music_bin[];
extern const unsigned char music_bin_end[];
}

namespace {

struct Music {
    mp3dec_t dec{};
    const unsigned char* data = music_bin;
    size_t size = static_cast<size_t>(music_bin_end - music_bin);
    size_t pos = 0;
    int channels = 2;
    int hz = 48000;
    SDL_AudioDeviceID dev = 0;
    std::mutex mu;
    std::vector<mp3d_sample_t> ring;
    size_t read = 0;
    size_t used = 0;
    std::thread feeder;
    std::atomic<bool> run{false};
};

Music g_music;

size_t spaceLocked() {
    return g_music.ring.size() - g_music.used;
}

void pushLocked(const mp3d_sample_t* src, size_t n) {
    const size_t cap = g_music.ring.size();
    if (n > cap - g_music.used) n = cap - g_music.used;
    const size_t wr = (g_music.read + g_music.used) % cap;
    const size_t first = std::min(n, cap - wr);
    std::memcpy(g_music.ring.data() + wr, src, first * sizeof(mp3d_sample_t));
    if (n > first) std::memcpy(g_music.ring.data(), src + first, (n - first) * sizeof(mp3d_sample_t));
    g_music.used += n;
}

void pullLocked(mp3d_sample_t* dst, size_t n) {
    const size_t cap = g_music.ring.size();
    const size_t first = std::min(n, cap - g_music.read);
    std::memcpy(dst, g_music.ring.data() + g_music.read, first * sizeof(mp3d_sample_t));
    if (n > first) std::memcpy(dst + first, g_music.ring.data(), (n - first) * sizeof(mp3d_sample_t));
    g_music.read = (g_music.read + n) % cap;
    g_music.used -= n;
}

void decodeFrame() {
    mp3dec_frame_info_t info{};
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    const int samples = mp3dec_decode_frame(&g_music.dec, g_music.data + g_music.pos, static_cast<int>(g_music.size - g_music.pos), pcm, &info);
    if (info.frame_bytes <= 0 || g_music.pos >= g_music.size) {
        g_music.pos = 0;
        mp3dec_init(&g_music.dec);
        return;
    }
    g_music.pos += static_cast<size_t>(info.frame_bytes);
    if (g_music.pos >= g_music.size) {
        g_music.pos = 0;
        mp3dec_init(&g_music.dec);
    }
    if (samples <= 0) return;
    std::lock_guard<std::mutex> lock(g_music.mu);
    pushLocked(pcm, static_cast<size_t>(samples) * static_cast<size_t>(g_music.channels));
}

void feedLoop() {
    while (g_music.run.load()) {
        bool room = false;
        {
            std::lock_guard<std::mutex> lock(g_music.mu);
            room = spaceLocked() >= static_cast<size_t>(MINIMP3_MAX_SAMPLES_PER_FRAME);
        }
        if (room) decodeFrame();
        else SDL_Delay(4);
    }
}

void fillAudio(void* userdata, Uint8* stream, int len) {
    auto* music = static_cast<Music*>(userdata);
    std::memset(stream, 0, static_cast<size_t>(len));
    auto* dst = reinterpret_cast<mp3d_sample_t*>(stream);
    size_t need = static_cast<size_t>(len) / sizeof(mp3d_sample_t);
    std::lock_guard<std::mutex> lock(music->mu);
    if (need > music->used) need = music->used;
    if (need > 0) pullLocked(dst, need);
}

}  // namespace

void musicStart(bool on) {
    if (g_music.size == 0) return;
    mp3dec_init(&g_music.dec);
    mp3dec_frame_info_t info{};
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (int i = 0; i < 8 && info.hz <= 0 && g_music.pos < g_music.size; ++i) {
        mp3dec_decode_frame(&g_music.dec, g_music.data + g_music.pos, static_cast<int>(g_music.size - g_music.pos), pcm, &info);
        if (info.frame_bytes <= 0) break;
        g_music.pos += static_cast<size_t>(info.frame_bytes);
    }
    g_music.pos = 0;
    mp3dec_init(&g_music.dec);
    if (info.hz <= 0 || info.channels <= 0) return;
    g_music.hz = info.hz;
    g_music.channels = info.channels;
    g_music.ring.assign(static_cast<size_t>(info.hz) * static_cast<size_t>(info.channels), 0);
    g_music.read = 0;
    g_music.used = 0;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want{};
    want.freq = info.hz;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(info.channels);
    want.samples = 8192;
    want.callback = fillAudio;
    want.userdata = &g_music;
    g_music.dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g_music.dev == 0) return;
    SDL_PauseAudioDevice(g_music.dev, 1);
    g_music.run = true;
    g_music.feeder = std::thread(feedLoop);
    const size_t warm = static_cast<size_t>(info.hz) * static_cast<size_t>(info.channels) / 4;
    for (int i = 0; i < 80; ++i) {
        size_t have = 0;
        {
            std::lock_guard<std::mutex> lock(g_music.mu);
            have = g_music.used;
        }
        if (have >= warm) break;
        SDL_Delay(5);
    }
    if (on) SDL_PauseAudioDevice(g_music.dev, 0);
}

void musicSet(bool on) {
    if (g_music.dev != 0) SDL_PauseAudioDevice(g_music.dev, on ? 0 : 1);
}

void musicStop() {
    g_music.run = false;
    if (g_music.feeder.joinable()) g_music.feeder.join();
    if (g_music.dev == 0) return;
    SDL_CloseAudioDevice(g_music.dev);
    g_music.dev = 0;
}
