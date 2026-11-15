// Kartenformat "worldridr-map" v1: Manifest + eine JSON-Datei pro Slippy-Map-Kachel, ein Objekt/eine Strasse pro Zeile,
// sortiert nach ID und mit fester Rundung (damit Git-Diffs klein bleiben). Gegenstueck: studio_format (World Studio, Rust).
#include "mapformat.h"
#include "common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <climits>
#include <mutex>
#include <map>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

namespace mapformat {
namespace {
const char* MANIFEST = "worldridr-map.json";
const char* SHAPES[] = {"block", "cylinder", "wedge", "sphere", "model", "mesh"};

bool readText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool writeIfChanged(const fs::path& p, const std::string& text) {
    std::string old;
    if (readText(p, old) && old == text) return false;
    fs::path tmp = p;
    tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << text; }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    return true;
}
double rnd(double v, int dec) { double m = std::pow(10.0, dec); return std::round(v * m) / m; }

double lonToMx(double lon) { return (lon + 180.0) / 360.0; }
double latToMy(double lat) {
    lat = std::max(-85.05, std::min(85.05, lat));
    return (1.0 - std::asinh(std::tan(lat * 3.14159265358979323846 / 180.0)) / 3.14159265358979323846) / 2.0;
}
std::pair<long, long> tileOf(double lat, double lon, int zoom) {
    double n = (double)(1ull << zoom);
    long x = (long)std::floor(lonToMx(lon) * n), y = (long)std::floor(latToMy(lat) * n);
    long mx = (long)n - 1;
    return {std::max(0L, std::min(mx, x)), std::max(0L, std::min(mx, y))};
}
int shapeIndex(const std::string& s) {
    for (int i = 0; i < 6; i++) if (s == SHAPES[i]) return i;
    return 0;
}
// Unbekannte (neuere) Felder unveraendert durchreichen, damit Erweiterungen des Spiels nicht verloren gehen.
void copyExtras(const json& from, ojson& to, std::initializer_list<const char*> known) {
    for (auto it = from.begin(); it != from.end(); ++it) {
        bool k = false;
        for (const char* n : known) if (it.key() == n) k = true;
        if (!k) to[it.key()] = ojson::parse(it.value().dump());
    }
}
} // namespace

std::string newId() {
    static std::mt19937_64 rng{std::random_device{}()};
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)rng());
    return b;
}

bool exists(const std::string& dir) { return fs::exists(fs::path(dir) / MANIFEST); }

bool load(const std::string& dir, json& legacy) {
    std::string text;
    if (!readText(fs::path(dir) / MANIFEST, text)) return false;
    json man = json::parse(text, nullptr, false);
    if (man.is_discarded() || man.value("format", "") != "worldridr-map" || man.value("version", 99) > 1) {
        logMsg("Karte %s: unbekanntes Format oder zu neue Version", dir.c_str());
        return false;
    }
    legacy = json::object();
    legacy["objects"] = json::array();
    legacy["roads"] = json::array();
    legacy["palette"] = man.contains("palette") ? man["palette"] : json::array();
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(dir) / "tiles", ec))
        if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string t;
        if (!readText(f, t)) continue;
        json tile = json::parse(t, nullptr, false);
        if (tile.is_discarded()) { logMsg("Kachel %s fehlerhaft (Merge-Konflikt?), uebersprungen", f.string().c_str()); continue; }
        for (json o : tile.value("objects", json::array())) {
            o["shape"] = shapeIndex(o.value("shape", "block"));
            legacy["objects"].push_back(o);
        }
        for (json r : tile.value("roads", json::array())) {
            r["type"] = r.value("kind", 0);
            legacy["roads"].push_back(r);
        }
    }
    return true;
}

bool save(const std::string& dir, const json& legacy) {
    fs::path root(dir);
    std::string text;
    if (!readText(root / MANIFEST, text)) return false;
    ojson man = ojson::parse(text, nullptr, false);
    if (man.is_discarded()) return false;
    int zoom = man.value("tile_zoom", 15);
    man["palette"] = legacy.contains("palette") ? ojson::parse(legacy["palette"].dump()) : ojson::array();
    writeIfChanged(root / MANIFEST, man.dump(2) + "\n");

    struct Item { std::string id, line; };
    std::map<std::pair<long, long>, std::pair<std::vector<Item>, std::vector<Item>>> tiles;
    for (const json& e : legacy.value("objects", json::array())) {
        ojson o;
        std::string id = e.value("id", "");
        if (id.empty()) id = newId();
        int sh = e.value("shape", 0);
        double lat = rnd(e.value("lat", 0.0), 8), lon = rnd(e.value("lon", 0.0), 8);
        o["id"] = id;
        o["name"] = e.value("name", "");
        o["shape"] = SHAPES[std::max(0, std::min(5, sh))];
        o["lat"] = lat;
        o["lon"] = lon;
        o["alt"] = rnd(e.value("alt", 0.0), 4);
        ojson rot = ojson::array(), scale = ojson::array();
        for (int i = 0; i < 3; i++) { rot.push_back(rnd(e["rot"][i].get<double>(), 5)); scale.push_back(rnd(e["scale"][i].get<double>(), 4)); }
        o["rot"] = rot;
        o["scale"] = scale;
        o["color"] = ojson::parse(e["color"].dump());
        if (!e.value("texture", "").empty()) o["texture"] = e["texture"];
        if (!e.value("model", "").empty()) o["model"] = e["model"];
        if (!e.value("solid", true)) o["solid"] = false;
        if (e.value("layer", "Default") != "Default") o["layer"] = e["layer"];
        if (e.contains("mesh")) o["mesh"] = ojson::parse(e["mesh"].dump());
        if (std::fabs(e.value("tile", 2.0) - 2.0) > 1e-6) o["tile"] = e["tile"];
        copyExtras(e, o, {"id", "uid", "name", "shape", "lat", "lon", "alt", "rot", "scale", "color", "texture", "model", "solid", "layer", "mesh", "tile"});
        tiles[tileOf(lat, lon, zoom)].first.push_back({id, o.dump()});
    }
    for (const json& e : legacy.value("roads", json::array())) {
        if (!e.contains("points") || e["points"].size() < 2) continue;
        ojson r;
        std::string id = e.value("id", "");
        if (id.empty()) id = newId();
        r["id"] = id;
        r["kind"] = e.value("type", 0);
        ojson pts = ojson::array();
        for (const json& p : e["points"]) pts.push_back({rnd(p[0].get<double>(), 8), rnd(p[1].get<double>(), 8), rnd(p[2].get<double>(), 3)});
        r["points"] = pts;
        copyExtras(e, r, {"id", "uid", "type", "kind", "points"});
        tiles[tileOf(e["points"][0][0].get<double>(), e["points"][0][1].get<double>(), zoom)].second.push_back({id, r.dump()});
    }
    fs::create_directories(root / "tiles");
    std::vector<std::string> keep;
    for (auto& [xy, lists] : tiles) {
        auto& [objs, roads] = lists;
        auto cmp = [](const Item& a, const Item& b) { return a.id < b.id; };
        std::sort(objs.begin(), objs.end(), cmp);
        std::sort(roads.begin(), roads.end(), cmp);
        std::string s = "{\n  \"format\": \"worldridr-tile\",\n  \"version\": 1,\n";
        s += "  \"tile\": {\"z\":" + std::to_string(zoom) + ",\"x\":" + std::to_string(xy.first) + ",\"y\":" + std::to_string(xy.second) + "},\n";
        for (int k = 0; k < 2; k++) {
            auto& items = k == 0 ? objs : roads;
            s += k == 0 ? "  \"objects\": [" : "  \"roads\": [";
            if (!items.empty()) {
                s += "\n";
                for (size_t i = 0; i < items.size(); i++) s += "    " + items[i].line + (i + 1 < items.size() ? ",\n" : "\n  ");
            }
            s += k == 0 ? "],\n" : "]\n";
        }
        s += "}\n";
        char name[96];
        snprintf(name, sizeof name, "z%d_x%ld_y%ld.json", zoom, xy.first, xy.second);
        writeIfChanged(root / "tiles" / name, s);
        keep.push_back(name);
    }
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / "tiles", ec)) {
        std::string n = e.path().filename().string();
        if (e.path().extension() == ".json" && std::find(keep.begin(), keep.end(), n) == keep.end()) fs::remove(e.path(), ec);
    }
    return true;
}

// ---------- Hoehenmodell-Ueberschreibung (terrain.json) ----------
namespace {
struct TerrainOv {
    bool ok = false;
    int zoom = 12;
    long px = 0, py = 0;
    int w = 0, h = 0;
    std::vector<int> dm; // INT_MIN = unbekannt
};
TerrainOv& terrainOv() {
    static TerrainOv t;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* e = std::getenv("WR_MAPDIR");
        fs::path p = fs::path(e && *e ? e : "maps/world") / "terrain.json";
        std::string text;
        if (!readText(p, text)) return;
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || j.value("format", "") != "worldridr-terrain") return;
        t.zoom = j.value("zoom", 12);
        t.px = j.value("px", 0L);
        t.py = j.value("py", 0L);
        t.w = j.value("w", 0);
        t.h = j.value("h", 0);
        if (t.w <= 0 || t.h <= 0 || t.zoom < 12 || t.zoom > 20) return;
        t.dm.assign((size_t)t.w * t.h, INT_MIN);
        const json& rows = j["rows"];
        for (int y = 0; y < t.h && y < (int)rows.size(); y++) {
            if (rows[y].is_string()) { // "Start Delta DeltaxAnzahl nxAnzahl ..."
                std::istringstream ss(rows[y].get<std::string>());
                std::string tok;
                int x = 0, last = 0;
                bool have = false;
                while (ss >> tok && x < t.w) {
                    if (tok[0] == 'n') { // Luecke
                        size_t cx = tok.find('x');
                        x += cx == std::string::npos ? 1 : std::atoi(tok.c_str() + cx + 1);
                        have = false;
                        continue;
                    }
                    if (!have) { last = std::atoi(tok.c_str()); t.dm[(size_t)y * t.w + x] = last; x++; have = true; continue; }
                    size_t cx = tok.find('x');
                    int d = std::atoi(tok.c_str()), n = cx == std::string::npos ? 1 : std::atoi(tok.c_str() + cx + 1);
                    for (int k = 0; k < n && x < t.w; k++, x++) { last += d; t.dm[(size_t)y * t.w + x] = last; }
                }
            } else {
                for (int x = 0; x < t.w && x < (int)rows[y].size(); x++)
                    if (rows[y][x].is_number()) t.dm[(size_t)y * t.w + x] = rows[y][x].get<int>();
            }
        }
        t.ok = true;
        logMsg("Gelaende aus Karte: %dx%d Pixel, Zoom %d", t.w, t.h, t.zoom);
    });
    return t;
}
bool terrainAt(const TerrainOv& t, long ix, long iy, float& out) {
    long x = ix - t.px, y = iy - t.py;
    if (x < 0 || y < 0 || x >= t.w || y >= t.h) return false;
    int v = t.dm[(size_t)y * t.w + x];
    if (v == INT_MIN) return false;
    out = v / 10.0f;
    return true;
}
} // namespace

void applyTerrain(int tx, int ty, float* heights) {
    const TerrainOv& t = terrainOv();
    if (!t.ok) return;
    const double PI_ = 3.14159265358979323846;
    double s12 = 4096.0 * 256.0, sz = std::ldexp(256.0, t.zoom);
    for (int iy = 0; iy < 256; iy++) {
        for (int ix = 0; ix < 256; ix++) {
            // Mitte des Zoom-12-Pixels -> Geokoordinate -> Pixel des Hoehenmodells
            double gx12 = (double)tx * 256 + ix + 0.5, gy12 = (double)ty * 256 + iy + 0.5;
            double lon = gx12 / s12 * 360.0 - 180.0;
            double lat = std::atan(std::sinh(PI_ * (1.0 - 2.0 * gy12 / s12))) * 180.0 / PI_;
            double gx = (lon + 180.0) / 360.0 * sz;
            double gy = latToMy(lat) * sz;
            double fx = gx - 0.5, fy = gy - 0.5;
            long x0 = (long)std::floor(fx), y0 = (long)std::floor(fy);
            float tx_ = (float)(fx - x0), ty_ = (float)(fy - y0);
            float a, b, c, d;
            if (!terrainAt(t, x0, y0, a) || !terrainAt(t, x0 + 1, y0, b) || !terrainAt(t, x0, y0 + 1, c) || !terrainAt(t, x0 + 1, y0 + 1, d)) continue;
            heights[iy * 256 + ix] = (a * (1 - tx_) + b * tx_) * (1 - ty_) + (c * (1 - tx_) + d * tx_) * ty_;
        }
    }
}
} // namespace mapformat
