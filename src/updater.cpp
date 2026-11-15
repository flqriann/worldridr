// Update-Rueckfrage: vergleicht den lokalen Stand mit dem Hauptzweig auf GitHub und fragt, ob heruntergeladen werden soll.
// Mit .git-Ordner: "git pull --ff-only" (ueberschreibt nie lokale Aenderungen). Ohne Git: ZIP nach update/ laden.
// Abschalten: Umgebungsvariable WR_UPDATE=0. Anderes Repository: WR_UPDATE_REPO=besitzer/name, Zweig: WR_UPDATE_BRANCH.
#include "updater.h"
#include "common.h"
#include "net.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <windows.h>

namespace updater {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

enum State { Idle = 0, Available = 1, Working = 2, Done = 3 };
std::atomic<int> state{Idle};
std::mutex mtx;
std::string remoteSha, remoteMsg, resultMsg, dismissedSha;
int aheadBy = 0;
bool resultOk = true;

std::string repo() { const char* e = std::getenv("WR_UPDATE_REPO"); return e && *e ? e : "flqriann/worldridr"; }
std::string branch() { const char* e = std::getenv("WR_UPDATE_BRANCH"); return e && *e ? e : "main"; }

// Programm ohne sichtbares Fenster starten und Ausgabe einsammeln.
bool runCmd(const std::string& cmdline, std::string& out, DWORD* exitCode = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdline;
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return true;
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
std::string slurp(const char* path) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return "";
    return trim(std::string(d.begin(), d.end()));
}
void spit(const char* path, const std::string& s) { writeFile(path, std::vector<uint8_t>(s.begin(), s.end())); }

bool hasGit() { return fs::exists(".git"); }

std::string localSha() {
    if (hasGit()) {
        std::string o;
        DWORD code = 1;
        if (runCmd("git rev-parse HEAD", o, &code) && code == 0) return trim(o);
    }
    return slurp("cache/installed_commit.txt");
}

bool apiGet(const std::string& path, json& out, int& status) {
    std::vector<uint8_t> data;
    status = 0;
    if (!httpRequest("https://api.github.com/repos/" + repo() + path, data, status) || status != 200) return false;
    out = json::parse(data.begin(), data.end(), nullptr, false);
    return !out.is_discarded();
}

void check() {
    if (state != Idle) return;
    json head;
    int st = 0;
    if (!apiGet("/commits/" + branch(), head, st)) { logMsg("Update-Pruefung: GitHub nicht erreichbar (HTTP %d)", st); return; }
    std::string rsha = head.value("sha", "");
    if (rsha.empty()) return;
    std::string lsha = localSha();
    fs::create_directories("cache");
    if (lsha.empty()) { spit("cache/installed_commit.txt", rsha); logMsg("Update-Pruefung: Ausgangsstand %s gemerkt", rsha.substr(0, 7).c_str()); return; }
    if (lsha == rsha || rsha == dismissedSha || rsha == slurp("cache/update_skipped.txt")) return;
    json cmp;
    int ahead = 0;
    if (!apiGet("/compare/" + lsha + "..." + rsha, cmp, st)) { logMsg("Update-Pruefung: lokaler Stand %s ist GitHub unbekannt (nicht hochgeladen?)", lsha.substr(0, 7).c_str()); return; }
    std::string status = cmp.value("status", "");
    if (status != "ahead" && status != "diverged") return; // identisch oder lokal weiter als GitHub
    ahead = cmp.value("ahead_by", 1);
    std::string msg = head.contains("commit") ? head["commit"].value("message", "") : "";
    msg = msg.substr(0, msg.find('\n'));
    {
        std::lock_guard<std::mutex> lk(mtx);
        remoteSha = rsha;
        remoteMsg = msg;
        aheadBy = ahead;
    }
    logMsg("Neue Version auf GitHub: %s (%d Aenderungen) - %s", rsha.substr(0, 7).c_str(), ahead, msg.c_str());
    state = Available;
}

void download() {
    std::string sha, out;
    { std::lock_guard<std::mutex> lk(mtx); sha = remoteSha; }
    bool ok = false;
    std::string msg;
    if (hasGit()) {
        std::string before = localSha(), o;
        DWORD code = 1;
        runCmd("git pull --ff-only", o, &code);
        ok = code == 0;
        if (ok) {
            std::string diff;
            runCmd("git diff --name-only " + before + " HEAD", diff);
            bool code_changed = diff.find("src/") != std::string::npos || diff.find("CMakeLists.txt") != std::string::npos;
            msg = code_changed ? "Aktualisiert. Programmcode hat sich geaendert: build.bat ausfuehren und neu starten."
                               : "Aktualisiert (Karten/Assets/Skripte). Beim naechsten Start aktiv.";
        } else {
            msg = "Automatisches Update nicht moeglich (lokale Aenderungen im Weg?). Bitte in einem Terminal 'git pull' ausfuehren.";
            logMsg("git pull: %s", trim(o).c_str());
        }
    } else {
        std::vector<uint8_t> zip;
        int st = 0;
        if (httpRequest("https://github.com/" + repo() + "/archive/refs/heads/" + branch() + ".zip", zip, st) && st == 200) {
            fs::create_directories("update");
            writeFile("update/worldridr-" + branch() + ".zip", zip);
            spit("cache/installed_commit.txt", sha);
            ok = true;
            msg = "Heruntergeladen: update\\worldridr-" + branch() + ".zip - entpacken und Ordner ersetzen.";
        } else msg = "Download fehlgeschlagen (HTTP " + std::to_string(st) + ").";
    }
    { std::lock_guard<std::mutex> lk(mtx); resultMsg = msg; resultOk = ok; }
    state = Done;
}

void loop() {
    std::this_thread::sleep_for(std::chrono::seconds(8)); // Start nicht ausbremsen
    for (;;) {
        try { check(); } catch (...) {}
        std::this_thread::sleep_for(std::chrono::minutes(10));
    }
}
} // namespace

void start() {
    const char* off = std::getenv("WR_UPDATE");
    if (off && std::string(off) == "0") return;
    std::thread(loop).detach();
}

void draw(Ui& ui, bool allowed) {
    int s = state;
    if (s == Idle || !allowed) return;
    std::string sha, msg, res;
    int ahead;
    bool ok;
    { std::lock_guard<std::mutex> lk(mtx); sha = remoteSha; msg = remoteMsg; res = resultMsg; ahead = aheadBy; ok = resultOk; }
    float w = 460, x = (ui.W - w) / 2, y = 70;
    if (s == Available) {
        ui.panel(x, y, w, 190, Color{14, 17, 23, 235}, 14);
        ui.label(x + 18, y + 28, 15, col::accent, "Neue Version auf GitHub");
        ui.label(x + 18, y + 54, 14, col::white, std::to_string(ahead) + (ahead == 1 ? " neue Aenderung" : " neue Aenderungen") + " (" + sha.substr(0, 7) + ")");
        int line = 0;
        for (const std::string& l : wrapText(msg, 13, w - 36)) { if (line++ >= 2) break; ui.label(x + 18, y + 76 + 18 * (line - 1), 13, col::gray, l); }
        ui.label(x + 18, y + 118, 13, col::gray, "Jetzt herunterladen?");
        if (ui.button(x + 18, y + 134, 150, 38, "Herunterladen", false, 16)) {
            state = Working;
            std::thread(download).detach();
        }
        if (ui.button(x + 178, y + 134, 110, 38, "Spaeter", false, 16)) { dismissedSha = sha; state = Idle; }
        if (ui.button(x + 298, y + 134, 144, 38, "Ueberspringen", false, 15)) { spit("cache/update_skipped.txt", sha); state = Idle; }
    } else if (s == Working) {
        ui.panel(x, y, w, 70, Color{14, 17, 23, 235}, 14);
        ui.label(x + 18, y + 40, 16, col::white, "Lade Update von GitHub ...");
    } else if (s == Done) {
        ui.panel(x, y, w, 130, Color{14, 17, 23, 235}, 14);
        ui.label(x + 18, y + 28, 15, ok ? col::green : col::red, ok ? "Update" : "Update fehlgeschlagen");
        int line = 0;
        for (const std::string& l : wrapText(res, 13, w - 36)) { if (line++ >= 3) break; ui.label(x + 18, y + 52 + 18 * (line - 1), 13, col::white, l); }
        if (ui.button(x + 18, y + 84, 100, 34, "OK", false, 16)) state = Idle;
    }
}
} // namespace updater
