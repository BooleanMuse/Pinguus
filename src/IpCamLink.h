// ============================================================================
// IpCamLink.h — Use a phone as camera+mic through an EXISTING Android/iOS app
// that serves an MJPEG/HTTP stream (the classic one is "IP Webcam", free on the
// Play Store; DroidCam and others work the same way).
//
// WHY: it needs NO Termux, NO pip, NO script on the phone. You install the app,
// press "Start server", and it serves:
//     http://<phone-ip>:8080/video       multipart MJPEG video stream
//     http://<phone-ip>:8080/shot.jpg    a single JPEG frame
//     http://<phone-ip>:8080/audio.wav   streaming PCM WAV
// Pinguus uses /shot.jpg for the LIVE PREVIEW (dead simple: one GET = one JPEG,
// no multipart parsing) and hands /video + /audio.wav straight to ffmpeg when
// RECORDING — so the recorded clip goes through the same tested load path.
//
// It also AUTO-DISCOVERS the phone: scanLan() sweeps the local /24 looking for
// hosts answering on the port, then confirms each one actually returns a JPEG.
// So the user clicks their phone in a list instead of typing an IP.
//
// Pure sockets (POSIX/Winsock), own threads, no raylib/engine access.
// ============================================================================
#ifndef PINGUUS_IPCAMLINK_H
#define PINGUUS_IPCAMLINK_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define PG_CLOSESOCK closesocket
  #define PG_BADSOCK INVALID_SOCKET
  typedef SOCKET pg_sock_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #define PG_CLOSESOCK ::close
  #define PG_BADSOCK (-1)
  typedef int pg_sock_t;
#endif

class IpCamLink {
public:
    IpCamLink() {}
    ~IpCamLink() { stopPreview(); joinScan(); }

    // ---- Configuration -----------------------------------------------------
    void setHost(const std::string& h, int p = 8080) {
        std::lock_guard<std::mutex> lk(mtx_);
        host_ = h; port_ = p;
    }
    std::string host() { std::lock_guard<std::mutex> lk(mtx_); return host_; }
    int port() { std::lock_guard<std::mutex> lk(mtx_); return port_; }
    bool hasHost() { std::lock_guard<std::mutex> lk(mtx_); return !host_.empty(); }

    // Full URLs for ffmpeg (recording) — path names match IP Webcam.
    std::string videoUrl() {
        std::lock_guard<std::mutex> lk(mtx_);
        return host_.empty() ? std::string() : "http://" + host_ + ":" + std::to_string(port_) + "/video";
    }
    std::string audioUrl() {
        std::lock_guard<std::mutex> lk(mtx_);
        return host_.empty() ? std::string() : "http://" + host_ + ":" + std::to_string(port_) + "/audio.wav";
    }

    // ---- Live preview (polls /shot.jpg on its own thread) ------------------
    void startPreview() {
        if (previewOn_) return;
        if (!hasHost()) return;
        previewOn_ = true;
        previewThread_ = std::thread([this] { previewLoop(); });
    }
    void stopPreview() {
        if (!previewOn_) return;
        previewOn_ = false;
        if (previewThread_.joinable()) previewThread_.join();
    }
    bool previewRunning() const { return previewOn_; }
    bool connected() const {
        return previewOn_ && (nowMs() - lastFrameMs_.load()) < 3000;
    }
    // Copy the latest JPEG; `version` lets the caller skip redundant decodes.
    bool latestFrame(std::vector<uint8_t>& out, uint64_t& version) {
        std::lock_guard<std::mutex> lk(frameMtx_);
        if (latestJpeg_.empty()) return false;
        version = frameVersion_;
        out = latestJpeg_;
        return true;
    }

    // ---- LAN auto-discovery ------------------------------------------------
    struct Found { std::string ip; int w = 0, h = 0; };

    // Sweeps every /24 the machine is on, looking for `port` serving a JPEG.
    void scanLan(const std::vector<std::string>& localIps, int port = 8080) {
        if (scanning_) return;
        // Expand each local address into its whole /24 ("192.168.1." + 1..254).
        std::vector<std::string> hosts;
        std::vector<std::string> prefixes;
        for (const std::string& ip : localIps) {
            size_t dot = ip.rfind('.');
            if (dot == std::string::npos) continue;
            std::string pre = ip.substr(0, dot + 1);
            bool dup = false;
            for (auto& q : prefixes) if (q == pre) { dup = true; break; }
            if (dup) continue;
            prefixes.push_back(pre);
            for (int i = 1; i <= 254; i++) hosts.push_back(pre + std::to_string(i));
        }
        scanHosts(hosts, port);
    }

    // Probe an EXPLICIT list of addresses instead of sweeping a subnet. This is
    // how the TAILSCALE route works: the tailnet already knows exactly which
    // devices exist and where, so there is nothing to sweep — we ask those few
    // addresses directly, which also means the phone can be in another country.
    void scanHosts(const std::vector<std::string>& hosts, int port = 8080) {
        if (scanning_) return;
        joinScan();
        scanning_ = true;
        scanProgress_ = 0;
        { std::lock_guard<std::mutex> lk(foundMtx_); found_.clear(); }
        scanThread_ = std::thread([this, hosts, port] { scanLoop(hosts, port); });
    }
    bool scanning() const { return scanning_; }
    int  scanProgress() const { return scanProgress_; }   // 0..100
    std::vector<Found> foundDevices() {
        std::lock_guard<std::mutex> lk(foundMtx_);
        return found_;
    }
    void joinScan() { if (scanThread_.joinable()) scanThread_.join(); }

    // ---- One-shot helpers (also used by the scanner) -----------------------
    // GET <path> from host:port. Returns the response BODY, or empty on failure.
    static std::vector<uint8_t> httpGet(const std::string& host, int port,
                                        const char* path, int timeoutMs, size_t maxBytes = 4u << 20) {
        std::vector<uint8_t> body;
        pg_sock_t s = connectTimeout(host, port, timeoutMs);
        if (s == PG_BADSOCK) return body;
        char req[512];
        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Pinguus\r\nConnection: close\r\n\r\n",
                 path, host.c_str());
        if (sendAll(s, req, (int)strlen(req)) <= 0) { PG_CLOSESOCK(s); return body; }
        setRecvTimeout(s, timeoutMs);

        std::vector<uint8_t> raw;
        char buf[8192];
        for (;;) {
            int n = (int)recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.insert(raw.end(), buf, buf + n);
            if (raw.size() > maxBytes) break;
        }
        PG_CLOSESOCK(s);
        // Split headers/body at the blank line.
        static const char* sep = "\r\n\r\n";
        for (size_t i = 0; i + 3 < raw.size(); i++) {
            if (memcmp(&raw[i], sep, 4) == 0) {
                body.assign(raw.begin() + i + 4, raw.end());
                break;
            }
        }
        return body;
    }
    // A JPEG starts with FF D8 FF and ends with FF D9.
    static bool looksLikeJpeg(const std::vector<uint8_t>& b) {
        return b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
    }

private:
    static uint64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    static void setRecvTimeout(pg_sock_t s, int ms) {
#if defined(_WIN32)
        DWORD tv = (DWORD)ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    static int sendAll(pg_sock_t s, const char* p, int n) {
        int sent = 0;
        while (sent < n) {
            int k = (int)send(s, p + sent, n - sent, 0);
            if (k <= 0) return k;
            sent += k;
        }
        return sent;
    }
    // Non-blocking connect with a timeout (so a dead IP costs `timeoutMs`, not 2 min).
    static pg_sock_t connectTimeout(const std::string& host, int port, int timeoutMs) {
        pg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == PG_BADSOCK) return PG_BADSOCK;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) { PG_CLOSESOCK(s); return PG_BADSOCK; }
        setNonBlocking(s, true);
        int r = ::connect(s, (sockaddr*)&a, sizeof(a));
        if (r != 0) {
#if defined(_WIN32)
            if (WSAGetLastError() != WSAEWOULDBLOCK) { PG_CLOSESOCK(s); return PG_BADSOCK; }
#else
            if (errno != EINPROGRESS) { PG_CLOSESOCK(s); return PG_BADSOCK; }
#endif
            fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
            timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
            if (select((int)s + 1, nullptr, &wf, nullptr, &tv) <= 0) { PG_CLOSESOCK(s); return PG_BADSOCK; }
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
            if (err != 0) { PG_CLOSESOCK(s); return PG_BADSOCK; }
        }
        setNonBlocking(s, false);
        return s;
    }
    static void setNonBlocking(pg_sock_t s, bool nb) {
#if defined(_WIN32)
        u_long m = nb ? 1 : 0; ioctlsocket(s, FIONBIO, &m);
#else
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
    }

    void previewLoop() {
        while (previewOn_) {
            std::string h; int p;
            { std::lock_guard<std::mutex> lk(mtx_); h = host_; p = port_; }
            if (h.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            std::vector<uint8_t> jpg = httpGet(h, p, "/shot.jpg", 1200);
            if (looksLikeJpeg(jpg)) {
                std::lock_guard<std::mutex> lk(frameMtx_);
                latestJpeg_.swap(jpg);
                frameVersion_++;
                lastFrameMs_ = nowMs();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            // ~8 fps is plenty for a preview thumbnail and stays light on the phone.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    }

    void scanLoop(const std::vector<std::string>& hosts, int port) {
        const int totalHosts = (int)hosts.size();
        int done = 0;
        // Probe in batches of concurrent non-blocking connects (fast sweep).
        const int kBatch = 32;
        for (int base = 0; base < totalHosts && scanning_; base += kBatch) {
            std::vector<pg_sock_t> socks;
            std::vector<std::string> ips;
            int hi = base + kBatch; if (hi > totalHosts) hi = totalHosts;
            for (int i = base; i < hi; i++) {
                const std::string& ip = hosts[(size_t)i];
                pg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
                if (s == PG_BADSOCK) continue;
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
                if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { PG_CLOSESOCK(s); continue; }
                setNonBlocking(s, true);
                ::connect(s, (sockaddr*)&a, sizeof(a));
                socks.push_back(s); ips.push_back(ip);
            }
            // Wait once for the whole batch. A tailnet hop can be a continent
            // away, so this is deliberately generous compared to a LAN sweep.
            fd_set wf; FD_ZERO(&wf);
            pg_sock_t maxfd = 0;
            for (pg_sock_t s : socks) { FD_SET(s, &wf); if (s > maxfd) maxfd = s; }
            timeval tv; tv.tv_sec = 0; tv.tv_usec = 700000; // 700 ms per batch
            std::vector<std::string> candidates;
            if (!socks.empty() && select((int)maxfd + 1, nullptr, &wf, nullptr, &tv) > 0) {
                for (size_t k = 0; k < socks.size(); k++) {
                    if (!FD_ISSET(socks[k], &wf)) continue;
                    int err = 0; socklen_t el = sizeof(err);
                    getsockopt(socks[k], SOL_SOCKET, SO_ERROR, (char*)&err, &el);
                    if (err == 0) candidates.push_back(ips[k]);
                }
            }
            for (pg_sock_t s : socks) PG_CLOSESOCK(s);
            done += (int)ips.size();
            scanProgress_ = totalHosts > 0 ? (done * 100 / totalHosts) : 100;

            // Confirm each candidate really serves a JPEG frame.
            for (const std::string& ip : candidates) {
                if (!scanning_) break;
                std::vector<uint8_t> jpg = httpGet(ip, port, "/shot.jpg", 2500);
                if (looksLikeJpeg(jpg)) {
                    Found f; f.ip = ip;
                    jpegSize(jpg, f.w, f.h);
                    std::lock_guard<std::mutex> lk(foundMtx_);
                    found_.push_back(f);
                }
            }
        }
        scanProgress_ = 100;
        scanning_ = false;
    }

    // Reads width/height from a JPEG's SOF marker (nice-to-have for the list).
    static void jpegSize(const std::vector<uint8_t>& b, int& w, int& h) {
        w = h = 0;
        for (size_t i = 2; i + 9 < b.size();) {
            if (b[i] != 0xFF) { i++; continue; }
            uint8_t m = b[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            size_t len = ((size_t)b[i + 2] << 8) | b[i + 3];
            // SOF0..SOF15 except DHT(C4)/JPG(C8)/DAC(CC) carry the dimensions.
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                if (i + 9 < b.size()) {
                    h = ((int)b[i + 5] << 8) | b[i + 6];
                    w = ((int)b[i + 7] << 8) | b[i + 8];
                }
                return;
            }
            i += 2 + len;
        }
    }

    std::mutex mtx_;
    std::string host_;
    int port_ = 8080;

    std::atomic<bool> previewOn_{false};
    std::thread previewThread_;
    std::mutex frameMtx_;
    std::vector<uint8_t> latestJpeg_;
    uint64_t frameVersion_ = 0;
    std::atomic<uint64_t> lastFrameMs_{0};

    std::atomic<bool> scanning_{false};
    std::atomic<int>  scanProgress_{0};
    std::thread scanThread_;
    std::mutex foundMtx_;
    std::vector<Found> found_;
};

#endif // PINGUUS_IPCAMLINK_H
