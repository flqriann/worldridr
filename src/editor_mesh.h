#pragma once
// Bearbeitbare Meshes fuer das Studio: Eckpunkte, Flaechen (beliebige Polygone) mit eigener Farbe.
// Reine Geometrie ohne Abhaengigkeit vom Editorzustand: Formen, Extrudieren, Teilen, Loeschen, Auswahl per Strahl.
#include "gfx.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct EFace {
    std::vector<int> v;
    Color c{205, 205, 205, 255};
};
struct EMesh {
    std::vector<Vec3> v;
    std::vector<EFace> f;
    bool empty() const { return f.empty(); }
};

namespace emesh {

inline Vec3 faceNormal(const EMesh& m, int fi) { // Newell: robust auch fuer unebene Polygone
    const EFace& f = m.f[fi];
    Vec3 n{};
    size_t k = f.v.size();
    for (size_t i = 0; i < k; i++) {
        const Vec3& a = m.v[f.v[i]];
        const Vec3& b = m.v[f.v[(i + 1) % k]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return normalize(n);
}
inline Vec3 faceCenter(const EMesh& m, int fi) {
    Vec3 c{};
    for (int i : m.f[fi].v) c += m.v[i];
    return c * (1.0f / std::max<size_t>(1, m.f[fi].v.size()));
}
inline void addFace(EMesh& m, std::vector<int> idx, Color c) { m.f.push_back({std::move(idx), c}); }
inline void orientFace(EMesh& m, int fi, Vec3 want) { // Umlaufsinn so waehlen, dass die Normale in 'want' zeigt
    if (dot(faceNormal(m, fi), want) < 0) std::reverse(m.f[fi].v.begin(), m.f[fi].v.end());
}
// Alle Flaechen eines konvexen Koerpers nach aussen ausrichten
inline void orientConvex(EMesh& m) {
    Vec3 c{};
    for (const Vec3& p : m.v) c += p;
    c = c * (1.0f / std::max<size_t>(1, m.v.size()));
    for (int i = 0; i < (int)m.f.size(); i++) orientFace(m, i, faceCenter(m, i) - c);
}

// ---------- Formen (lokale Groesse = size, Mittelpunkt im Ursprung) ----------
inline EMesh block(Vec3 s) {
    EMesh m;
    for (int i = 0; i < 8; i++) m.v.push_back({(i & 1 ? 0.5f : -0.5f) * s.x, (i & 2 ? 0.5f : -0.5f) * s.y, (i & 4 ? 0.5f : -0.5f) * s.z});
    const int F[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    for (auto& f : F) addFace(m, {f[0], f[1], f[2], f[3]}, Color{});
    orientConvex(m);
    return m;
}
inline EMesh cylinder(Vec3 s, int n = 16) {
    EMesh m;
    for (int i = 0; i < n; i++) {
        float a = i * 2 * PI / n;
        m.v.push_back({std::cos(a) * 0.5f * s.x, -0.5f * s.y, std::sin(a) * 0.5f * s.z});
    }
    for (int i = 0; i < n; i++) m.v.push_back({m.v[i].x, 0.5f * s.y, m.v[i].z});
    std::vector<int> top, bot;
    for (int i = 0; i < n; i++) { bot.push_back(i); top.push_back(n + i); }
    addFace(m, bot, Color{}); addFace(m, top, Color{});
    for (int i = 0; i < n; i++) addFace(m, {i, (i + 1) % n, n + (i + 1) % n, n + i}, Color{});
    orientConvex(m);
    return m;
}
inline EMesh cone(Vec3 s, int n = 16) {
    EMesh m;
    for (int i = 0; i < n; i++) { float a = i * 2 * PI / n; m.v.push_back({std::cos(a) * 0.5f * s.x, -0.5f * s.y, std::sin(a) * 0.5f * s.z}); }
    m.v.push_back({0, 0.5f * s.y, 0});
    std::vector<int> bot;
    for (int i = 0; i < n; i++) bot.push_back(i);
    addFace(m, bot, Color{});
    for (int i = 0; i < n; i++) addFace(m, {i, (i + 1) % n, n}, Color{});
    orientConvex(m);
    return m;
}
inline EMesh pyramid(Vec3 s) {
    EMesh m;
    m.v = {{-0.5f * s.x, -0.5f * s.y, -0.5f * s.z}, {0.5f * s.x, -0.5f * s.y, -0.5f * s.z}, {0.5f * s.x, -0.5f * s.y, 0.5f * s.z}, {-0.5f * s.x, -0.5f * s.y, 0.5f * s.z}, {0, 0.5f * s.y, 0}};
    addFace(m, {0, 1, 2, 3}, Color{});
    for (int i = 0; i < 4; i++) addFace(m, {i, (i + 1) % 4, 4}, Color{});
    orientConvex(m);
    return m;
}
inline EMesh sphere(Vec3 s, int la = 8, int lo = 14) {
    EMesh m;
    m.v.push_back({0, 0.5f * s.y, 0});
    for (int i = 1; i < la; i++) {
        float th = i * PI / la;
        for (int k = 0; k < lo; k++) {
            float ph = k * 2 * PI / lo;
            m.v.push_back({std::sin(th) * std::cos(ph) * 0.5f * s.x, std::cos(th) * 0.5f * s.y, std::sin(th) * std::sin(ph) * 0.5f * s.z});
        }
    }
    m.v.push_back({0, -0.5f * s.y, 0});
    int bot = (int)m.v.size() - 1;
    auto ring = [&](int i, int k) { return 1 + (i - 1) * lo + ((k % lo) + lo) % lo; };
    for (int k = 0; k < lo; k++) addFace(m, {0, ring(1, k + 1), ring(1, k)}, Color{});
    for (int i = 1; i + 1 < la; i++)
        for (int k = 0; k < lo; k++) addFace(m, {ring(i, k), ring(i, k + 1), ring(i + 1, k + 1), ring(i + 1, k)}, Color{});
    for (int k = 0; k < lo; k++) addFace(m, {bot, ring(la - 1, k), ring(la - 1, k + 1)}, Color{});
    orientConvex(m);
    return m;
}
inline EMesh plate(Vec3 s) {
    EMesh m;
    m.v = {{-0.5f * s.x, 0, -0.5f * s.z}, {0.5f * s.x, 0, -0.5f * s.z}, {0.5f * s.x, 0, 0.5f * s.z}, {-0.5f * s.x, 0, 0.5f * s.z}};
    addFace(m, {0, 1, 2, 3}, Color{});
    addFace(m, {3, 2, 1, 0}, Color{}); // Rueckseite (sonst waere sie von unten unsichtbar)
    orientFace(m, 0, {0, 1, 0});
    orientFace(m, 1, {0, -1, 0});
    return m;
}
// Profil in der z-y-Ebene (Punkte gegen den Uhrzeigersinn) entlang x extrudiert: Keil, Dach, Treppe, L-Winkel
inline EMesh profile(std::vector<std::pair<float, float>> pts, float width) {
    EMesh m;
    float area = 0;
    for (size_t i = 0; i < pts.size(); i++) { auto& a = pts[i]; auto& b = pts[(i + 1) % pts.size()]; area += a.first * b.second - b.first * a.second; }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    int n = (int)pts.size();
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < n; i++) m.v.push_back({side ? width * 0.5f : -width * 0.5f, pts[i].second, pts[i].first});
    std::vector<int> l, r;
    for (int i = 0; i < n; i++) { l.push_back(i); r.push_back(n + i); }
    addFace(m, l, Color{}); addFace(m, r, Color{});
    orientFace(m, 0, {-1, 0, 0});
    orientFace(m, 1, {1, 0, 0});
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dz = pts[j].first - pts[i].first, dy = pts[j].second - pts[i].second;
        addFace(m, {i, j, n + j, n + i}, Color{});
        orientFace(m, (int)m.f.size() - 1, {0, -dz, dy}); // nach aussen: rechte Normale des Profils (dy, -dz) in (y, z)
    }
    return m;
}
inline EMesh wedge(Vec3 s) { return profile({{-0.5f * s.z, -0.5f * s.y}, {0.5f * s.z, -0.5f * s.y}, {-0.5f * s.z, 0.5f * s.y}}, s.x); }
inline EMesh roof(Vec3 s) { return profile({{-0.5f * s.z, -0.5f * s.y}, {0.5f * s.z, -0.5f * s.y}, {0, 0.5f * s.y}}, s.x); }
inline EMesh stairs(Vec3 s, int steps = 6) {
    // Treppe steigt in +z an: Profil unten durchgehend, oben Stufen von hinten (hoch) nach vorn (niedrig)
    std::vector<std::pair<float, float>> q;
    q.push_back({-0.5f * s.z, -0.5f * s.y});
    q.push_back({0.5f * s.z, -0.5f * s.y});
    for (int i = steps; i >= 1; i--) {
        float zEnd = 0.5f * s.z - s.z * (steps - i) / steps;
        float zStart = zEnd - s.z / steps;
        float y = -0.5f * s.y + s.y * i / steps;
        q.push_back({zEnd, y});
        q.push_back({zStart, y});
    }
    return profile(q, s.x);
}
inline EMesh lshape(Vec3 s) {
    float t = 0.3f;
    return profile({{-0.5f * s.z, -0.5f * s.y}, {0.5f * s.z, -0.5f * s.y}, {0.5f * s.z, -0.5f * s.y + t * s.y}, {-0.5f * s.z + t * s.z, -0.5f * s.y + t * s.y},
                    {-0.5f * s.z + t * s.z, 0.5f * s.y}, {-0.5f * s.z, 0.5f * s.y}}, s.x);
}

// ---------- Darstellung ----------
inline void triangulate(const EMesh& m, std::vector<Vertex>& out) {
    out.clear();
    for (int fi = 0; fi < (int)m.f.size(); fi++) {
        const EFace& f = m.f[fi];
        if (f.v.size() < 3) continue;
        Vec3 n = faceNormal(m, fi);
        for (size_t k = 1; k + 1 < f.v.size(); k++) {
            int id[3] = {f.v[0], f.v[k], f.v[k + 1]};
            for (int q : id) out.push_back({{m.v[q].x, m.v[q].y, m.v[q].z}, {n.x, n.y, n.z}, {0, 0}, f.c});
        }
    }
}
inline bool rayTri(Vec3 ro, Vec3 rd, Vec3 a, Vec3 b, Vec3 c, float& t) {
    Vec3 e1 = b - a, e2 = c - a, p = cross(rd, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-9f) return false;
    float inv = 1.0f / det;
    Vec3 s = ro - a;
    float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    Vec3 q = cross(s, e1);
    float v = dot(rd, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t > 1e-4f;
}
inline int pickFace(const EMesh& m, Vec3 ro, Vec3 rd, float& tBest) {
    int best = -1;
    tBest = 1e30f;
    for (int fi = 0; fi < (int)m.f.size(); fi++) {
        const EFace& f = m.f[fi];
        for (size_t k = 1; k + 1 < f.v.size(); k++) {
            float t;
            if (rayTri(ro, rd, m.v[f.v[0]], m.v[f.v[k]], m.v[f.v[k + 1]], t) && t < tBest) { tBest = t; best = fi; }
        }
    }
    return best;
}

// ---------- Bearbeitung ----------
inline void compact(EMesh& m) { // unbenutzte Eckpunkte entfernen
    std::vector<int> map(m.v.size(), -1);
    std::vector<Vec3> nv;
    for (EFace& f : m.f)
        for (int& i : f.v) {
            if (map[i] < 0) { map[i] = (int)nv.size(); nv.push_back(m.v[i]); }
            i = map[i];
        }
    m.v = std::move(nv);
}
inline void deleteFaces(EMesh& m, const std::vector<char>& sel) {
    std::vector<EFace> keep;
    for (size_t i = 0; i < m.f.size(); i++) if (!(i < sel.size() && sel[i])) keep.push_back(m.f[i]);
    m.f = std::move(keep);
    compact(m);
}
// Ausgewaehlte Flaechen extrudieren: neue Eckpunkte + Seitenflaechen. Liefert die neuen Eckpunkte (zum anschliessenden Verschieben)
inline std::vector<int> extrudeFaces(EMesh& m, const std::vector<char>& sel) {
    std::map<int, int> map;
    std::map<std::pair<int, int>, int> edgeCount;
    int total = (int)m.f.size();
    for (int fi = 0; fi < total; fi++) {
        if (!(fi < (int)sel.size() && sel[fi])) continue;
        const EFace& f = m.f[fi];
        for (size_t k = 0; k < f.v.size(); k++) {
            int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
            edgeCount[{std::min(a, b), std::max(a, b)}]++;
        }
    }
    std::vector<int> created;
    for (int fi = 0; fi < total; fi++) {
        if (!(fi < (int)sel.size() && sel[fi])) continue;
        EFace f = m.f[fi]; // Kopie: m.f waechst unten
        Vec3 fc = faceCenter(m, fi), fn = faceNormal(m, fi);
        for (int i : f.v)
            if (!map.count(i)) { map[i] = (int)m.v.size(); m.v.push_back(m.v[i]); created.push_back(map[i]); }
        for (size_t k = 0; k < f.v.size(); k++) {
            int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
            if (edgeCount[{std::min(a, b), std::max(a, b)}] != 1) continue; // nur Randkanten bekommen eine Seitenflaeche
            EFace s;
            s.c = f.c;
            s.v = {a, b, map[b], map[a]};
            m.f.push_back(s); // (a, b, b', a') ist bei nach aussen gerichteten Flaechen bereits richtig herum
        }
    }
    for (int fi = 0; fi < total; fi++)
        if (fi < (int)sel.size() && sel[fi]) for (int& i : m.f[fi].v) i = map[i];
    return created;
}
// Flaeche in n Vierecke teilen (Mittelpunkt + Kantenmitten)
inline void subdivideFace(EMesh& m, int fi) {
    EFace f = m.f[fi];
    int n = (int)f.v.size();
    if (n < 3) return;
    int c = (int)m.v.size();
    m.v.push_back(faceCenter(m, fi));
    std::vector<int> mid(n);
    for (int k = 0; k < n; k++) { mid[k] = (int)m.v.size(); m.v.push_back((m.v[f.v[k]] + m.v[f.v[(k + 1) % n]]) * 0.5f); }
    m.f.erase(m.f.begin() + fi);
    for (int k = 0; k < n; k++) m.f.push_back({{f.v[k], mid[k], c, mid[(k + n - 1) % n]}, f.c});
}
inline void bounds(const EMesh& m, Vec3& lo, Vec3& hi) {
    lo = {1e9f, 1e9f, 1e9f}; hi = {-1e9f, -1e9f, -1e9f};
    for (const Vec3& p : m.v) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (m.v.empty()) { lo = {-0.5f, -0.5f, -0.5f}; hi = {0.5f, 0.5f, 0.5f}; }
}

} // namespace emesh
