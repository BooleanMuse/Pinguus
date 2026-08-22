// ============================================================================
// Tailscale.h — Find this machine on its tailnet, and list the other devices.
//
// WHY
//   Every phone route in Pinguus used to assume "the phone is on the same
//   Wi-Fi": the APK server, the UDP link and the MJPEG scanner all speak to a
//   192.168.x address. That has three problems the user hits in practice:
//     1. it only works in one room — you cannot film from another city;
//     2. it means showing (and typing) the PC's local IP, which we would
//        rather not paint across the screen while recording or streaming;
//     3. guest Wi-Fi, hotspots and "client isolation" break it silently.
//
//   Tailscale solves all three: both devices join the same private network
//   (a "tailnet"), each gets a stable 100.x.y.z address that only that network
//   can reach, and the traffic is end-to-end encrypted and NAT-punched. The
//   phone can then be anywhere on Earth and everything else in Pinguus keeps
//   working unchanged, because the sockets already bind to every interface.
//
// WHAT THIS CLASS DOES
//   Nothing but READ state. It never runs `tailscale up`, never logs anybody
//   in, never changes the machine's networking — those are the user's calls,
//   made in Tailscale's own UI. Here we only answer:
//     * is Tailscale installed, and is this machine logged in?   requirements()
//     * what is our tailnet address?                             selfIp()
//     * which other devices are in the tailnet, and are they up? peers()
//   The panel turns that into "connect to this phone", the same way the LAN
//   scanner turns a /24 sweep into a list.
//
// HOW IT FINDS THINGS
//   The address comes from the interfaces themselves (getifaddrs / getaddrinfo)
//   by looking for the CGNAT range 100.64.0.0/10 that Tailscale hands out — no
//   subprocess, instant, and it works even when the CLI is somewhere odd. The
//   peer list does need the CLI (`tailscale status`), so it runs on a worker
//   thread through RunCommandRead (CREATE_NO_WINDOW on Windows, so no console
//   flashes over the app).
// ============================================================================
#ifndef PINGUUS_TAILSCALE_H
#define PINGUUS_TAILSCALE_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "PlatformProc.h"   // RunCommandRead: sin ventana de consola en Windows

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <unistd.h>
#endif

class Tailscale {
public:
    struct Peer {
        std::string ip;     // 100.x.y.z
        std::string name;   // el nombre del dispositivo en el tailnet
        std::string os;     // android / ios / linux / windows / macOS
        bool online = false;
    };
    struct Requirement {
        std::string what;   // qué falta, en una línea
        std::string fix;    // el comando/URL exacto que lo arregla
        bool ok = false;
        bool isLink = false; // fix es una URL para abrir, no un comando
    };

    ~Tailscale() { join(); }

    // Una IP del rango CGNAT 100.64.0.0/10 = una dirección de tailnet.
    static bool isTailnetIp(const std::string& ip) {
        int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
        return a == 100 && b >= 64 && b <= 127;
    }

    // Dirección de tailnet de ESTA máquina leída de las interfaces (sin lanzar
    // procesos). Vacía si Tailscale no está levantado.
    static std::string localTailnetIp() {
#if defined(_WIN32)
        char host[256];
        if (gethostname(host, sizeof(host)) != 0) return std::string();
        addrinfo hints{}; hints.ai_family = AF_INET; addrinfo* res = nullptr;
        std::string found;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                char buf[64];
                sockaddr_in* s = (sockaddr_in*)p->ai_addr;
                inet_ntop(AF_INET, &s->sin_addr, buf, sizeof(buf));
                if (isTailnetIp(buf)) { found = buf; break; }
            }
            freeaddrinfo(res);
        }
        return found;
#else
        std::string found;
        ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) == 0) {
            for (ifaddrs* p = ifap; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                char buf[64];
                sockaddr_in* s = (sockaddr_in*)p->ifa_addr;
                inet_ntop(AF_INET, &s->sin_addr, buf, sizeof(buf));
                if (isTailnetIp(buf)) { found = buf; break; }
            }
            freeifaddrs(ifap);
        }
        return found;
#endif
    }

    // Ruta del ejecutable de Tailscale, entrecomillada y lista para el shell, o
    // vacía si no está instalado. Se busca una vez y se recuerda.
    static const std::string& binary() {
        static std::string path;
        static bool done = false;
        if (done) return path;
        done = true;
        static const char* kCandidates[] = {
#if defined(_WIN32)
            "tailscale",
            "C:\\Program Files\\Tailscale\\tailscale.exe",
            "C:\\Program Files (x86)\\Tailscale\\tailscale.exe",
#elif defined(__APPLE__)
            "tailscale",
            "/Applications/Tailscale.app/Contents/MacOS/Tailscale",
            "/usr/local/bin/tailscale",
            "/opt/homebrew/bin/tailscale",
#else
            "tailscale",
            "/usr/bin/tailscale",
            "/usr/local/bin/tailscale",
#endif
        };
        // El error de "no existe ese programa" se tira a la basura: probar
        // candidatos es NORMAL aquí, y sin esto la terminal de Pinguus se llena
        // de "command not found" en cualquier máquina sin Tailscale.
#if defined(_WIN32)
        const char* kQuiet = " 2>nul";
#else
        const char* kQuiet = " 2>/dev/null";
#endif
        for (const char* c : kCandidates) {
            std::string quoted = std::string("\"") + c + "\"";
            std::string out;
            // --version es la comprobación más barata que existe y no toca nada.
            if (RunCommandRead((quoted + " --version" + kQuiet).c_str(), out) == 0 && !out.empty()) {
                path = quoted;
                break;
            }
        }
        return path;
    }

    static bool installed() { return !binary().empty(); }

    // ------------------------------------------------------------------
    // Estado (se refresca en un hilo: `tailscale status` tarda unos ms y esto
    // se dibuja dentro del bucle de la interfaz).
    // ------------------------------------------------------------------
    void refreshAsync() {
        if (busy_) return;
        join();
        busy_ = true;
        worker_ = std::thread([this] {
            std::string ip = localTailnetIp();
            std::vector<Peer> found;
            std::string err;
            if (!binary().empty()) {
                std::string out;
                RunCommandRead((binary() + " status").c_str(), out);
                parseStatus(out, ip, found, err);
            } else {
                err = "not installed";
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                selfIp_ = ip;
                peers_.swap(found);
                error_ = err;
                haveData_ = true;
            }
            busy_ = false;
        });
    }
    void join() { if (worker_.joinable()) worker_.join(); }
    bool refreshing() const { return busy_; }
    bool haveData() const { std::lock_guard<std::mutex> lk(mtx_); return haveData_; }

    std::string selfIp() const { std::lock_guard<std::mutex> lk(mtx_); return selfIp_; }
    // Conectado al tailnet = tenemos dirección 100.x en una interfaz viva.
    bool connected() const { std::lock_guard<std::mutex> lk(mtx_); return !selfIp_.empty(); }
    std::vector<Peer> peers() const { std::lock_guard<std::mutex> lk(mtx_); return peers_; }
    std::string error() const { std::lock_guard<std::mutex> lk(mtx_); return error_; }

    // Los dispositivos que probablemente lleven una cámara encima, primero.
    std::vector<Peer> cameraCandidates() const {
        std::vector<Peer> all = peers(), out;
        for (const Peer& p : all) if (p.online && isPhone(p.os)) out.push_back(p);
        for (const Peer& p : all) if (p.online && !isPhone(p.os)) out.push_back(p);
        for (const Peer& p : all) if (!p.online) out.push_back(p);
        return out;
    }
    static bool isPhone(const std::string& os) {
        return os == "android" || os == "iOS" || os == "ios" || os == "iPadOS";
    }

    // ------------------------------------------------------------------
    // Requisitos: qué falta y el comando EXACTO de ESTA plataforma que lo
    // arregla (para el botón "Copy").
    //
    // Esto es BARATO (no lanza procesos: binary() está cacheado y el resto son
    // lecturas del estado ya leído), así que el panel lo recalcula en cada
    // fotograma. La alternativa —recalcularlo desde el hilo que consulta el
    // tailnet— sería una carrera de datos, porque quien dibuja recorre este
    // mismo vector. Llámalo SÓLO desde el hilo principal.
    // ------------------------------------------------------------------
    const std::vector<Requirement>& requirements(bool recheck = false) {
        if (!recheck && !reqs_.empty()) return reqs_;
        reqs_.clear();

        Requirement r1;
        r1.what = "Tailscale installed on this computer";
#if defined(_WIN32)
        r1.fix = "https://tailscale.com/download/windows";
        r1.isLink = true;
#elif defined(__APPLE__)
        r1.fix = "https://tailscale.com/download/mac";
        r1.isLink = true;
#else
        r1.fix = fileExists("/etc/debian_version")
                     ? "curl -fsSL https://tailscale.com/install.sh | sh"
                     : "sudo pacman -S tailscale && sudo systemctl enable --now tailscaled";
#endif
        r1.ok = installed();
        reqs_.push_back(r1);

        Requirement r2;
        r2.what = "this computer logged in to your tailnet";
        r2.fix = "sudo tailscale up";
#if defined(_WIN32) || defined(__APPLE__)
        r2.fix = "open Tailscale and sign in";
#endif
        r2.ok = connected();
        reqs_.push_back(r2);

        Requirement r3;
        r3.what = "the phone in the SAME tailnet (same account)";
        r3.fix = "https://tailscale.com/download";
        r3.isLink = true;
        r3.ok = false;
        for (const Peer& p : peers()) if (p.online) { r3.ok = true; break; }
        reqs_.push_back(r3);

        return reqs_;
    }
    bool allRequirementsMet() {
        const std::vector<Requirement>& rs = requirements();
        for (size_t i = 0; i < rs.size(); i++) if (!rs[i].ok) return false;
        return !rs.empty();
    }

private:
    static bool fileExists(const char* p) {
        FILE* f = fopen(p, "r");
        if (!f) return false;
        fclose(f);
        return true;
    }

    // `tailscale status` escribe una línea por dispositivo:
    //   100.98.252.123  vironlap    user@   linux    -
    //   100.89.93.115   0-komputer  user@   windows  offline, last seen 15d ago
    // La primera es esta máquina. Basta con partir por espacios: los cuatro
    // primeros campos son fijos y el resto es el estado en prosa.
    static void parseStatus(const std::string& out, const std::string& selfIp,
                            std::vector<Peer>& peers, std::string& err) {
        size_t pos = 0;
        bool any = false;
        while (pos <= out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? out.size() + 1 : nl + 1;
            if (line.empty()) continue;

            std::vector<std::string> f;
            size_t i = 0;
            while (i < line.size() && f.size() < 4) {
                while (i < line.size() && isspace((unsigned char)line[i])) i++;
                size_t s = i;
                while (i < line.size() && !isspace((unsigned char)line[i])) i++;
                if (i > s) f.push_back(line.substr(s, i - s));
            }
            const size_t restAt = i;   // lo que sigue al 4º campo = el estado
            if (f.size() < 4 || !isTailnetIp(f[0])) {
                // Mensajes tipo "Logged out." / "Tailscale is stopped." — se
                // guardan para poder enseñar ALGO cuando no hay dispositivos.
                if (err.empty() && line.find("Logged out") != std::string::npos) err = "logged out";
                else if (err.empty() && line.find("stopped") != std::string::npos) err = "stopped";
                continue;
            }
            any = true;
            if (f[0] == selfIp) continue;          // nosotros no somos un peer
            Peer p;
            p.ip = f[0];
            p.name = f[1];
            p.os = f[3];
            std::string rest = restAt < line.size() ? line.substr(restAt) : std::string();
            p.online = rest.find("offline") == std::string::npos;
            peers.push_back(p);
        }
        if (!any && err.empty()) err = "no devices";
    }

    mutable std::mutex mtx_;
    std::string selfIp_;
    std::vector<Peer> peers_;
    std::string error_;
    bool haveData_ = false;
    std::atomic<bool> busy_{false};
    std::thread worker_;
    std::vector<Requirement> reqs_;
};

#endif // PINGUUS_TAILSCALE_H
