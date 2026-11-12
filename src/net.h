#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Blockierender HTTP(S)-Abruf ueber WinHTTP. body != leer => POST (JSON).
bool httpRequest(const std::string& url, std::vector<uint8_t>& out, int& status, const std::string& body = "");
// Abruf mit Datei-Cache (cachePath leer => kein Cache).
bool cachedGet(const std::string& url, const std::string& cachePath, std::vector<uint8_t>& out);
bool readFile(const std::string& path, std::vector<uint8_t>& out);
void writeFile(const std::string& path, const std::vector<uint8_t>& data);
std::string urlEncode(const std::string& s);
