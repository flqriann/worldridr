// Einfacher synthetischer Motorklang (wichtig, um die Kupplung nach Gehoer zu dosieren), Wind, Blinker, Hupe
#include "audio.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cmath>
#include <vector>

static std::atomic<float> aRpm{0}, aLoad{0}, aSpeed{0}, aVolume{0.6f};
static std::atomic<bool> aHorn{false};
static std::atomic<int> aClick{0};
static std::atomic<float> aSiren{0};

static void SDLCALL audioCb(void*, SDL_AudioStream* stream, int additional, int) {
    static float ph[4] = {0, 0, 0, 0}, hornPh[2] = {0, 0}, lp = 0, rpmS = 0, loadS = 0, clickT = 1, sirenPh = 0, sirenT = 0, sirenS = 0;
    static uint32_t seed = 12345;
    static int lastClick = 0;
    int n = additional / (int)sizeof(float);
    if (n <= 0) return;
    std::vector<float> buf(n);
    float rpm = aRpm.load(), load = aLoad.load(), speed = aSpeed.load(), vol = aVolume.load();
    bool horn = aHorn.load();
    float siren = aSiren.load();
    int click = aClick.load();
    if (click != lastClick) { lastClick = click; clickT = 0; }
    const float sr = 48000.0f, twoPi = 6.2831853f;
    for (int i = 0; i < n; i++) {
        rpmS += (rpm - rpmS) * 0.002f;
        loadS += (load - loadS) * 0.002f;
        float f = rpmS / 30.0f; // Zuendfrequenz 4-Zylinder
        static const float mult[4] = {0.5f, 1.0f, 1.5f, 2.0f}, amp[4] = {0.35f, 0.5f, 0.2f, 0.18f};
        float s = 0;
        for (int h = 0; h < 4; h++) {
            ph[h] += twoPi * f * mult[h] / sr;
            if (ph[h] > twoPi) ph[h] -= twoPi;
            float v = std::sin(ph[h]);
            s += amp[h] * (v + 0.3f * v * v * v);
        }
        seed = seed * 1664525u + 1013904223u;
        float noise = ((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        lp += (noise - lp) * 0.04f;
        float eng = rpmS > 80 ? s * (0.10f + 0.22f * loadS) * std::min(1.0f, rpmS / 600.0f) : 0.0f;
        float wind = lp * std::min(0.25f, speed * 0.004f);
        float out = eng + wind;
        if (horn) {
            hornPh[0] += twoPi * 420.0f / sr; hornPh[1] += twoPi * 505.0f / sr;
            out += 0.18f * ((std::sin(hornPh[0]) > 0 ? 1.0f : -1.0f) + (std::sin(hornPh[1]) > 0 ? 1.0f : -1.0f)) * 0.5f;
        }
        sirenS += (siren - sirenS) * 0.0005f;
        if (sirenS > 0.002f) { // Martinshorn: zwei Toene im Wechsel
            sirenT += 1.0f / sr;
            if (sirenT > 0.9f) sirenT -= 0.9f;
            sirenPh += twoPi * (sirenT < 0.45f ? 660.0f : 880.0f) / sr;
            if (sirenPh > twoPi) sirenPh -= twoPi;
            out += 0.16f * sirenS * (std::sin(sirenPh) + 0.35f * std::sin(2 * sirenPh));
        }
        if (clickT < 0.012f) { out += 0.25f * std::sin(clickT * twoPi * 1800.0f) * (1.0f - clickT / 0.012f); clickT += 1.0f / sr; }
        buf[i] = out * vol;
    }
    SDL_PutAudioStreamData(stream, buf.data(), n * (int)sizeof(float));
}

void audioInit(float volume) {
    aVolume = volume;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCb, nullptr);
    if (s) SDL_ResumeAudioStreamDevice(s);
}
void audioSet(float rpm, float load, float speed, bool horn) { aRpm = rpm; aLoad = load; aSpeed = speed; aHorn = horn; }
void audioClick() { aClick++; }
void audioSiren(float level) { aSiren = level; }
