// Mehrspieler ueber UDP: ein Spieler hostet, die anderen treten per Adresse bei. Der Host verteilt die Positionen.
// Positionen werden als Geokoordinaten gesendet, damit jeder seinen eigenen lokalen Ursprung behalten kann.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "game.h"
#include <cstring>
#include <random>

namespace {
#pragma pack(push, 1)
struct Packet {
    uint32_t magic;
    uint8_t type; // 1 Zustand, 2 Abschied
    uint32_t id;
    char name[16];
    double lat, lon;
    float y, q[4], steer, speed, wheelRot;
    uint8_t flags;
};
#pragma pack(pop)
const uint32_t MAGIC = 0x57524431; // "WRD1"

struct Peer { sockaddr_in addr; uint32_t id; double last; };
SOCKET sock = INVALID_SOCKET;
bool isHost = false, wsaUp = false;
sockaddr_in hostAddr{};
std::vector<Peer> peers;
std::vector<NetCar> remote;
uint32_t myId = 0;
float sendTimer = 0;
double lastFromHost = -100;
std::string info;
int portUsed = 0;

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
void sendTo(const Packet& p, const sockaddr_in& to) {
    static int errs = 0;
    if (sendto(sock, (const char*)&p, sizeof p, 0, (const sockaddr*)&to, sizeof to) < 0 && errs++ < 3) logMsg("Mehrspieler: Senden fehlgeschlagen (%d)", WSAGetLastError());
}
} // namespace

namespace netplay {

bool host(int port) {
    if (!openSocket(port)) { info = "Port " + std::to_string(port) + " konnte nicht ge\xC3\xB6" "ffnet werden"; return false; }
    isHost = true;
    portUsed = port;
    info = "Host: " + localAddress() + ":" + std::to_string(port);
    logMsg("Mehrspieler: %s", info.c_str());
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
    isHost = false;
    lastFromHost = -100;
    info = "Verbinde mit " + h + ":" + std::to_string(port);
    logMsg("Mehrspieler: %s", info.c_str());
    return true;
}

void stop() {
    if (sock != INVALID_SOCKET) {
        Packet p{};
        p.magic = MAGIC; p.type = 2; p.id = myId;
        if (isHost) for (const Peer& pe : peers) sendTo(p, pe.addr); else sendTo(p, hostAddr);
        closesocket(sock);
    }
    sock = INVALID_SOCKET;
    peers.clear(); remote.clear();
    info.clear();
}

bool active() { return sock != INVALID_SOCKET; }
bool hosting() { return active() && isHost; }
std::vector<NetCar>& cars() { return remote; }

std::string status() {
    if (!active()) return info.empty() ? "Nicht verbunden" : info;
    if (isHost) return info + "  -  " + std::to_string(remote.size()) + " Mitspieler";
    return (simTime - lastFromHost < 3 ? "Verbunden" : info + " ...") + std::string("  -  ") + std::to_string(remote.size()) + " Mitspieler";
}

void update(float dt, const NetCar& mine) {
    if (!active()) return;
    for (;;) { // empfangen
        Packet p;
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(sock, (char*)&p, sizeof p, 0, (sockaddr*)&from, &fl);
        if (n < 0) {
            // WSAECONNRESET meldet nur ein frueheres unzustellbares Paket - weiterlesen statt abbrechen
            static int dbg = 0;
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && dbg++ < 3) logMsg("Mehrspieler: Empfangsfehler %d", err);
            if (err == WSAECONNRESET) continue;
            break;
        }
        if (n != (int)sizeof p) continue;
        if (p.magic != MAGIC || p.id == myId) continue;
        if (isHost) {
            Peer* pe = nullptr;
            for (Peer& q : peers) if (q.addr.sin_addr.s_addr == from.sin_addr.s_addr && q.addr.sin_port == from.sin_port) pe = &q;
            if (!pe && p.type == 1) { peers.push_back({from, p.id, simTime}); pe = &peers.back(); logMsg("Mehrspieler: neuer Mitspieler"); }
            if (pe) { pe->last = simTime; pe->id = p.id; }
            for (const Peer& q : peers) if (&q != pe) sendTo(p, q.addr); // an alle anderen weiterreichen
        } else lastFromHost = simTime;
        auto it = std::find_if(remote.begin(), remote.end(), [&](const NetCar& c) { return c.id == p.id; });
        if (p.type == 2) { if (it != remote.end()) remote.erase(it); continue; }
        if (it == remote.end()) { remote.push_back(NetCar{}); it = remote.end() - 1; it->id = p.id; }
        p.name[15] = 0;
        it->name = p.name;
        it->lat = p.lat; it->lon = p.lon; it->y = p.y;
        it->q = qnorm(Quat{p.q[0], p.q[1], p.q[2], p.q[3]});
        it->steer = p.steer; it->speed = p.speed; it->wheelRot = p.wheelRot; it->flags = p.flags;
        it->lastSeen = simTime;
    }
    sendTimer -= dt;
    if (sendTimer <= 0) { // 20-mal pro Sekunde eigenen Zustand senden
        sendTimer = 0.05f;
        Packet p{};
        p.magic = MAGIC; p.type = 1; p.id = myId;
        strncpy(p.name, mine.name.c_str(), 15);
        p.lat = mine.lat; p.lon = mine.lon; p.y = mine.y;
        p.q[0] = mine.q.w; p.q[1] = mine.q.x; p.q[2] = mine.q.y; p.q[3] = mine.q.z;
        p.steer = mine.steer; p.speed = mine.speed; p.wheelRot = mine.wheelRot; p.flags = mine.flags;
        if (isHost) for (const Peer& pe : peers) sendTo(p, pe.addr); else sendTo(p, hostAddr);
    }
    for (size_t i = 0; i < peers.size();) { if (simTime - peers[i].last > 6) peers.erase(peers.begin() + i); else i++; }
    for (size_t i = 0; i < remote.size();) { if (simTime - remote[i].lastSeen > 5) remote.erase(remote.begin() + i); else i++; }
    // weiche Bewegung der Mitspieler
    for (NetCar& c : remote) {
        Vec3 target;
        world.toLocal(c.lat, c.lon, target.x, target.z);
        target.y = c.y;
        if (!c.placed || length(target - c.pos) > 30) { c.pos = target; c.qs = c.q; c.placed = true; }
        float k = std::min(1.0f, dt * 12.0f);
        Vec3 f = qrot(c.q, {0, 0, -1});
        c.pos += f * (c.speed * dt) + (target - c.pos) * k;
        float sgn = (c.qs.w * c.q.w + c.qs.x * c.q.x + c.qs.y * c.q.y + c.qs.z * c.q.z) < 0 ? -1.0f : 1.0f;
        c.qs = qnorm({c.qs.w + (c.q.w * sgn - c.qs.w) * k, c.qs.x + (c.q.x * sgn - c.qs.x) * k, c.qs.y + (c.q.y * sgn - c.qs.y) * k, c.qs.z + (c.q.z * sgn - c.qs.z) * k});
        c.wheelRot += c.speed / 0.316f * dt;
    }
}

} // namespace netplay
