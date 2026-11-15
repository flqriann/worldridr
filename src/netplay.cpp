// Mehrspieler ueber UDP: ein Spieler hostet (oder ein eigenstaendiger Server laeuft), die anderen treten per Adresse bei.
// Der Host/Server verteilt Positionen und Chatnachrichten. Positionen werden als Geokoordinaten gesendet, damit jeder
// seinen eigenen lokalen Ursprung behalten kann. LAN-Spiele werden per Broadcast gefunden.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "game.h"
#include <cstring>
#include <random>
#include <map>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace {
#pragma pack(push, 1)
struct Hdr {
    uint32_t magic;
    uint8_t type; // 1 Zustand, 2 Abschied, 3 Chat, 4 Suche, 5 Antwort auf Suche, 6 Lebenszeichen des Servers
    uint32_t id;
};
struct Packet {
    Hdr h;
    char name[16];
    double lat, lon;
    float y, q[4], steer, speed, wheelRot;
    uint8_t flags;
    int8_t model; uint8_t kind, tinted, paint[3];
    int8_t headYaw, headPitch;   // Kopfhaltung des Fahrers (VR-Kopf oder Kamera): Gieren +-1,6 rad, Nicken +-0,7 rad in 1/127-Schritten
};
struct NpcPacket {
    Hdr h;
    uint8_t group, count;   // group 0 Fahrzeuge, 1 Fussgaenger
    NpcState s[24];
};
struct EnvPacket {
    Hdr h;
    float hour;
    uint8_t weather, speed;
};
struct MapPart {   // Teil eines Kartenstands (gemeinsames Bauen)
    Hdr h;
    uint32_t ver;
    uint16_t idx, total, len;
    char name[16];
    char data[1100];
};
struct ChatPacket {
    Hdr h;
    char name[16];
    char text[96];
};
struct InfoPacket {
    Hdr h;
    char name[24];
    uint8_t players, maxPlayers;
};
#pragma pack(pop)
const uint32_t MAGIC = 0x57524432; // "WRD2"

struct Peer { sockaddr_in addr; uint32_t id; double last; char name[16]; };
SOCKET sock = INVALID_SOCKET, discSock = INVALID_SOCKET;
bool isHost = false, isServer = false, wsaUp = false;
sockaddr_in hostAddr{};
std::vector<Peer> peers;
std::vector<NetCar> remote;
std::vector<LanServer> lan;
std::vector<netplay::ChatLine> chatLines;
uint32_t myId = 0;
float sendTimer = 0, beatTimer = 0, npcTimer = 0, envTimer = 0;
// Kartenstaende: werden in Teilen verschickt und dreimal wiederholt (UDP kann Pakete verlieren); der Empfaenger setzt sie zusammen
struct OutMap { std::string blob; uint32_t ver; int left; double next; bool all; sockaddr_in to; };
std::vector<OutMap> outMaps;
struct InMap { uint16_t total = 0; int got = 0; std::vector<std::string> parts; std::vector<char> have; double t = 0; std::string name; bool done = false; };
std::map<uint64_t, InMap> inMaps;
void queueMap(const std::string& blob, bool all, const sockaddr_in* to) {
    if (blob.empty() || blob.size() > 6 * 1024 * 1024) return;
    OutMap o;
    o.blob = blob; o.ver = (uint32_t)(simTime * 1000.0) ^ myId; o.left = 3; o.next = 0; o.all = all;
    if (to) o.to = *to;
    outMaps.push_back(o);
}
void pumpMaps() {
    for (size_t i = 0; i < outMaps.size();) {
        OutMap& o = outMaps[i];
        if (simTime < o.next) { i++; continue; }
        size_t total = (o.blob.size() + 1099) / 1100;
        for (size_t k = 0; k < total; k++) {
            MapPart mp{};
            mp.h = {MAGIC, 9, myId};
            mp.ver = o.ver; mp.idx = (uint16_t)k; mp.total = (uint16_t)total;
            size_t len = std::min<size_t>(1100, o.blob.size() - k * 1100);
            mp.len = (uint16_t)len;
            strncpy(mp.name, cfg.playerName.c_str(), 15);
            memcpy(mp.data, o.blob.data() + k * 1100, len);
            int bytes = (int)offsetof(MapPart, data) + (int)len;
            if (!o.all) sendto(sock, (const char*)&mp, bytes, 0, (const sockaddr*)&o.to, sizeof o.to);
            else if (isHost) for (const Peer& pe : peers) sendto(sock, (const char*)&mp, bytes, 0, (const sockaddr*)&pe.addr, sizeof pe.addr);
            else sendto(sock, (const char*)&mp, bytes, 0, (const sockaddr*)&hostAddr, sizeof hostAddr);
        }
        if (--o.left <= 0) outMaps.erase(outMaps.begin() + i);
        else { o.next = simTime + 1.2; i++; }
    }
}
double lastFromHost = -100, discStart = -100;
std::string info, serverName = "worldridr";
int portUsed = 0, maxPlayers = 16;
double wallNow = 0;

bool openSocket(int bindPort) {
    if (!wsaUp) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false; wsaUp = true; }
    netplay::stop();
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)bindPort);
    if (bind(sock, (sockaddr*)&a, sizeof a) != 0) { closesocket(sock); sock = INVALID_SOCKET; return false; }
    std::random_device rd;
    myId = rd() | 1u;
    return true;
}
std::string localAddress() {
    char name[256];
    if (gethostname(name, sizeof name) != 0) return "?";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) return "?";
    std::string best;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char buf[64];
        inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, buf, sizeof buf);
        std::string s = buf;
        if (best.empty() || s.rfind("192.168.", 0) == 0 || s.rfind("10.", 0) == 0) best = s;
    }
    freeaddrinfo(res);
    return best;
}
template <class T> void sendTo(const T& p, const sockaddr_in& to) {
    static int errs = 0;
    if (sendto(sock, (const char*)&p, sizeof p, 0, (const sockaddr*)&to, sizeof to) < 0 && errs++ < 3) logMsg("Mehrspieler: Senden fehlgeschlagen (%d)", WSAGetLastError());
}
template <class T> void sendAll(const T& p, const Peer* except) {
    for (const Peer& pe : peers) if (&pe != except) sendTo(p, pe.addr);
}
void say(const std::string& name, const std::string& text) {
    chatLines.push_back({name, text, simTime});
    if (chatLines.size() > 40) chatLines.erase(chatLines.begin());
    if (isServer) logMsg("[Chat] %s: %s", name.c_str(), text.c_str());
}
void sendChatPacket(const std::string& name, const std::string& text, const Peer* except) {
    ChatPacket c{};
    c.h = {MAGIC, 3, myId};
    strncpy(c.name, name.c_str(), 15);
    strncpy(c.text, text.c_str(), 95);
    if (isHost) sendAll(c, except); else sendTo(c, hostAddr);
}
std::string addrString(const sockaddr_in& a) {
    char buf[64];
    inet_ntop(AF_INET, (void*)&a.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}

void receive() {
    for (;;) {
        char buf[1500];
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 0) {
            // WSAECONNRESET meldet nur ein frueheres unzustellbares Paket - weiterlesen statt abbrechen
            static int dbg = 0;
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && dbg++ < 3) logMsg("Mehrspieler: Empfangsfehler %d", err);
            if (err == WSAECONNRESET) continue;
            break;
        }
        if (n < (int)sizeof(Hdr)) continue;
        Hdr h;
        memcpy(&h, buf, sizeof h);
        if (h.magic != MAGIC || h.id == myId) continue;
        if (h.type == 4 && isHost) { // Suche im LAN: mit Namen und Spielerzahl antworten
            InfoPacket ip{};
            ip.h = {MAGIC, 5, myId};
            strncpy(ip.name, serverName.c_str(), 23);
            ip.players = (uint8_t)(peers.size() + (isServer ? 0 : 1));
            ip.maxPlayers = (uint8_t)maxPlayers;
            sendTo(ip, from);
            continue;
        }
        if (h.type == 6) { lastFromHost = simTime; continue; }
        if (h.type == 9 && n >= (int)offsetof(MapPart, data)) { // gemeinsames Bauen: Teil eines Kartenstands
            MapPart mp{};
            memcpy(&mp, buf, std::min(n, (int)sizeof mp));
            if (mp.total == 0 || mp.idx >= mp.total || mp.len > 1100 || n < (int)offsetof(MapPart, data) + mp.len) continue;
            if (isHost) { // an alle anderen weiterreichen
                Peer* src = nullptr;
                for (Peer& q : peers) if (q.addr.sin_addr.s_addr == from.sin_addr.s_addr && q.addr.sin_port == from.sin_port) src = &q;
                for (const Peer& pe : peers) if (&pe != src) sendto(sock, buf, n, 0, (const sockaddr*)&pe.addr, sizeof pe.addr);
            }
            uint64_t key = ((uint64_t)h.id << 32) | mp.ver;
            InMap& im = inMaps[key];
            if (im.done) continue;
            if (im.total == 0) { im.total = mp.total; im.parts.assign(mp.total, ""); im.have.assign(mp.total, 0); mp.name[15] = 0; im.name = mp.name; }
            im.t = simTime;
            if (mp.total != im.total || im.have[mp.idx]) continue;
            im.parts[mp.idx].assign(mp.data, mp.len); im.have[mp.idx] = 1; im.got++;
            if (im.got == im.total) {
                std::string blob;
                for (const std::string& part : im.parts) blob += part;
                im.done = true; im.parts.clear();
                editor::applyRemote(blob, im.name);
                logMsg("Mehrspieler: Kartenstand von %s uebernommen (%d Bytes)", im.name.c_str(), (int)blob.size());
            }
            continue;
        }
        if (h.type == 8) { // Tageszeit und Wetter vom Host
            if (!isHost && n == (int)sizeof(EnvPacket)) { EnvPacket ep; memcpy(&ep, buf, sizeof ep); lastFromHost = simTime; envir::setRemote(ep.hour, ep.weather, ep.speed); }
            continue;
        }
        if (h.type == 7) { // Verkehr/Fussgaenger vom Host
            if (!isHost && n >= (int)offsetof(NpcPacket, s)) {
                lastFromHost = simTime;
                NpcPacket np{};
                memcpy(&np, buf, std::min(n, (int)sizeof np));
                int cnt = std::min<int>(np.count, 24);
                if (n >= (int)offsetof(NpcPacket, s) + cnt * (int)sizeof(NpcState)) { if (np.group == 0) traffic::applyRemote(np.s, cnt); else ped::applyRemote(np.s, cnt); }
            }
            continue;
        }
        Peer* pe = nullptr;
        if (isHost) {
            for (Peer& q : peers) if (q.addr.sin_addr.s_addr == from.sin_addr.s_addr && q.addr.sin_port == from.sin_port) pe = &q;
        } else lastFromHost = simTime;
        if (n == (int)sizeof(Packet) && (h.type == 1 || h.type == 2)) {
            Packet p;
            memcpy(&p, buf, sizeof p);
            if (isHost) {
                if (!pe && p.h.type == 1) {
                    if ((int)peers.size() >= maxPlayers) continue; // Server voll
                    Peer np{};
                    np.addr = from; np.id = p.h.id; np.last = simTime;
                    p.name[15] = 0;
                    strncpy(np.name, p.name, 15);
                    peers.push_back(np);
                    pe = &peers.back();
                    logMsg("Mehrspieler: %s ist beigetreten (%s)", np.name, addrString(from).c_str());
                    { std::string b = editor::currentBlob(); json jb = json::parse(b, nullptr, false);
                      bool empty = jb.is_object() && jb.value("objects", json::array()).empty() && jb.value("roads", json::array()).empty();
                      if (!empty) queueMap(b, false, &from); }
                    std::string msg = std::string(np.name) + " ist beigetreten";
                    say("Server", msg);
                    ChatPacket c{};
                    c.h = {MAGIC, 3, myId};
                    strncpy(c.name, "Server", 15);
                    strncpy(c.text, msg.c_str(), 95);
                    sendAll(c, nullptr);
                }
                if (pe) { pe->last = simTime; pe->id = p.h.id; }
                if (p.h.type == 2 && pe) {
                    std::string msg = std::string(pe->name) + " hat das Spiel verlassen";
                    say("Server", msg);
                    peers.erase(peers.begin() + (pe - peers.data()));
                    pe = nullptr;
                    ChatPacket c{};
                    c.h = {MAGIC, 3, myId};
                    strncpy(c.name, "Server", 15);
                    strncpy(c.text, msg.c_str(), 95);
                    sendAll(c, nullptr);
                } else if (pe) sendAll(p, pe); // an alle anderen weiterreichen
            }
            auto it = std::find_if(remote.begin(), remote.end(), [&](const NetCar& c) { return c.id == p.h.id; });
            if (p.h.type == 2) { if (it != remote.end()) remote.erase(it); continue; }
            if (it == remote.end()) { remote.push_back(NetCar{}); it = remote.end() - 1; it->id = p.h.id; }
            p.name[15] = 0;
            it->name = p.name;
            it->lat = p.lat; it->lon = p.lon; it->y = p.y;
            it->q = qnorm(Quat{p.q[0], p.q[1], p.q[2], p.q[3]});
            it->steer = p.steer; it->speed = p.speed; it->wheelRot = p.wheelRot; it->flags = p.flags;
            it->model = p.model; it->kind = p.kind; it->tinted = p.tinted != 0;
            for (int i = 0; i < 3; i++) it->paint[i] = p.paint[i];
            it->headYaw = p.headYaw * (1.6f / 127.0f); it->headPitch = p.headPitch * (0.7f / 127.0f);
            it->lastSeen = simTime; it->tRecv = simTime;
        } else if (n == (int)sizeof(ChatPacket) && h.type == 3) {
            ChatPacket c;
            memcpy(&c, buf, sizeof c);
            c.name[15] = 0; c.text[95] = 0;
            if (isHost && pe) sendAll(c, pe);
            say(c.name, c.text);
        }
    }
}

void purge() {
    for (size_t i = 0; i < peers.size();) {
        if (simTime - peers[i].last > 6) {
            say("Server", std::string(peers[i].name) + " hat die Verbindung verloren");
            peers.erase(peers.begin() + i);
        } else i++;
    }
    for (size_t i = 0; i < remote.size();) { if (simTime - remote[i].lastSeen > 5) remote.erase(remote.begin() + i); else i++; }
}
} // namespace

namespace netplay {

bool host(int port) {
    if (!openSocket(port)) { info = "Port " + std::to_string(port) + " konnte nicht ge\xC3\xB6" "ffnet werden"; return false; }
    isHost = true; isServer = false;
    portUsed = port;
    serverName = cfg.playerName + "s Spiel";
    info = "Host: " + localAddress() + ":" + std::to_string(port);
    logMsg("Mehrspieler: %s", info.c_str());
    return true;
}

bool server(int port, const std::string& name, int maxPl) {
    if (!openSocket(port)) { logMsg("Server: Port %d konnte nicht ge\xC3\xB6" "ffnet werden", port); return false; }
    isHost = true; isServer = true;
    portUsed = port;
    serverName = name.empty() ? "worldridr Server" : name;
    maxPlayers = std::max(1, maxPl);
    info = "Server \"" + serverName + "\" " + localAddress() + ":" + std::to_string(port);
    logMsg("%s (max. %d Spieler)", info.c_str(), maxPlayers);
    return true;
}

bool join(const std::string& address, int port) {
    std::string h = address;
    size_t c = h.rfind(':');
    if (c != std::string::npos) { port = atoi(h.c_str() + c + 1); h = h.substr(0, c); }
    if (!openSocket(0)) { info = "Netzwerk nicht verf\xC3\xBC" "gbar"; return false; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { stop(); info = "Adresse \"" + address + "\" nicht gefunden"; return false; }
    hostAddr = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    isHost = false; isServer = false;
    lastFromHost = -100;
    info = "Verbinde mit " + h + ":" + std::to_string(port);
    logMsg("Mehrspieler: %s", info.c_str());
    return true;
}

void stop() {
    outMaps.clear(); inMaps.clear();
    if (sock != INVALID_SOCKET) {
        Packet p{};
        p.h = {MAGIC, 2, myId};
        if (isHost) sendAll(p, nullptr); else sendTo(p, hostAddr);
        closesocket(sock);
    }
    sock = INVALID_SOCKET;
    peers.clear(); remote.clear(); chatLines.clear();
    isHost = isServer = false;
    info.clear();
}

bool active() { return sock != INVALID_SOCKET; }
bool hosting() { return active() && isHost; }
std::vector<NetCar>& cars() { return remote; }
std::vector<ChatLine>& chatLog() { return chatLines; }
int playerCount() { return (int)remote.size() + (isServer ? 0 : 1); }

std::string status() {
    if (!active()) return info.empty() ? "Nicht verbunden" : info;
    if (isHost) return info + "  -  " + std::to_string(remote.size()) + " Mitspieler";
    return (simTime - lastFromHost < 3 ? "Verbunden" : info + " ...") + std::string("  -  ") + std::to_string(remote.size()) + " Mitspieler";
}

void chat(const std::string& text) {
    if (!active() || text.empty()) return;
    say("Ich", text);
    // Name wird aus dem Spielerzustand abgeleitet: der Absender traegt seinen Namen im Paket
    sendChatPacket(cfg.playerName, text, nullptr);
}

void update(float dt, const NetCar& mine) {
    if (!active()) return;
    receive();
    sendTimer -= dt;
    if (sendTimer <= 0) { // 20-mal pro Sekunde eigenen Zustand senden
        sendTimer = 0.05f;
        Packet p{};
        p.h = {MAGIC, 1, myId};
        strncpy(p.name, mine.name.c_str(), 15);
        p.lat = mine.lat; p.lon = mine.lon; p.y = mine.y;
        p.q[0] = mine.q.w; p.q[1] = mine.q.x; p.q[2] = mine.q.y; p.q[3] = mine.q.z;
        p.steer = mine.steer; p.speed = mine.speed; p.wheelRot = mine.wheelRot; p.flags = mine.flags;
        p.model = (int8_t)mine.model; p.kind = (uint8_t)mine.kind; p.tinted = mine.tinted ? 1 : 0;
        for (int i = 0; i < 3; i++) p.paint[i] = mine.paint[i];
        p.headYaw = (int8_t)clampv((int)std::lround(mine.headYaw * (127.0f / 1.6f)), -127, 127);
        p.headPitch = (int8_t)clampv((int)std::lround(mine.headPitch * (127.0f / 0.7f)), -127, 127);
        if (isHost) sendAll(p, nullptr); else sendTo(p, hostAddr);
    }
    purge();
    if (isHost ? !peers.empty() : (simTime - lastFromHost < 5)) { std::string b = editor::syncPoll(simTime); if (!b.empty()) queueMap(b, true, nullptr); }
    pumpMaps();
    for (auto it = inMaps.begin(); it != inMaps.end();) { if (simTime - it->second.t > 20) it = inMaps.erase(it); else ++it; }
    if (isHost && !peers.empty()) { // Tageszeit/Wetter (1 Hz)
        envTimer -= dt;
        if (envTimer <= 0) {
            envTimer = 1.0f;
            EnvPacket ep{};
            ep.h = {MAGIC, 8, myId};
            float hr; int w, sp;
            envir::getState(hr, w, sp);
            ep.hour = hr; ep.weather = (uint8_t)w; ep.speed = (uint8_t)sp;
            sendAll(ep, nullptr);
        }
    }
    if (isHost && !peers.empty()) { // Verkehr und Fussgaenger: der Host simuliert, alle bekommen denselben Zustand (10 Hz)
        npcTimer -= dt;
        if (npcTimer <= 0) {
            npcTimer = 0.1f;
            for (int g = 0; g < 2; g++) {
                std::vector<NpcState> st;
                if (g == 0) traffic::snapshot(st); else ped::snapshot(st);
                for (size_t i = 0; i < st.size(); i += 24) {
                    NpcPacket np{};
                    np.h = {MAGIC, 7, myId};
                    np.group = (uint8_t)g;
                    np.count = (uint8_t)std::min<size_t>(24, st.size() - i);
                    for (int k = 0; k < np.count; k++) np.s[k] = st[i + k];
                    int len = (int)offsetof(NpcPacket, s) + np.count * (int)sizeof(NpcState);
                    for (const Peer& pe : peers) sendto(sock, (const char*)&np, len, 0, (const sockaddr*)&pe.addr, sizeof pe.addr);
                }
            }
        }
    }
    if (isHost) { // Lebenszeichen, damit Beitretende auch ohne weitere Mitspieler "Verbunden" sehen
        beatTimer -= dt;
        if (beatTimer <= 0) { beatTimer = 1.0f; Hdr h{MAGIC, 6, myId}; sendAll(h, nullptr); }
    }
    // weiche Bewegung der Mitspieler
    for (NetCar& c : remote) {
        Vec3 target;
        world.toLocal(c.lat, c.lon, target.x, target.z);
        target.y = c.y;
        if (!c.placed || length(target - c.pos) > 30) { c.pos = target; c.qs = c.q; c.placed = true; }
        // Ziel = letzte bekannte Position, entlang der Fahrtrichtung bis zur Gegenwart fortgeschrieben (Pakete kommen nur mit 20 Hz);
        // das Auto folgt dem Ziel exponentiell weich, ohne zusaetzlich selbst vorzulaufen (sonst entsteht das Ruckeln)
        float k = 1.0f - std::exp(-dt * 14.0f);
        Vec3 f = qrot(c.q, {0, 0, -1});
        target = target + f * (c.speed * (float)std::min(0.30, std::max(0.0, simTime - c.tRecv)));
        c.pos += (target - c.pos) * k;
        float sgn = (c.qs.w * c.q.w + c.qs.x * c.q.x + c.qs.y * c.q.y + c.qs.z * c.q.z) < 0 ? -1.0f : 1.0f;
        c.qs = qnorm({c.qs.w + (c.q.w * sgn - c.qs.w) * k, c.qs.x + (c.q.x * sgn - c.qs.x) * k, c.qs.y + (c.q.y * sgn - c.qs.y) * k, c.qs.z + (c.q.z * sgn - c.qs.z) * k});
        c.wheelRot += c.speed / 0.316f * dt;
        c.hyS += (c.headYaw - c.hyS) * std::min(1.0f, dt * 14.0f);   // Kopf weich nachfuehren (Pakete kommen mit 20 Hz)
        c.hpS += (c.headPitch - c.hpS) * std::min(1.0f, dt * 14.0f);
    }
}

// Eigenstaendiger Server ohne Fenster: nur Verteilen, kein Weltmodell noetig
void serverUpdate(float dt) {
    if (!active()) return;
    receive();
    purge();
    beatTimer -= dt;
    if (beatTimer <= 0) {
        beatTimer = 1.0f;
        Hdr h{MAGIC, 6, myId};
        sendAll(h, nullptr);
    }
}

// ---------- LAN-Suche ----------
void discover() {
    if (!wsaUp) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return; wsaUp = true; }
    if (discSock != INVALID_SOCKET) closesocket(discSock);
    discSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discSock == INVALID_SOCKET) return;
    u_long nb = 1;
    ioctlsocket(discSock, FIONBIO, &nb);
    BOOL bc = TRUE;
    setsockopt(discSock, SOL_SOCKET, SO_BROADCAST, (const char*)&bc, sizeof bc);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(discSock, (sockaddr*)&a, sizeof a);
    lan.clear();
    Hdr h{MAGIC, 4, 0};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short)cfg.port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(discSock, (const char*)&h, sizeof h, 0, (sockaddr*)&to, sizeof to);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr); // gleicher Rechner
    sendto(discSock, (const char*)&h, sizeof h, 0, (sockaddr*)&to, sizeof to);
    discStart = simTime;
}

void pollDiscovery() {
    if (discSock == INVALID_SOCKET) return;
    for (;;) {
        char buf[256];
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(discSock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 0) break;
        if (n != (int)sizeof(InfoPacket)) continue;
        InfoPacket ip;
        memcpy(&ip, buf, sizeof ip);
        if (ip.h.magic != MAGIC || ip.h.type != 5) continue;
        ip.name[23] = 0;
        LanServer s;
        s.name = ip.name;
        s.players = ip.players;
        s.address = addrString(from);
        bool known = false;
        for (LanServer& o : lan) if (o.address == s.address) { o = s; known = true; }
        if (!known) lan.push_back(s);
    }
    if (simTime - discStart > 4) { closesocket(discSock); discSock = INVALID_SOCKET; }
}

std::vector<LanServer> lanServers() { return lan; }

} // namespace netplay
