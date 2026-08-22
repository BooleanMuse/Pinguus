// ============================================================================
// ApkServer.h — Hands the "Pinguus Cam" app to a phone over the local network.
//
// WHY THIS EXISTS
//   Installing our own Android app used to mean: find the .apk in the project
//   folder, get it onto the phone somehow (cable, cloud, e-mail), then open it.
//   That is by far the worst step in the whole phone-camera flow, and it is
//   entirely avoidable: the PC is already on the same Wi-Fi as the phone.
//
//   So Pinguus serves the file itself. The DEV → PHONE panel shows a short
//   address like  http://192.168.1.42:45814  — the user types that into the
//   phone's browser, taps DOWNLOAD, and installs. No cable, no cloud account,
//   no QR code to scan.
//
// WHAT IT SERVES
//   GET /                    a one-page installer with the three steps
//   GET /PinguusCam.apk      the app itself
//   anything else            404
//
// SCOPE / SAFETY
//   It serves exactly ONE file, from a path this program chose at startup —
//   there is no path parsing, so no way to walk out of the folder and read
//   something else. It only listens while you have the panel open (start/stop
//   is explicit), it is HTTP on a LAN port, and it never writes anything.
//
//   Runs on its own thread; one connection at a time, which is plenty for a
//   25 KB download.
// ============================================================================
#ifndef PINGUUS_APKSERVER_H
#define PINGUUS_APKSERVER_H

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

class ApkServer {
public:
    ApkServer() {}
    ~ApkServer() { stop(); }

    // `apkPath` is the file to hand out. Returns false if the port is taken.
    bool start(int port, const std::string& apkPath) {
        if (running_) return true;
        port_ = port;
        apkPath_ = apkPath;
        hits_ = 0;
#if defined(_WIN32)
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
        listen_ = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) return false;
        int reuse = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (bind(listen_, (sockaddr*)&addr, sizeof(addr)) < 0) { closeSock(listen_); listen_ = -1; return false; }
        if (::listen(listen_, 4) < 0) { closeSock(listen_); listen_ = -1; return false; }
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (listen_ >= 0) { closeSock(listen_); listen_ = -1; }
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    bool isRunning() const { return running_; }
    int  port() const { return port_; }
    // How many times the app has been downloaded — the panel shows it so you
    // can see the phone actually fetched the file.
    int  downloads() const { return hits_.load(); }

private:
    static void closeSock(int s) {
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
    }

    void loop() {
        while (running_) {
            // select() so stop() is noticed within 200 ms instead of blocking
            // forever in accept().
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listen_, &rd);
            timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
            int r = select(listen_ + 1, &rd, nullptr, nullptr, &tv);
            if (r <= 0) continue;

            sockaddr_in from{}; socklen_t fl = sizeof(from);
            int c = (int)accept(listen_, (sockaddr*)&from, &fl);
            if (c < 0) continue;
            serve(c);
            closeSock(c);
        }
    }

    void serve(int c) {
        // We only need the request line; read one buffer's worth and stop.
        char req[2048] = {0};
        int n = (int)recv(c, req, sizeof(req) - 1, 0);
        if (n <= 0) return;
        req[n] = '\0';

        // "GET /path HTTP/1.1"
        char path[512] = {0};
        if (sscanf(req, "GET %511s", path) != 1) { sendSimple(c, "400 Bad Request", "text/plain", "bad request"); return; }

        if (strcmp(path, "/PinguusCam.apk") == 0) {
            sendApk(c);
        } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            sendSimple(c, "200 OK", "text/html; charset=utf-8", page());
        } else {
            sendSimple(c, "404 Not Found", "text/plain", "Not found. Open http://<this-address>/ instead.");
        }
    }

    void sendAll(int c, const char* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            int w = (int)send(c, data + off, (int)(len - off), 0);
            if (w <= 0) return;
            off += (size_t)w;
        }
    }

    void sendSimple(int c, const char* status, const char* type, const std::string& body) {
        char head[512];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n\r\n",
                          status, type, body.size());
        sendAll(c, head, (size_t)hn);
        sendAll(c, body.data(), body.size());
    }

    void sendApk(int c) {
        FILE* f = fopen(apkPath_.c_str(), "rb");
        if (!f) {
            sendSimple(c, "404 Not Found", "text/plain",
                       "PinguusCam.apk is missing. Build it with android/build_apk.sh");
            return;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char head[512];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/vnd.android.package-archive\r\n"
                          "Content-Disposition: attachment; filename=\"PinguusCam.apk\"\r\n"
                          "Content-Length: %ld\r\n"
                          "Connection: close\r\n\r\n",
                          size);
        sendAll(c, head, (size_t)hn);

        std::vector<char> buf(64 * 1024);
        size_t got;
        while ((got = fread(buf.data(), 1, buf.size(), f)) > 0) sendAll(c, buf.data(), got);
        fclose(f);
        hits_++;
    }

    // Deliberately plain: no external CSS, no fonts, no scripts, so it renders
    // instantly on any phone browser and works with no internet connection.
    static std::string page() {
        return
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Pinguus Cam</title></head>"
            "<body style=\"font-family:system-ui,sans-serif;background:#181a26;color:#eee;"
            "margin:0;padding:24px;line-height:1.5\">"
            "<h1 style=\"color:#8fd0ff;margin-top:0\">Pinguus Cam</h1>"
            "<p>Turns this phone into a camera and microphone for Pinguus on your PC. "
            "No ads, no accounts, no tracking.</p>"
            "<p><a href=\"/PinguusCam.apk\" "
            "style=\"display:inline-block;background:#3a7d4f;color:#fff;text-decoration:none;"
            "padding:16px 28px;border-radius:8px;font-size:20px\">DOWNLOAD THE APP</a></p>"
            "<ol>"
            "<li>Tap the button above. Android will warn you that the file may be harmful "
            "because it did not come from the Play Store &mdash; that warning appears for "
            "every app installed this way. Choose <b>Download anyway</b>.</li>"
            "<li>Open the downloaded file and allow <b>install from unknown sources</b> "
            "for your browser when asked.</li>"
            "<li>Open <b>Pinguus Cam</b> and press <b>SEARCH FOR PINGUUS</b>. It finds this "
            "computer on its own &mdash; there is no address to type.</li>"
            "</ol>"
            "<p style=\"color:#9aa;font-size:14px\">The phone and the PC must be on the same "
            "Wi-Fi. USB tethering works too, and is even faster.</p>"
            "</body></html>";
    }

    int listen_ = -1;
    int port_ = 0;
    std::string apkPath_;
    std::atomic<bool> running_{false};
    std::atomic<int>  hits_{0};
    std::thread thread_;
};

#endif // PINGUUS_APKSERVER_H
