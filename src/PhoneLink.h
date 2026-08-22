// ============================================================================
// PhoneLink.h — Receive one or MORE phones' live CAMERA (JPEG frames) + MIC
// (PCM audio) over a simple UDP protocol, buffer them, let the main thread show
// a live preview and RECORD a few seconds into a clip/sample slot — and let a
// phone ASK for a recording by itself.
//
// This is the "simple, full-control" path (no browser / HTTPS / QR): the
// Pinguus Cam Android app (android/), or the small Python script in phone/,
// captures the camera and the mic and streams both over UDP to the PC. Pinguus
// listens on one UDP port and reassembles the streams.
//
// WIRE PROTOCOL (little-endian), 16-byte header + payload:
//   [0..3]  magic  = 'P','I','N','G'
//   [4]     type   = 1 video-chunk | 2 audio-chunk | 3 hello
//                  | 5 discover (phone → broadcast) | 6 here-I-am (PC → phone)
//                  | 7 slot state (PC → phone) | 8 record request (phone → PC)
//                  | 9 record status (PC → phone)
//   [5]     reserved
//   [6..7]  chunkIdx   (uint16)  — video: which slice of the JPEG
//   [8..9]  chunkCount (uint16)  — video: how many slices this frame has
//   [10..13] seq       (uint32)  — video: frame number | audio: chunk number
//   [14..15] payloadLen(uint16)  — bytes of payload that follow
// Audio payload = signed 16-bit PCM, MONO, 44100 Hz (matches the bank), so no
// resampling is needed on the PC. Video payload = a slice of a JPEG frame.
// A hello's payload is the phone's NAME (optional; older builds send none, and
// then the device is listed by its address instead).
//
// DISCOVERY (so nobody has to read an IP off one screen and type it into
// another): the phone sends a type-5 packet to the broadcast address on this
// same port; every Pinguus listening on the LAN answers with a type 6 whose
// payload is its computer name. The phone then just lists the answers and the
// user taps one. A discover packet does NOT count as "a phone is streaming",
// so it never turns the connection indicator green on its own.
//
// WHY EVERYTHING IS KEYED BY SENDER (this used to be a real bug)
//   Frames arrive as numbered slices that have to be glued back together, and
//   the reassembly used to be keyed by the frame's sequence number ALONE. Two
//   phones streaming at once both start counting at 0, so their slices landed
//   in the same bucket and were concatenated into a single corrupt JPEG — the
//   preview tore and any recording was ruined. There was no way to "pick the
//   right camera" either, because there was only ever one slot for a camera.
//   Now every phone is identified by the address its packets come FROM (no
//   protocol change needed, and it cannot be spoofed by accident), and each one
//   gets its own reassembly map, its own latest frame and its own frame rate.
//   The UI picks which device to preview; recording accumulates ONLY the packets
//   of the device being recorded.
//
// RECORDING ASKED FOR BY THE PHONE
//   A phone can send a type-8 packet meaning "record N seconds into slot S".
//   PhoneLink only QUEUES it (takeRequests); the actual recording is done by
//   the main thread, which is the only one that may touch the slot bank and
//   draw. The PC pushes slot occupancy back with type 7 so the phone can grey
//   out the slots that are already taken, and progress with type 9.
//
// The receiver runs on its OWN thread and only does networking + buffering (no
// raylib, no engine access). The main thread pulls the latest JPEG for preview
// and, while recording, the accumulated JPEG frames + PCM to build the slot.
// ============================================================================
#ifndef PINGUUS_PHONELINK_H
#define PINGUUS_PHONELINK_H

#include <vector>
#include <unordered_map>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <algorithm>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "iphlpapi.lib")
  typedef int socklen_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <ifaddrs.h>
  #include <fcntl.h>
#endif

class PhoneLink {
public:
    static const int    kHeader = 16;
    static const int    kAudioRate = 44100;   // phone must send mono 16-bit @44100
    static const size_t kMaxAudioRec = (size_t)kAudioRate * 60; // cap 60 s

    // Packet types (see the header comment).
    enum : uint8_t {
        kTypeVideo = 1, kTypeAudio = 2, kTypeHello = 3,
        kTypeDiscover = 5, kTypeHere = 6,
        kTypeSlots = 7, kTypeRecReq = 8, kTypeRecStatus = 9
    };

    // What the phone is told about a recording it asked for.
    enum : uint8_t {
        kStatIdle = 0, kStatQueued = 1, kStatRecording = 2,
        kStatDone = 3, kStatFailed = 4, kStatRefused = 5
    };

    // How many slots of each kind the phone is shown. Mirrors main.cpp's
    // SAMPLE_BASE / MAX_SLOTS: 64 CLIP slots then 64 SMP slots.
    static const int kClipSlots = 64;
    static const int kSampleSlots = 64;

    // One phone, as the UI sees it.
    struct Device {
        uint64_t key = 0;         // address+port, the identity of this phone
        std::string ip;
        std::string name;         // what the phone called itself, else its ip
        double fps = 0.0;
        bool streaming = false;   // sent video/audio in the last ~2.5 s
        bool hasFrame = false;
    };

    // "Record <seconds> into <slot>", asked for by a phone.
    struct RecordRequest {
        uint64_t device = 0;
        std::string deviceName;
        int  slot = 0;            // absolute slot index (0..127)
        int  seconds = 4;
        bool withVideo = true;
    };

    PhoneLink() {}
    ~PhoneLink() { stop(); }

    // Start listening on `port`. Returns false if the socket can't be opened.
    bool start(int port) {
        if (running_) return true;
        port_ = port;
#if defined(_WIN32)
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
        sock_ = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) return false;
        int reuse = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        // 200 ms receive timeout so the loop can notice stop().
#if defined(_WIN32)
        DWORD tv = 200; setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        // Big receive buffer to absorb bursts of chunks.
        int rcv = 1 << 21; setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((uint16_t)port);
        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) { closeSock(); return false; }
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        closeSock();
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    bool isRunning() const { return running_; }
    int  port() const { return port_; }

    // ------------------------------------------------------------------
    // The phones we can see
    // ------------------------------------------------------------------
    // Everything that has spoken to us recently. Devices that go quiet for
    // ~8 s are dropped: a phone that walks out of range should stop cluttering
    // the picker, but the window is wide enough to survive a lift ride.
    std::vector<Device> devices() {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked();
        std::vector<Device> out;
        out.reserve(devs_.size());
        uint64_t t = nowMs();
        for (const auto& kv : devs_) {
            Device d;
            d.key = kv.first;
            d.ip = kv.second.ip;
            d.name = kv.second.name.empty() ? kv.second.ip : kv.second.name;
            d.fps = kv.second.fps;
            d.streaming = (t - kv.second.lastStreamMs) < 2500;
            d.hasFrame = !kv.second.latestJpeg.empty();
            out.push_back(d);
        }
        // Stable order so the list does not jump around under the cursor.
        std::sort(out.begin(), out.end(),
                  [](const Device& a, const Device& b) { return a.key < b.key; });
        return out;
    }

    int deviceCount() {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked();
        return (int)devs_.size();
    }

    // Which phone the preview and the PC's own "Record from PHONE" use. Picking
    // one by hand sticks; if that phone disappears we fall back to whichever is
    // streaming, so the panel is never left pointing at nothing.
    uint64_t selected() {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked();
        if (selected_ && devs_.count(selected_)) return selected_;
        uint64_t best = 0; uint64_t bestSeen = 0;
        for (const auto& kv : devs_) {
            if (kv.second.lastStreamMs > bestSeen) { bestSeen = kv.second.lastStreamMs; best = kv.first; }
        }
        selected_ = best;
        return selected_;
    }
    void select(uint64_t key) {
        std::lock_guard<std::mutex> lk(mtx_);
        selected_ = key;
    }
    std::string deviceName(uint64_t key) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = devs_.find(key);
        if (it == devs_.end()) return std::string();
        return it->second.name.empty() ? it->second.ip : it->second.name;
    }

    // A phone is "connected" if the SELECTED one sent a stream packet recently.
    bool connected() {
        uint64_t k = selected();
        if (!k) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = devs_.find(k);
        return it != devs_.end() && (nowMs() - it->second.lastStreamMs) < 2500;
    }
    // True if ANY phone is streaming, whichever is selected.
    bool anyConnected() {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked();
        uint64_t t = nowMs();
        for (const auto& kv : devs_) if ((t - kv.second.lastStreamMs) < 2500) return true;
        return false;
    }
    double framesPerSec() {
        uint64_t k = selected();
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = devs_.find(k);
        return it == devs_.end() ? 0.0 : it->second.fps;
    }

    // True when a phone asked "where is Pinguus?" in the last ~5 s. The DEV →
    // PHONE panel shows this so you can tell the two ends found each other even
    // before you press START STREAMING on the phone.
    bool phoneIsLooking() const {
        return running_ && lastDiscoverMs_.load() != 0 && (nowMs() - lastDiscoverMs_.load()) < 5000;
    }

    // Copy the most recent complete JPEG frame of the SELECTED phone. `version`
    // is bumped each new frame; pass your last-seen value to skip redundant
    // decodes. Returns true if a frame is available.
    bool latestFrame(std::vector<uint8_t>& out, uint64_t& version) {
        return latestFrameOf(selected(), out, version);
    }
    bool latestFrameOf(uint64_t key, std::vector<uint8_t>& out, uint64_t& version) {
        if (!key) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = devs_.find(key);
        if (it == devs_.end() || it->second.latestJpeg.empty()) return false;
        version = it->second.frameVersion;
        out = it->second.latestJpeg;
        return true;
    }

    // --- Recording control (main thread) ---
    // Records from ONE phone. Anything the others send while this runs is still
    // previewed, but never mixed into the take — which is exactly the bug that
    // made two phones unusable together.
    void startRecording(uint64_t device) {
        std::lock_guard<std::mutex> lk(mtx_);
        recVideo_.clear(); recAudio_.clear();
        recDevice_ = device ? device : selected_;
        recStartMs_ = nowMs(); recording_ = true;
    }
    void startRecording() { startRecording(selected()); }

    // Stops and hands over the captured JPEG frames + PCM. `outFps` is the
    // measured frame rate over the take (>=1).
    void stopRecording(std::vector<std::vector<uint8_t>>& outFrames,
                       std::vector<int16_t>& outAudio, double& outFps) {
        std::lock_guard<std::mutex> lk(mtx_);
        recording_ = false;
        recDevice_ = 0;
        double secs = (nowMs() - recStartMs_) / 1000.0;
        outFrames.swap(recVideo_);
        outAudio.swap(recAudio_);
        outFps = (secs > 0.05 && !outFrames.empty()) ? (outFrames.size() / secs) : 15.0;
        if (outFps < 1.0) outFps = 1.0;
        recVideo_.clear(); recAudio_.clear();
    }
    bool isRecording() const { return recording_; }
    // Live progress for the UI while recording.
    void recProgress(int& frames, int& audioSamples) {
        std::lock_guard<std::mutex> lk(mtx_);
        frames = (int)recVideo_.size();
        audioSamples = (int)recAudio_.size();
    }

    // ------------------------------------------------------------------
    // Recordings asked for BY a phone
    // ------------------------------------------------------------------
    // Drains the queue. The main thread calls this once a frame; PhoneLink
    // never records on its own, because only the main thread may touch slots.
    std::vector<RecordRequest> takeRequests() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<RecordRequest> out;
        out.swap(requests_);
        return out;
    }

    // Tell one phone which slots are taken, and whether it may record at all.
    // `clip` and `smp` are kClipSlots / kSampleSlots bytes: 0 = free, 1 = used.
    void sendSlots(uint64_t key, const uint8_t* clip, const uint8_t* smp,
                   bool allowed, bool busy, int queueLen) {
        uint8_t pkt[kHeader + 4 + kClipSlots + kSampleSlots];
        memset(pkt, 0, sizeof(pkt));
        const uint16_t plen = 4 + kClipSlots + kSampleSlots;
        putHeader(pkt, kTypeSlots, plen);
        pkt[kHeader + 0] = allowed ? 1 : 0;
        pkt[kHeader + 1] = busy ? 1 : 0;
        pkt[kHeader + 2] = (uint8_t)(queueLen < 255 ? queueLen : 255);
        pkt[kHeader + 3] = 0;
        memcpy(pkt + kHeader + 4, clip, kClipSlots);
        memcpy(pkt + kHeader + 4 + kClipSlots, smp, kSampleSlots);
        sendTo(key, pkt, kHeader + plen);
    }
    void broadcastSlots(const uint8_t* clip, const uint8_t* smp,
                        bool allowed, bool busy, int queueLen) {
        std::vector<uint64_t> keys;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            expireLocked();
            for (const auto& kv : devs_) keys.push_back(kv.first);
        }
        for (uint64_t k : keys) sendSlots(k, clip, smp, allowed, busy, queueLen);
    }

    // Progress for a recording a phone asked for. `msg` is shown verbatim.
    void sendRecStatus(uint64_t key, uint8_t state, int secondsLeft,
                       int queuePos, int slot, const std::string& msg) {
        std::string m = msg;
        if (m.size() > 96) m.resize(96);
        std::vector<uint8_t> pkt(kHeader + 4 + m.size());
        memset(pkt.data(), 0, pkt.size());
        const uint16_t plen = (uint16_t)(4 + m.size());
        putHeader(pkt.data(), kTypeRecStatus, plen);
        pkt[kHeader + 0] = state;
        pkt[kHeader + 1] = (uint8_t)(secondsLeft < 0 ? 0 : (secondsLeft > 255 ? 255 : secondsLeft));
        pkt[kHeader + 2] = (uint8_t)(queuePos < 0 ? 0 : (queuePos > 255 ? 255 : queuePos));
        pkt[kHeader + 3] = (uint8_t)(slot & 0xFF);
        if (!m.empty()) memcpy(pkt.data() + kHeader + 4, m.data(), m.size());
        sendTo(key, pkt.data(), (int)pkt.size());
    }

    // Local IPv4 addresses (for showing the user which IP the phone should target).
    static std::vector<std::string> localIPv4() {
        std::vector<std::string> out;
#if defined(_WIN32)
        // Minimal: rely on gethostname/gethostbyname.
        char host[256]; if (gethostname(host, sizeof(host)) == 0) {
            addrinfo hints{}; hints.ai_family = AF_INET; addrinfo* res = nullptr;
            if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
                for (addrinfo* p = res; p; p = p->ai_next) {
                    char buf[64];
                    sockaddr_in* s = (sockaddr_in*)p->ai_addr;
                    inet_ntop(AF_INET, &s->sin_addr, buf, sizeof(buf));
                    out.push_back(buf);
                }
                freeaddrinfo(res);
            }
        }
#else
        ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) == 0) {
            for (ifaddrs* p = ifap; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                char buf[64];
                sockaddr_in* s = (sockaddr_in*)p->ifa_addr;
                inet_ntop(AF_INET, &s->sin_addr, buf, sizeof(buf));
                std::string ip = buf;
                if (ip == "127.0.0.1") continue; // skip loopback
                out.push_back(ip);
            }
            freeifaddrs(ifap);
        }
#endif
        return out;
    }

private:
    struct Assembly {
        uint16_t count = 0;
        int received = 0;
        std::vector<std::vector<uint8_t>> chunks;
    };

    // Everything we know about one phone. Each has its OWN reassembly map, so
    // two phones counting frames from 0 can never contaminate each other.
    struct DevState {
        sockaddr_in addr{};
        socklen_t   addrLen = sizeof(sockaddr_in);
        std::string ip;
        std::string name;
        uint64_t lastSeenMs = 0;     // any packet, discovery included
        uint64_t lastStreamMs = 0;   // video/audio only — drives "connected"
        std::unordered_map<uint32_t, Assembly> pending;
        std::vector<uint8_t> latestJpeg;
        uint64_t frameVersion = 0;
        double   fps = 0.0;
        int      framesThisSec = 0;
        uint64_t lastFpsMs = 0;
    };

    static uint64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    void closeSock() {
        if (sock_ >= 0) {
#if defined(_WIN32)
            closesocket(sock_);
#else
            ::close(sock_);
#endif
            sock_ = -1;
        }
    }

    // The identity of a phone: its address AND its port. Two phones behind the
    // same NAT differ by port, and one phone that reopens its socket simply
    // shows up as a new device rather than corrupting the old one's stream.
    static uint64_t keyOf(const sockaddr_in& a) {
        return ((uint64_t)a.sin_addr.s_addr << 16) | (uint64_t)ntohs(a.sin_port);
    }
    static std::string ipOf(const sockaddr_in& a) {
        char buf[64] = {0};
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }

    // Caller must hold mtx_.
    void expireLocked() {
        uint64_t t = nowMs();
        for (auto it = devs_.begin(); it != devs_.end(); ) {
            if (t - it->second.lastSeenMs > 8000) {
                if (selected_ == it->first) selected_ = 0;
                if (recDevice_ == it->first) recDevice_ = 0;
                it = devs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void putHeader(uint8_t* pkt, uint8_t type, uint16_t payloadLen) {
        pkt[0] = 'P'; pkt[1] = 'I'; pkt[2] = 'N'; pkt[3] = 'G';
        pkt[4] = type;
        pkt[5] = 0;
        pkt[6] = pkt[7] = pkt[8] = pkt[9] = 0;
        pkt[10] = pkt[11] = pkt[12] = pkt[13] = 0;
        pkt[14] = (uint8_t)(payloadLen & 0xFF);
        pkt[15] = (uint8_t)((payloadLen >> 8) & 0xFF);
    }

    // Send to one known device. Safe to call from the main thread: sendto on a
    // UDP socket is atomic per datagram, and only the address lookup needs the
    // lock (which is why the copy happens inside it).
    void sendTo(uint64_t key, const uint8_t* pkt, int len) {
        sockaddr_in to{}; socklen_t tolen = sizeof(to);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = devs_.find(key);
            if (it == devs_.end()) return;
            to = it->second.addr;
            tolen = it->second.addrLen;
        }
        if (sock_ < 0) return;
        sendto(sock_, (const char*)pkt, len, 0, (const sockaddr*)&to, tolen);
    }

    // This computer's name, so the phone's list says "PC-SALON" instead of a
    // bare IP. Falls back to the literal "Pinguus" if the OS won't tell us.
    static std::string hostName() {
        char h[128] = {0};
        if (gethostname(h, sizeof(h) - 1) == 0 && h[0] != '\0') return std::string(h);
        return std::string("Pinguus");
    }

    void replyToDiscover(const sockaddr_in& to, socklen_t tolen) {
        std::string name = hostName();
        if (name.size() > 64) name.resize(64);
        uint8_t pkt[16 + 64];
        memset(pkt, 0, sizeof(pkt));
        putHeader(pkt, kTypeHere, (uint16_t)name.size());
        memcpy(pkt + 16, name.data(), name.size());
        sendto(sock_, (const char*)pkt, (int)(16 + name.size()), 0, (const sockaddr*)&to, tolen);
    }

    void loop() {
        std::vector<uint8_t> buf(65536);
        while (running_) {
            sockaddr_in from{}; socklen_t fl = sizeof(from);
            int n = (int)recvfrom(sock_, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
            if (n < kHeader) continue;
            if (!(buf[0] == 'P' && buf[1] == 'I' && buf[2] == 'N' && buf[3] == 'G')) continue;
            uint8_t type = buf[4];

            // A phone hunting for us on the broadcast address. Answer with our
            // name and go back to waiting — this is not a stream, so it must
            // NOT count as "connected" for anybody.
            if (type == kTypeDiscover) {
                replyToDiscover(from, fl);
                lastDiscoverMs_ = nowMs();
                continue;
            }

            uint16_t chunkIdx   = rd16(&buf[6]);
            uint16_t chunkCount = rd16(&buf[8]);
            uint32_t seq        = rd32(&buf[10]);
            uint16_t plen       = rd16(&buf[14]);
            if (kHeader + (int)plen > n) plen = (uint16_t)(n - kHeader);
            const uint8_t* payload = &buf[kHeader];

            const uint64_t key = keyOf(from);
            const uint64_t t = nowMs();

            std::lock_guard<std::mutex> lk(mtx_);
            DevState& d = devs_[key];
            if (d.lastSeenMs == 0) {          // first time we hear from this phone
                d.addr = from; d.addrLen = fl;
                d.ip = ipOf(from);
                d.lastFpsMs = t;
            }
            d.addr = from; d.addrLen = fl;    // keep it fresh
            d.lastSeenMs = t;

            if (type == kTypeHello) {
                // The payload is the phone's own name, when it sends one.
                if (plen > 0) {
                    std::string nm((const char*)payload, (size_t)plen);
                    // Keep it printable and short: it goes straight into the UI.
                    std::string clean;
                    for (char c : nm) if ((unsigned char)c >= 32 && clean.size() < 40) clean += c;
                    if (!clean.empty()) d.name = clean;
                }
                d.lastStreamMs = t;   // a hello means "I am here and about to send"
                continue;
            }

            if (type == kTypeRecReq) {
                // "Record <seconds> into <slot>". Only queued here; the main
                // thread decides whether it is allowed and does the work.
                if (plen >= 3) {
                    RecordRequest r;
                    r.device = key;
                    r.deviceName = d.name.empty() ? d.ip : d.name;
                    bool isSample = (payload[0] != 0);
                    int idx = payload[1];
                    r.withVideo = !isSample;
                    r.slot = isSample ? (kClipSlots + (idx % kSampleSlots)) : (idx % kClipSlots);
                    r.seconds = payload[2];
                    if (r.seconds < 1) r.seconds = 1;
                    if (r.seconds > 60) r.seconds = 60;
                    // A phone that taps twice should not queue twice.
                    if (requests_.size() < 8) requests_.push_back(r);
                }
                continue;
            }

            if (type == kTypeAudio) {
                d.lastStreamMs = t;
                if (recording_ && key == recDevice_ && recAudio_.size() < kMaxAudioRec) {
                    const int16_t* s = (const int16_t*)payload;
                    int cnt = plen / 2;
                    recAudio_.insert(recAudio_.end(), s, s + cnt);
                }
                continue;
            }

            if (type != kTypeVideo) continue;   // unknown → just a keep-alive
            d.lastStreamMs = t;

            // Video chunk → reassemble, in THIS phone's own bucket.
            if (chunkCount == 0) continue;
            std::vector<uint8_t> completed;
            Assembly& a = d.pending[seq];
            if (a.count != chunkCount) { a.count = chunkCount; a.chunks.assign(chunkCount, {}); a.received = 0; }
            if (chunkIdx < chunkCount && a.chunks[chunkIdx].empty()) {
                a.chunks[chunkIdx].assign(payload, payload + plen);
                a.received++;
            }
            if (a.received == a.count) {
                for (auto& c : a.chunks) completed.insert(completed.end(), c.begin(), c.end());
                d.pending.erase(seq);
            }
            // Prune stale partial frames (keep the map small).
            if (d.pending.size() > 12) {
                uint32_t minSeq = UINT32_MAX;
                for (auto& kv : d.pending) minSeq = std::min(minSeq, kv.first);
                d.pending.erase(minSeq);
            }

            if (!completed.empty()) {
                d.latestJpeg = completed;
                d.frameVersion++;
                if (recording_ && key == recDevice_ && recVideo_.size() < 3600)
                    recVideo_.push_back(completed);
                d.framesThisSec++;
                if (t - d.lastFpsMs >= 1000) {
                    d.fps = d.framesThisSec * 1000.0 / (t - d.lastFpsMs);
                    d.framesThisSec = 0;
                    d.lastFpsMs = t;
                }
            }
        }
    }

    static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    static uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

    int  sock_ = -1;
    int  port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // ONE mutex for the whole device map and the recording buffers. The frame
    // rate here is a couple of hundred small packets a second, so the coarser
    // lock costs nothing measurable and removes any chance of a lock-ordering
    // mistake between "the phone list" and "what is being recorded".
    std::mutex mtx_;
    std::unordered_map<uint64_t, DevState> devs_;
    uint64_t selected_ = 0;
    std::vector<RecordRequest> requests_;

    std::atomic<uint64_t> lastDiscoverMs_{0};

    std::atomic<bool> recording_{false};
    uint64_t recDevice_ = 0;       // only this phone's packets go into the take
    uint64_t recStartMs_ = 0;
    std::vector<std::vector<uint8_t>> recVideo_; // one JPEG per frame
    std::vector<int16_t> recAudio_;              // mono 16-bit @44100
};

#endif // PINGUUS_PHONELINK_H
