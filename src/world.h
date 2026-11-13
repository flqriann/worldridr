#pragma once
#include "gfx.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// Lokales Koordinatensystem: x = Osten, y = Hoehe (m ue. M.), z = Sueden. Ursprung = Startpunkt.
struct GeoRef {
    double lat0 = 0, lon0 = 0, mx0 = 0, my0 = 0, S = 1; // S = Meter pro Mercator-Einheit
    int gen = 0;
};

struct RoadSeg {
    float ax, az, bx, bz, hw, maxspeed;
    int8_t oneway;
    uint8_t klass, lanes, marked;
    int name;
    uint8_t ring = 0;   // Kreisverkehr: 1 = Aussenseite rechts der Laufrichtung, 2 = links, 0 = kein Kreisverkehr
};
// Ampel (0), Stoppschild (1), Vorrang geben (2) - jeweils fuer eine Anfahrtsrichtung
struct Control {
    Vec3 pos;
    float dx, dz, hw;
    int type, group;
    float phase;
    bool oneway;
};
struct Poi { // Tankstelle (0) oder Supermarkt (1)
    int kind = 0;
    float x = 0, z = 0, ux = 1, uz = 0, price = 1.6f;
    std::string name;
};
struct TreePos { float x, y, z, rnd; }; // Strassenbaum (wird als Instanz gezeichnet)
struct MapConflict { // Unklarheit in den Kartendaten (z. B. Spurzahl wechselt von 2 auf 3)
    std::string key, name;
    int64_t idA = 0, idB = 0;
    int lanesA = 0, lanesB = 0;
    double lat = 0, lon = 0;
};
struct Building {
    float minx, minz, maxx, maxz;
    std::vector<float> poly;
};
struct RoadHit {
    bool found = false;
    float dist = 1e9f, side = 0, dx = 0, dz = -1, px = 0, pz = 0;
    const RoadSeg* seg = nullptr;
    std::string name;
};

class World {
public:
    void init(const Config& cfg);
    void setOrigin(double lat, double lon);
    void update(Vec3 carPos, double now);
    bool height(float x, float z, float& h);
    Vec3 normal(float x, float z);
    RoadHit nearestRoad(float x, float z, float maxDist, int maxKlass = 7);
    bool insideBuilding(float x, float z);
    void forControls(Vec3 p, float radius, const std::function<void(const Control&)>& fn);
    static int signalState(const Control& c, double t); // 0 rot, 1 rot-gelb, 2 gruen, 3 gelb
    void drawTerrain(int level, const Mat4& vp); // 0 fern, 1 mittel, 2 nah (nur Sichtbares)
    void forPoisNear(float x, float z, float r, const std::function<void(const Poi&)>& fn) const;
    void forTreesNear(float x, float z, float r, const std::function<void(const TreePos&)>& fn) const;
    int forestAt(float x, float z) const;   // 1 dunkle Waldflaeche im Satellitenbild, 0 sonst, -1 Bild noch nicht geladen
    bool osmReady(float x, float z) const;  // Strassen/Gebaeude der Kachel sind geladen
    std::vector<MapConflict> takeConflicts();              // neu erkannte Unklarheiten abholen
    void resolveConflict(const MapConflict& c, int lanes); // lanes > 0: Spurzahl setzen, 0: so lassen (wird gespeichert)
    void setQuality(int q);                       // 0 niedrig, 1 mittel, 2 hoch: Reichweite des Gelaendes
    void drawRoads(const Mat4& vp, Vec3 cam, float maxDist);
    void drawBuildings(const Mat4& vp, Vec3 cam, float maxDist);
    bool ready(Vec3 p, double now);
    void toLatLon(float x, float z, double& lat, double& lon) const;
    void toLocal(double lat, double lon, float& x, float& z) const;
    const GeoRef& geo() const { return g; }
    std::string attribution() const;
    int pendingJobs() const;
    // Selbst gebaute Strassen (Editor) und befahrbare Objekte
    void setCustomRoads(const std::vector<Vertex>& verts, const std::vector<RoadSeg>& segs);
    void drawCustomRoads();
    int roadVersion() const;                          // aendert sich, wenn Strassen nachgeladen/entfernt wurden
    void collectSegs(std::vector<RoadSeg>& out) const; // alle geladenen Strassenabschnitte (fuer den Verkehr)
    // Strassenabschnitte / Gebaeude im Umkreis (fuer Minimap und Navigation)
    void forSegmentsNear(float x, float z, float r, const std::function<void(const RoadSeg&)>& fn) const;
    void forBuildingsNear(float x, float z, float r, const std::function<void(const Building&)>& fn) const;
    std::function<bool(float x, float z, float yRef, float& h)> surfaceHook;
    bool surface(float x, float z, float yRef, float& h); // Gelaende oder hoechste befahrbare Flaeche unter yRef
    Vec3 surfaceNormal(float x, float z, float yRef);
    struct Impl;

private:
    Impl* d = nullptr;
    GeoRef g;
};
