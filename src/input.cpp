#include "input.h"
#include "keys.h"
#include <cstring>

void Input::init(const WheelCfg& c) {
    cfg = c;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    if (ids) {
        for (int i = 0; i < n && !joy; i++) openWheel(ids[i]);
        SDL_free(ids);
    }
}

static bool looksLikeWheel(const char* name) {
    if (!name) return false;
    std::string s = name;
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    const char* keys[] = {"g920", "g29", "g923", "g27", "g25", "driving force", "wheel", "racing", "thrustmaster", "fanatec", "moza", "simucube"};
    for (const char* k : keys) if (s.find(k) != std::string::npos) return true;
    return false;
}

void Input::openWheel(SDL_JoystickID id) {
    if (joy) return;
    const char* name = SDL_GetJoystickNameForID(id);
    if (!looksLikeWheel(name)) { logMsg("Eingabegeraet ignoriert (kein Lenkrad): %s", name ? name : "?"); return; }
    joy = SDL_OpenJoystick(id);
    if (!joy) return;
    joyId = id;
    wheelPresent = true;
    wheelName = name ? name : "Lenkrad";
    naxes = std::min(SDL_GetNumJoystickAxes(joy), 8);
    for (int i = 0; i < naxes; i++) { axisInit[i] = SDL_GetJoystickAxis(joy, i); axisTouched[i] = false; }
    logMsg("Lenkrad: %s (%d Achsen, %d Tasten)", wheelName.c_str(), SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy));
    haptic = SDL_OpenHapticFromJoystick(joy);
    if (haptic && (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_CONSTANT)) {
        if (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(haptic, 0);
        if (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_GAIN) SDL_SetHapticGain(haptic, 100);
        SDL_HapticEffect e;
        memset(&e, 0, sizeof e);
        e.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e.constant.direction.dir[0] = 1;
        e.constant.length = SDL_HAPTIC_INFINITY;
        effect = SDL_CreateHapticEffect(haptic, &e);
        if (effect >= 0 && SDL_RunHapticEffect(haptic, effect, 1)) ffbOk = true;
    }
    logMsg(ffbOk ? "Force Feedback aktiv" : "Force Feedback nicht verfuegbar (%s)", SDL_GetError());
}

void Input::beginFrame() {
    shiftUp = shiftDown = handbrake = blinkL = blinkR = hazard = lights = belt = reset = camera = false;
}

void Input::handleEvent(const SDL_Event& e, bool typing) {
    if (e.type == SDL_EVENT_JOYSTICK_ADDED) openWheel(e.jdevice.which);
    else if (e.type == SDL_EVENT_JOYSTICK_REMOVED && joy && e.jdevice.which == joyId) {
        if (haptic) SDL_CloseHaptic(haptic);
        SDL_CloseJoystick(joy);
        joy = nullptr; haptic = nullptr; effect = -1; wheelPresent = false; ffbOk = false;
        logMsg("Lenkrad getrennt");
    } else if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && joy && e.jbutton.which == joyId) {
        int b = e.jbutton.button;
        if (b == cfg.bShiftUp) shiftUp = true;
        else if (b == cfg.bShiftDown) shiftDown = true;
        else if (b == cfg.bBlinkL) blinkL = true;
        else if (b == cfg.bBlinkR) blinkR = true;
        else if (b == cfg.bHandbrake) handbrake = true;
        else if (b == cfg.bBelt) belt = true;
        else if (b == cfg.bLights) lights = true;
        else if (b == cfg.bCamera) camera = true;
        else if (b == cfg.bReset) reset = true;
    } else if (e.type == SDL_EVENT_KEY_DOWN && !typing && !e.key.repeat) {
        SDL_Keycode k = e.key.key;
        if (keys::is(A_SHIFTUP, k)) shiftUp = true;
        else if (keys::is(A_SHIFTDOWN, k)) shiftDown = true;
        else if (keys::is(A_HANDBRAKE, k)) handbrake = true;
        else if (keys::is(A_BLINK_L, k)) blinkL = true;
        else if (keys::is(A_BLINK_R, k)) blinkR = true;
        else if (keys::is(A_HAZARD, k)) hazard = true;
        else if (keys::is(A_LIGHTS, k)) lights = true;
        else if (keys::is(A_BELT, k)) belt = true;
        else if (keys::is(A_RESET, k)) reset = true;
        else if (keys::is(A_CAMERA, k)) camera = true;
    }
}

float Input::pedal(int axis) {
    if (axis < 0 || axis >= naxes) return 0;
    Sint16 raw = SDL_GetJoystickAxis(joy, axis);
    // Pedale melden vor der ersten Bewegung oft einen Mittelwert - erst ab Bewegung auswerten
    if (!axisTouched[axis]) {
        if (std::abs(raw - axisInit[axis]) < 600) return 0;
        axisTouched[axis] = true;
    }
    float v = raw / 32767.0f;
    float p = cfg.invertPedals ? (1.0f - v) * 0.5f : (v + 1.0f) * 0.5f;
    return clampv((p - 0.03f) / 0.94f, 0.0f, 1.0f);
}

void Input::update(float dt, float speedMs, bool typing) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    (void)k;
    auto key = [&](Act a) { return !typing && keys::held(a); };
    // Tastatur: weiches, geschwindigkeitsabhaengiges Lenken
    float target = (key(A_RIGHT) ? 1.0f : 0.0f) - (key(A_LEFT) ? 1.0f : 0.0f);
    float kmh = std::fabs(speedMs) * 3.6f;
    target *= 1.0f / (1.0f + (kmh / 45.0f) * (kmh / 45.0f));
    float rate = (std::fabs(target) < std::fabs(kbSteer) || target * kbSteer < 0) ? 3.0f : 1.4f;
    kbSteer += clampv(target - kbSteer, -rate * dt, rate * dt);
    auto ramp = [&](float& v, bool on, float up, float down) { v = clampv(v + (on ? up : -down) * dt, 0.0f, 1.0f); };
    ramp(kbThrottle, key(A_THROTTLE), 1.6f, 4.0f);
    ramp(kbBrake, key(A_BRAKE), 2.2f, 6.0f);
    ramp(kbClutch, key(A_CLUTCH), 6.0f, 1.1f);
    steer = kbSteer; throttle = kbThrottle; brake = kbBrake; clutch = kbClutch;
    horn = key(A_HORN);
    starter = key(A_IGNITION);
    lookYaw = 0;
    buttons = 0;
    if (joy) {
        for (int i = 0; i < naxes; i++) axes[i] = SDL_GetJoystickAxis(joy, i) / 32767.0f;
        if (cfg.steerAxis < naxes && std::fabs(kbSteer) < 0.01f) steer = clampv(axes[cfg.steerAxis], -1.0f, 1.0f);
        throttle = std::max(throttle, pedal(cfg.throttleAxis));
        brake = std::max(brake, pedal(cfg.brakeAxis));
        clutch = std::max(clutch, pedal(cfg.clutchAxis));
        int nb = std::min(SDL_GetNumJoystickButtons(joy), 32);
        for (int i = 0; i < nb; i++) if (SDL_GetJoystickButton(joy, i)) buttons |= 1u << i;
        auto btn = [&](int b) { return b >= 0 && b < 32 && (buttons & (1u << b)); };
        if (btn(cfg.bHorn)) horn = true;
        if (btn(cfg.bIgnition)) starter = true;
        if (cfg.hShifter) {
            int g = 0;
            for (int i = 0; i < 6; i++) if (btn(cfg.gearButtons[i])) g = i + 1;
            if (btn(cfg.reverseButton)) g = -1;
            if (g != 0) hUsed = true;
            hGear = hUsed ? g : -99;
        }
        if (SDL_GetNumJoystickHats(joy) > 0) {
            Uint8 hat = SDL_GetJoystickHat(joy, 0);
            if (hat & SDL_HAT_LEFT) lookYaw = 1.2f;
            if (hat & SDL_HAT_RIGHT) lookYaw = -1.2f;
        }
    }
}

// Lenkmoment in Nm (positiv = nach rechts) -> konstante Kraft am Lenkrad
void Input::setFFB(float torqueNm, float dt) {
    if (!ffbOk || !haptic || effect < 0) return;
    float rate = dt > 0 ? (steer - lastSteer) / dt : 0; // Daempfung gegen Aufschwingen
    lastSteer = steer;
    float t = torqueNm / 6.0f - clampv(rate * 0.12f, -0.35f, 0.35f);
    float level = clampv(-t * cfg.ffbStrength, -1.0f, 1.0f); // SDL: positive Kraft drueckt nach links
    if (cfg.ffbInvert) level = -level;
    SDL_HapticEffect e;
    memset(&e, 0, sizeof e);
    e.type = SDL_HAPTIC_CONSTANT;
    e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    e.constant.direction.dir[0] = 1;
    e.constant.length = SDL_HAPTIC_INFINITY;
    e.constant.level = (Sint16)(level * 32767.0f);
    SDL_UpdateHapticEffect(haptic, effect, &e);
}
