#include "vehicle.h"
#include "world.h"

namespace {
float MASS = 1300.0f;
Vec3 INERTIA{1900.0f, 2100.0f, 550.0f}; // Nicken (x), Gieren (y), Wanken (z)
float R = 0.316f;                       // Radradius
float SUS_REST = 0.30f, SUS_MAX = 0.26f;
float I_WHEEL = 1.1f, I_ENGINE = 0.23f; // Kurbeltrieb + Zweimassenschwungrad + Kupplung
float FINAL = 3.65f;
float GEARS[8] = {-3.60f, 0.0f, 3.78f, 2.12f, 1.36f, 1.03f, 0.84f, 0.70f}; // R, N, 1..6
float CLUTCH_K = 40.0f, CLUTCH_CAP = 330.0f;
float MAX_STEER = 0.56f;
// weitere Fahrzeugparameter (Auto / Moped / Motorrad), siehe Vehicle::setKind
float TQ = 1.0f, RPMK = 1.0f, IDLE = 850.0f, RLIMIT = 6500.0f, STALL = 350.0f, START = 480.0f, BRAKE_F = 1900.0f, BRAKE_R = 1000.0f, BRAKE_H = 1600.0f;
int ENG = 0;                 // 0 Benzin (Turbo), 1 Diesel (Turbo)
float IDLEAUTH = 0.28f, STALLT = 0.35f, stallTimer = 0.0f, FRICK = 1.0f;
float STAB_F = 12000.0f, STAB_R = 6000.0f, MUN = 3500.0f, CDA = 0.66f, TWIDTH = 0.77f, ZF = 1.10f, ZR = 1.52f, KF = 29000.0f, CF = 2400.0f, KR = 21000.0f, CR = 1750.0f;
const float STEER_RATIO = 14.5f;
const float SR_PEAK = 0.10f, SA_PEAK = 0.14f;

float torqueCurve(float rpmIn) {
    float rpm = rpmIn * RPMK;
    static const float petrol[8][2] = {{0, 60}, {800, 115}, {1500, 200}, {3500, 200}, {5000, 176}, {6000, 142}, {6600, 100}, {7500, 0}};
    // Turbodiesel 1.6: viel Drehmoment schon bei Leerlauf, Maximum 320 Nm ab 1750/min, Drehzahlbegrenzung bei knapp 5000
    static const float diesel[8][2] = {{0, 120}, {800, 175}, {1250, 262}, {1750, 320}, {3000, 320}, {3800, 270}, {4500, 190}, {5000, 0}};
    const float (*pts)[2] = (ENG == 1 && TQ == 1.0f) ? diesel : petrol;
    for (int i = 0; i < 7; i++)
        if (rpm <= pts[i + 1][0]) {
            float t = (rpm - pts[i][0]) / (pts[i + 1][0] - pts[i][0]);
            return (pts[i][1] + (pts[i + 1][1] - pts[i][1]) * clampv(t, 0.0f, 1.0f)) * TQ;
        }
    return 0;
}
float clutchEngage(float pedal) {
    // Pedal 0 = losgelassen, 1 = getreten. Voll eingekuppelt unter 22 %, getrennt ab 78 %;
    // dazwischen steigt das Moment progressiv (Schleifpunkt bei ca. 55-65 %).
    float x = clampv((0.78f - pedal) / 0.56f, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
} // namespace

float Vehicle::wheelRadius() { return R; }


// Motorvariante des Autos: 0 Benzin (1,4 l Turbo), 1 Diesel (1,6 l Turbo)
static void applyEngine(int e) {
    if (e == 1) { IDLE = 800; RLIMIT = 4800; I_ENGINE = 0.34f; STALL = 300; START = 450; IDLEAUTH = 0.65f; STALLT = 0.8f; FRICK = 1.15f; }
    else { IDLE = 850; RLIMIT = 6500; I_ENGINE = 0.23f; STALL = 350; START = 480; IDLEAUTH = 0.28f; STALLT = 0.35f; FRICK = 1.0f; }
}
void Vehicle::setEngine(int e) {
    engineType = e == 1 ? 1 : 0;
    ENG = engineType;
    if (kind == 0) applyEngine(ENG);
}
Vehicle::Vehicle() { setKind(0); }

// Fahrzeugart: 0 Auto (Kompaktwagen), 1 Moped (Derbi Senda 50, 6 Gaenge, ca. 50 km/h), 2 Motorrad (125 ccm, ca. 100 km/h).
// Gleiche Physik, andere Kennwerte; die Spurweite der virtuellen Stuetzpunkte bleibt breit genug, damit nichts umkippt.
void Vehicle::setKind(int k) {
    kind = k;
    static const float carGears[8] = {-3.60f, 0.0f, 3.78f, 2.12f, 1.36f, 1.03f, 0.84f, 0.70f};
    for (int i = 0; i < 8; i++) GEARS[i] = carGears[i];
    if (k == 0) {
        MASS = 1300; INERTIA = {1900, 2100, 550}; R = 0.316f; SUS_REST = 0.30f; SUS_MAX = 0.26f; I_WHEEL = 1.1f; I_ENGINE = 0.23f; FINAL = 3.65f;
        CLUTCH_K = 40; CLUTCH_CAP = 330; MAX_STEER = 0.56f; TQ = 1; RPMK = 1; IDLE = 850; RLIMIT = 6500; STALL = 350; START = 480;
        BRAKE_F = 1900; BRAKE_R = 1000; BRAKE_H = 1600; STAB_F = 12000; STAB_R = 6000; MUN = 3500; CDA = 0.66f; TWIDTH = 0.77f; ZF = 1.10f; ZR = 1.52f;
        KF = 29000; CF = 2400; KR = 21000; CR = 1750;
    } else {
        bool moped = k == 1;
        float m = moped ? 185.0f : 255.0f, rel = m / 1300.0f;
        MASS = m; INERTIA = {m * 0.16f, m * 0.18f, m * 0.07f}; R = 0.30f; SUS_REST = 0.30f; SUS_MAX = 0.26f;
        I_WHEEL = moped ? 0.25f : 0.35f;
        TQ = moped ? 0.016f : 0.065f; I_ENGINE = 0.23f * TQ * 1.5f;
        RPMK = moped ? 0.5f : 0.42f; IDLE = moped ? 1700.0f : 1300.0f; RLIMIT = moped ? 7500.0f : 9500.0f; STALL = IDLE * 0.45f; START = IDLE * 0.6f;
        FINAL = moped ? 24.3f : 13.6f;
        CLUTCH_K = 40 * TQ; CLUTCH_CAP = 330 * TQ * 1.6f;
        MAX_STEER = 0.42f;
        BRAKE_F = 1900 * rel * 1.7f; BRAKE_R = 1000 * rel * 1.7f; BRAKE_H = 1600 * rel;
        STAB_F = 12000 * rel; STAB_R = 6000 * rel; MUN = 3500 * rel; CDA = 0.42f;
        TWIDTH = 0.62f; ZF = 0.64f; ZR = 0.64f;
        KF = 29000 * rel * 1.2f; CF = 2400 * rel * 1.4f; KR = 21000 * rel * 1.2f; CR = 1750 * rel * 1.4f;
        static const float bikeGears[8] = {-3.0f, 0.0f, 3.4f, 2.4f, 1.8f, 1.3f, 1.0f, 0.78f};
        for (int i = 0; i < 8; i++) GEARS[i] = bikeGears[i];
        if (moped) FINAL = 24.3f * 0.70f / 0.78f; // 6. Gang: ca. 50 km/h bei 7500 U/min
    }
    if (k == 0) applyEngine(ENG);
    else { IDLEAUTH = 0.28f; STALLT = 0.35f; FRICK = 1.0f; }
    const float my = -0.05f;
    w[0].mount = {-TWIDTH, my, -ZF}; w[1].mount = {TWIDTH, my, -ZF};
    w[2].mount = {-TWIDTH, my, ZR}; w[3].mount = {TWIDTH, my, ZR};
    for (int i = 0; i < 4; i++) {
        w[i].front = i < 2; w[i].driven = i < 2;
        w[i].k = i < 2 ? KF : KR;
        w[i].c = i < 2 ? CF : CR;
    }
}

float Vehicle::speed() const { return dot(vel, forward()); }

Vec3 Vehicle::hubLocal(int i) const { return w[i].mount - Vec3{0, SUS_REST - w[i].comp, 0}; }

void Vehicle::place(Vec3 p, float yaw) {
    pos = p; vel = {}; angVel = {};
    q = qaxis({0, 1, 0}, yaw);
    for (Wheel& wh : w) { wh.omega = 0; wh.comp = 0.12f; wh.hasGround = false; }
}

void Vehicle::shift(int dir) {
    if (automatic) { selector = clampv(selector + dir, 0, 3); return; }
    requestGear(clampv(gear + dir, -1, 6));
}

void Vehicle::requestGear(int g) {
    if (automatic || g == gear) return;
    // Ohne getretene Kupplung geht bei laufendem Motor kein Gang rein (bei stehendem Motor und Stillstand schon)
    bool still = !engineOn && std::fabs(speed()) < 0.5f;
    if (g != 0 && !autoClutch && clutch < 0.7f && !still) { evGrind = true; return; }
    if ((g == -1 && speed() > 2.0f) || (g > 0 && speed() < -2.0f)) { evGrind = true; return; }
    gear = g;
    if (autoClutch) shiftTimer = 0.28f;
}

// Automatik-Schaltlogik und automatische Kupplung (Tastatur-Hilfe)
void Vehicle::logic(float dt) {
    float r = rpm();
    shiftTimer = std::max(0.0f, shiftTimer - dt);
    autoShiftCd = std::max(0.0f, autoShiftCd - dt);
    if (automatic) {
        if (selector == 1) gear = -1;
        else if (selector == 3) {
            if (speed() < 1.0f) autoGear = 1;
            if (autoShiftCd <= 0) {
                if (r > 2300 + 3300 * throttle && autoGear < 6) { autoGear++; autoShiftCd = 0.9f; shiftTimer = 0.2f; }
                else if (r < 1150 + 1200 * throttle && autoGear > 1) { autoGear--; autoShiftCd = 0.9f; shiftTimer = 0.2f; }
            }
            gear = autoGear;
        } else gear = 0;
    }
    if (automatic || autoClutch) {
        float ratio = GEARS[gear + 1] * FINAL;
        float inRpm = std::fabs(0.5f * (w[0].omega + w[1].omega) * ratio) * 9.5493f;
        float want;
        if (gear == 0 || shiftTimer > 0 || !engineOn) want = 0;
        else if (inRpm > 1150) want = 1;
        else {
            want = clampv((r - (IDLE + 130.0f)) / 700.0f, 0.0f, 1.0f);
            if (automatic && brake < 0.1f) want = std::max(want, 0.07f); // Kriechen
            if (brake > 0.2f && throttle < 0.05f) want = 0;
            if (r < IDLE - 70.0f) want = 0;
        }
        autoEngage += clampv(want - autoEngage, -dt * 8.0f, dt * 3.0f);
    }
}

void Vehicle::update(float dt, World& world) {
    evStall = false; // evGrind setzt requestGear(); das Hauptprogramm wertet es aus und loescht es
    dt = clampv(dt, 0.0f, 0.05f);
    int grounded = 0;
    for (Wheel& wh : w) {
        Vec3 m = pos + qrot(q, wh.mount);
        float h;
        float yRef = pos.y - 0.2f; // Stufen bis ca. 35 cm werden ueberfahren, hoehere Kanten sind Hindernisse
        wh.hasGround = world.surface(m.x, m.z, yRef, h);
        if (wh.hasGround) {
            wh.gp = {m.x, h, m.z};
            wh.gn = world.surfaceNormal(m.x, m.z, yRef);
            grounded++;
        }
        wh.mu = onRoad ? 1.0f : 0.62f;
        wh.crr = onRoad ? 0.012f : 0.07f;
    }
    if (grounded < 4) return; // Gelaende noch nicht geladen: Auto anhalten
    float gh;
    if (world.surface(pos.x, pos.z, pos.y - 0.2f, gh) && pos.y < gh - 0.2f) { pos.y = gh + 0.6f; vel.y = 0; }

    float st = steer * MAX_STEER;
    w[0].steer = w[1].steer = st;
    logic(dt);
    int n = std::max(1, (int)std::ceil(dt / 0.001f));
    float h = dt / n;
    ffb = 0;
    Vec3 p0 = pos;
    for (int i = 0; i < n; i++) substep(h);
    ffb /= n;
    odo += length(Vec3{pos.x - p0.x, 0, pos.z - p0.z});
    for (Wheel& wh : w) wh.rot = std::fmod(wh.rot + wh.omega * dt, 2 * PI);
}

void Vehicle::substep(float dt) {
    Vec3 right = qrot(q, {1, 0, 0}), up = qrot(q, {0, 1, 0}), fwd = qrot(q, {0, 0, -1});
    Vec3 F{0, -MASS * 9.81f, 0}, T{};
    float sp = length(vel);
    F += vel * (-0.5f * 1.2f * CDA * sp); // Luftwiderstand

    // --- Federung ---
    float fs[4];
    Vec3 cp[4];
    for (int i = 0; i < 4; i++) {
        Wheel& wh = w[i];
        fs[i] = 0;
        wh.contact = false;
        Vec3 mw = pos + qrot(q, wh.mount);
        float dn = dot(up, wh.gn);
        float c = 0;
        if (dn > 0.25f) {
            float dist = dot(mw - wh.gp, wh.gn) / dn;
            c = SUS_REST + R - dist;
            if (c > 0) {
                wh.contact = true;
                float cv = clampv((c - wh.comp) / dt, -4.0f, 4.0f);
                float f = wh.k * c + wh.c * cv;
                if (c > SUS_MAX) f += wh.k * 12.0f * (c - SUS_MAX);
                fs[i] = std::max(f, 0.0f);
                cp[i] = mw - up * dist;
            }
        }
        wh.comp = wh.contact ? std::min(c, SUS_MAX + 0.08f) : 0.0f;
    }
    for (int a = 0; a < 4; a += 2) { // Stabilisatoren
        float dlt = (w[a].comp - w[a + 1].comp) * (a == 0 ? STAB_F : STAB_R);
        if (w[a].contact) fs[a] = std::max(0.0f, fs[a] + dlt);
        if (w[a + 1].contact) fs[a + 1] = std::max(0.0f, fs[a + 1] - dlt);
    }

    // --- Motor und Kupplung ---
    float ratio = GEARS[gear + 1] * FINAL;
    float engage = (automatic || autoClutch) ? autoEngage : clutchEngage(clutch);
    float Tc = 0, Keff = 0;
    if (ratio != 0 && engage > 0) {
        float wg = 0.5f * (w[0].omega + w[1].omega) * ratio;
        float raw = CLUTCH_K * (engW - wg), cap = CLUTCH_CAP * engage;
        if (std::fabs(raw) > cap) Tc = raw > 0 ? cap : -cap;
        else { Tc = raw; Keff = CLUTCH_K; }
    }
    // Motor (1,4-l-Turbobenziner, 200 Nm / 92 kW):
    //  - Die Drosselklappe begrenzt den Luftstrom. Bei wenig Gas faellt die Fuellung mit steigender Drehzahl,
    //    deshalb pendelt sich der Motor ohne Last auf eine zur Pedalstellung passende Drehzahl ein
    //    (ca. 10 % -> 2000, 15 % -> 3000, 20 % -> 4200 U/min) statt in den Begrenzer zu laufen.
    //  - Saugrohr (ca. 0,09 s) und Turbolader (0,2 .. 1,1 s je nach Drehzahl) verzoegern den Momentenaufbau.
    //  - Reib- und Pumpverluste entsprechen ca. 1 .. 2 bar Reibmitteldruck (15 Nm bei 2000, 29 Nm bei 6000 U/min).
    float r = rpm();
    float n = std::max(r, 300.0f) / 1000.0f;
    float fric = (8.0f + 0.0035f * r) * TQ * FRICK;
    float Te;
    if (engineOn) {
        bool diesel = ENG == 1 && kind == 0;
        float area = 1.2f * throttle + 25.0f * throttle * throttle * throttle;
        float req = diesel ? throttle : 1.0f - std::exp(-area / n); // Diesel: Einspritzmenge folgt dem Pedal, keine Drosselklappe
        // Leerlaufregler: haelt die Leerlaufdrehzahl mit begrenzter Momentenreserve (Benzin knapp, Diesel kraeftig)
        req = std::max(req, clampv((IDLE - r) * (diesel ? 0.0050f : 0.0030f), 0.0f, IDLEAUTH));
        if (r > RLIMIT) req = 0;                                              // Drehzahlbegrenzer
        manifold += (req - manifold) * std::min(1.0f, dt / 0.09f);
        float boostTarget = diesel ? manifold : std::max(0.0f, manifold - 0.6f) / 0.4f;
        float tau = boostTarget < boost ? 0.15f : (diesel ? clampv(1.4f - 0.35f * n, 0.25f, 1.0f) : clampv(1.5f - 0.33f * n, 0.22f, 1.1f)); // Turboloch
        boost += (boostTarget - boost) * std::min(1.0f, dt / tau);
        float fill = diesel ? 0.55f * manifold + 0.45f * boost : std::min(manifold, 0.6f) + 0.4f * boost;
        float pump = (1.0f - fill) * (diesel ? 2.0f + 0.0008f * r : 4.0f + 0.0012f * r) * TQ;                  // Drosselverluste = Motorbremse
        Te = fill * (torqueCurve(r) + fric) - fric - pump;
    } else {
        manifold = boost = 0;
        Te = -(fric + (4.0f + 0.0012f * r) * TQ) * clampv(engW / 2.0f, 0.0f, 1.0f);
        if (starter) Te += 75.0f * TQ;
    }

    // --- Reifen ---
    for (int i = 0; i < 4; i++) {
        Wheel& wh = w[i];
        float Tdrive = wh.driven ? Tc * ratio * 0.5f * 0.93f : 0.0f; // 7 % Verluste in Getriebe und Differential
        float Dc = wh.driven ? Keff * ratio * ratio : 0.0f;
        float Fx = 0, Dt = 0;
        float Tb = brake * (wh.front ? BRAKE_F : BRAKE_R);
        if (!wh.front && (handbrake || (automatic && selector == 0))) Tb += BRAKE_H;
        wh.slip = 0;
        if (wh.contact) {
            Vec3 nrm = wh.gn;
            Vec3 hd = fwd * std::cos(wh.steer) + right * std::sin(wh.steer);
            Vec3 f = normalize(hd - nrm * dot(hd, nrm));
            Vec3 s = cross(f, nrm);
            Vec3 rc = cp[i] - pos;
            Vec3 vc = vel + cross(angVel, rc);
            float vx = dot(vc, f), vy = dot(vc, s);
            float den = std::max(std::fabs(vx), 0.5f);
            float sr = (wh.omega * R - vx) / den;
            float sa = vy / (std::fabs(vx) + 0.5f);
            float sx = sr / SR_PEAK, sy = sa / SA_PEAK;
            float rho = std::sqrt(sx * sx + sy * sy);
            float mu = wh.mu * clampv(1.12f - 0.1f * fs[i] / MUN, 0.75f, 1.12f);
            const float B = 1.732f, C = 1.5f;
            float at = std::atan(B * rho);
            float cur = std::sin(C * at), dcur = C * B / (1 + B * B * rho * rho) * std::cos(C * at);
            float Fmax = mu * fs[i], Fy = 0;
            if (rho > 1e-5f) { Fx = Fmax * cur * sx / rho; Fy = -Fmax * cur * sy / rho; }
            Dt = R * Fmax * std::max(dcur, 0.0f) / SR_PEAK * R / den;
            float Frr = -wh.crr * fs[i] * std::tanh(vx * 2.0f);
            Vec3 Fw = f * (Fx + Frr) + s * Fy + nrm * fs[i];
            F += Fw;
            T += cross(rc, Fw);
            wh.slip = rho;
            if (wh.front) { // Rueckstellmoment fuer Force Feedback
                float trail = 0.022f + 0.04f * std::max(0.0f, 1.0f - std::fabs(sy) * 0.8f);
                ffb += -Fy * trail / STEER_RATIO * 0.25f; // 0.25 = Servounterstuetzung
            }
            if (Tb > 0 && vx > 3.0f && sr < -0.2f) Tb *= 0.25f;  // ABS
            if (Tb > 0 && vx < -3.0f && sr > 0.2f) Tb *= 0.25f;
        }
        float Ieff = I_WHEEL + dt * (Dt + Dc);
        float wn = wh.omega + dt * (Tdrive - R * Fx) / Ieff;
        float db = Tb * dt / Ieff;
        if (std::fabs(wn) <= db) wn = 0;
        else wn -= wn > 0 ? db : -db;
        wh.omega = wn;
    }

    // --- Motor ---
    engW += dt * (Te - Tc) / (I_ENGINE + dt * 2.0f * Keff);
    if (engW < 0) engW = 0;
    // Abwuergen: erst wenn die Drehzahl laenger unter der Schwelle bleibt (Schwungrad, Leerlaufregler); sehr niedrig sofort
    if (engineOn && engW * 9.5493f < STALL) stallTimer += dt; else stallTimer = 0;
    if (engineOn && (stallTimer > STALLT || engW * 9.5493f < STALL * 0.45f)) { engineOn = false; evStall = true; stallTimer = 0; }
    if (!engineOn && starter && engW * 9.5493f > START) engineOn = true;

    // --- Starrkoerper integrieren ---
    vel += F * (dt / MASS);
    Quat qc = qconj(q);
    Vec3 wb = qrot(qc, angVel), tb = qrot(qc, T);
    Vec3 Iw{INERTIA.x * wb.x, INERTIA.y * wb.y, INERTIA.z * wb.z};
    Vec3 gy = cross(wb, Iw);
    wb += Vec3{(tb.x - gy.x) / INERTIA.x, (tb.y - gy.y) / INERTIA.y, (tb.z - gy.z) / INERTIA.z} * dt;
    wb = wb * (1.0f - 0.05f * dt);
    angVel = qrot(q, wb);
    pos += vel * dt;
    Quat dq = qmul(Quat{0, angVel.x, angVel.y, angVel.z}, q);
    q = qnorm({q.w + 0.5f * dt * dq.w, q.x + 0.5f * dt * dq.x, q.y + 0.5f * dt * dq.y, q.z + 0.5f * dt * dq.z});
}
