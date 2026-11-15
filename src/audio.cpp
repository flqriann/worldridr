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

static std::atomic<int> aCyl{4};
static std::atomic<float> aRain{0};
static std::atomic<bool> aDiesel{false};

// Biquad-Bandpass (konstante Spitzenverstaerkung) fuer Resonanzen von Auspuff und Motorblock
struct Bq {
    float b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(float f, float q, float sr) {
        float w = 6.2831853f * f / sr, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = al / a0; b2 = -al / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    float run(float x) { float y = b0 * x + z1; z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// Motor als Folge von Zuendimpulsen (je Zylinder leicht unterschiedlich) durch Auspuffrohr (Kammfilter), Resonanzen,
// Tiefpass (Schalldaempfer), dazu Ansauggeraeusch, Ventiltrieb und Rollen/Wind.
static void SDLCALL audioCb(void*, SDL_AudioStream* stream, int additional, int) {
    static float hornPh[2] = {0, 0}, rpmS = 0, loadS = 0, clickT = 1, sirenPh = 0, sirenT = 0, sirenS = 0;
    static double crank = 0;
    static long long fireCount = 0;
    static uint32_t seed = 12345;
    static int lastClick = 0;
    static float comb[1024]; static int cw = 0;
    static float env = 0, envT = 0, lp1 = 0, lp2 = 0, lpN1 = 0, lpN2 = 0, lpW = 0, hpV = 0, hpPrev = 0, ex = 0;
    static float cylF[8] = {1.00f, 0.93f, 1.06f, 0.96f, 1.03f, 0.91f, 1.05f, 0.98f};
    static Bq boom, body, honk;
    static bool init = false;
    if (!init) { init = true; boom.set(95, 2.2f, 48000); body.set(260, 1.6f, 48000); honk.set(640, 2.5f, 48000); }
    int n = additional / (int)sizeof(float);
    if (n <= 0) return;
    std::vector<float> buf(n);
    float rpm = aRpm.load(), load = aLoad.load(), speed = aSpeed.load(), vol = aVolume.load();
    bool horn = aHorn.load(), diesel = aDiesel.load();
    int cyl = std::max(1, std::min(8, aCyl.load()));
    float siren = aSiren.load();
    int click = aClick.load();
    if (click != lastClick) { lastClick = click; clickT = 0; }
    const float sr = 48000.0f, twoPi = 6.2831853f;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f; };
    for (int i = 0; i < n; i++) {
        rpmS += (rpm - rpmS) * 0.0015f;
        loadS += (load - loadS) * 0.0012f;
        float rpmN = std::min(1.0f, rpmS / 6500.0f);
        float noise = rnd();
        float impulse = 0;
        if (rpmS > 60) {
            crank += rpmS / 60.0 / sr;
            long long fp = (long long)std::floor(crank * cyl * 0.5);
            if (fp != fireCount) { // ein Zylinder zuendet
                fireCount = fp;
                float lump = (1.0f - rpmN) * 0.30f; // Leerlauf: unrunder
                float a = cylF[fireCount % cyl] * (1.0f + lump * rnd());
                float pwr = 0.30f + 0.70f * loadS; // Schub: kaum Zuendung, nur Ladungswechsel
                impulse = a * pwr * (diesel ? 1.25f : 1.0f);
            }
        }
        // Zuendimpuls -> Druckwelle: kurzer Anstieg, schnelles Abklingen
        env = env * (diesel ? 0.9925f : 0.9905f) + impulse;
        float pulse = env;   // glatter Druckimpuls, kein Rauschen (sonst klingt der Motor sandig)
        // Auspuffrohr: Kammfilter mit Laufzeit der Welle (ca. 1,2 m Rohr)
        int D = 190 + (int)(60.0f * (1.0f - rpmN));
        float cyc = comb[(cw - D) & 1023];
        float x = pulse + 0.42f * cyc;
        comb[cw & 1023] = x; cw++;
        // Schalldaempfer: Tiefpass, oeffnet mit Last und Drehzahl
        float cut = 0.012f + 0.050f * loadS + 0.030f * rpmN;
        lp1 += (x - lp1) * cut; lp2 += (lp1 - lp2) * (cut * 1.4f);
        float exh = lp2 * 3.4f;
        float res = 0.9f * boom.run(exh) + 0.55f * body.run(exh) + (0.25f + 0.55f * loadS) * honk.run(exh) * rpmN;
        // Ansauggeraeusch (pfeift mit Last)
        lpN1 += (noise - lpN1) * 0.02f; lpN2 += (lpN1 - lpN2) * 0.02f;   // sehr tiefes, weiches Ansaugbrummen statt hellem Rauschen
        float intake = lpN2 * 14.0f * loadS * loadS * (0.15f + 0.85f * rpmN);
        // Ventiltrieb / Dieselnageln: hochfrequentes Ticken im Rhythmus der Zuendungen
        float tickPh = (float)(crank * cyl * 0.5 - std::floor(crank * cyl * 0.5));
        hpV = noise - hpPrev; hpPrev = noise;
        float tick = hpV * (diesel ? 0.03f : 0.004f) * (0.3f + 0.7f * std::exp(-tickPh * 9.0f)) * std::min(1.0f, rpmS / 500.0f);
        float eng = rpmS > 50 ? (res * 0.55f + intake * 0.22f + tick) * std::min(1.0f, rpmS / 450.0f) : 0.0f;
        eng = std::tanh(eng * 1.6f) * 0.62f;
        // Rollen und Wind: Rauschen, tiefpassgefiltert, wird mit Tempo lauter und heller
        lpW += (noise - lpW) * (0.012f + std::min(0.06f, speed * 0.0012f));   // Fahrtwind dunkler und leiser
        float wind = lpW * 2.2f * std::min(0.25f, speed * speed * 0.00014f + speed * 0.0012f);
        static float lpR = 0; lpR += (noise - lpR) * 0.35f;
        float rainS = (noise - lpR) * 0.09f * aRain.load();
        float out = eng + wind + rainS;
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

void audioRain(float level) { aRain = level; }
void audioEngine(int cylinders, bool diesel) { aCyl = cylinders; aDiesel = diesel; }

void audioInit(float volume) {
    aVolume = volume;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCb, nullptr);
    if (s) SDL_ResumeAudioStreamDevice(s);
}
void audioSet(float rpm, float load, float speed, bool horn) { aRpm = rpm; aLoad = load; aSpeed = speed; aHorn = horn; }
void audioClick() { aClick++; }
void audioSiren(float level) { aSiren = level; }
