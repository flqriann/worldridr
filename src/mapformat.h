#pragma once
// Git-freundliches Kartenformat "worldridr-map" (Ordner mit worldridr-map.json und tiles/*.json), gemeinsam mit World Studio.
// Das Spiel arbeitet intern weiter mit dem alten JSON ({objects, roads, palette}); dieses Modul wandelt in beide Richtungen um.
#include <nlohmann/json.hpp>
#include <string>

namespace mapformat {
bool exists(const std::string& dir);                          // dir/worldridr-map.json vorhanden?
bool load(const std::string& dir, nlohmann::json& legacy);    // Kacheln lesen -> altes Format
bool save(const std::string& dir, const nlohmann::json& legacy); // altes Format -> Kacheln (nur geaenderte Dateien werden geschrieben)
std::string newId();
// Hoehenmodell aus <Kartenordner>/terrain.json (World Studio): ueberschreibt die Terrarium-Hoehen einer Zoom-12-Kachel (65536 Werte in Metern)
void applyTerrain(int tx, int ty, float* heights);                                          // 16 Hex-Zeichen, zufaellig
}
