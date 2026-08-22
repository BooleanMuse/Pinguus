// En Windows, varias cabeceras de aquí abajo acaban arrastrando <windows.h>
// (winsock2.h lo hace), y windows.h declara Rectangle(), DrawText(),
// CloseWindow(), LoadImage()... con los MISMOS nombres que la API de raylib.
// Sin estos NO*, esta unidad de compilación no compila en Windows: `Rectangle r`
// pasa a leerse como una llamada a la función Rectangle() de GDI.
// Lo que sí necesita windows.h completo (lanzar procesos, memoria compartida)
// vive aparte, en PlatformProc.cpp.
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOGDI       // fuera Rectangle(), Ellipse(), TextOut()...
  #define NOMINMAX
  // NOUSER no sirve aquí: miniaudio necesita OLE (WASAPI) y las cabeceras de
  // OLE necesitan el tipo MSG, que NOUSER elimina.
  //
  // Quedan dos funciones de user32 que chocan de frente con raylib y que no
  // tienen guarda propia: CloseWindow(HWND) y ShowCursor(BOOL). Como raylib
  // las declara extern "C", no pueden convivir como sobrecargas. Las
  // renombramos MIENTRAS se parsea windows.h para que raylib se quede con el
  // nombre bueno; Pinguus no usa las versiones de Win32.
  //
  // Incluimos windows.h aquí a propósito, para fijar el orden: si lo dejamos
  // llegar por su cuenta (winsock2.h desde PhoneLink.h) podría parsearse
  // DESPUÉS de raylib.h y volver el choque.
  #define CloseWindow Win32CloseWindow_unused
  #define ShowCursor  Win32ShowCursor_unused
  #include <windows.h>
  #undef CloseWindow
  #undef ShowCursor
  // Estos otros son MACROS (DrawText -> DrawTextA) que reescribirían las
  // llamadas a raylib antes de que el compilador las viera.
  #undef DrawText
  #undef DrawTextEx
  #undef LoadImage
  #undef PlaySound
  #undef near
  #undef far
#endif

#define MA_IMPLEMENTATION
#include "miniaudio.h"
#include "raylib.h"
#include "raymath.h"
extern "C" {
#include "external/cgltf.h"   // parser glTF (ya viene con raylib) para leer .vrma
}
#include "RtMidi.h"

#include "CommandQueue.h"
#include "VisualSnapshot.h"
#include "VideoEventQueue.h"
#include "VideoVoicePool.h"
#include "SequencerEngine.h"
#include "BlendFX.h"
#include "NtscFX.h"
#include "PitchToNotes.h"
#include "MelodyBank.h"
#include "ScriptEngine.h"
#include "PhoneLink.h"
#include "IpCamLink.h"
#include "ApkServer.h"
#include "PlatformProc.h"
#include "Tailscale.h"
#include "BvhAnim.h"

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

// mkdir(), para crear temp/ donde aterrizan todas las grabaciones.
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#if defined(_WIN32)
#define POPEN _popen
#define PCLOSE _pclose
// Modo de escritura para las tuberías que llevan BYTES (fotogramas crudos hacia
// ffmpeg). En Windows una tubería abierta en modo texto — que es lo que da "w" —
// traduce cada 0x0A a 0x0D 0x0A. En un flujo rawvideo eso mete un byte de más
// cada vez que un canal R/G/B vale 10, así que las filas se van desplazando y
// los fotogramas se descuadran unos respecto a otros: el vídeo exportado salía
// convertido en rayas diagonales de colores. "wb" desactiva esa traducción.
#define POPEN_WB "wb"
#else
#define POPEN popen
#define PCLOSE pclose
// OJO: aquí NO vale "wb". glibc valida el modo de popen() y rechaza cualquier
// letra que no sea "r"/"w" (+"e"), devolviendo NULL con EINVAL. En POSIX no hay
// modo texto, así que "w" ya es binario.
#define POPEN_WB "w"
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>    // open(): comprobar si la cámara está ocupada
#include <cerrno>
#endif

// Entrecomilla una ruta para el intérprete de comandos DE ESTA plataforma.
// Hace falta porque las rutas de Pinguus llevan espacios en cuanto el usuario
// graba en "Mis documentos". Escribir 'ruta' a pelo sólo funciona en POSIX: en
// Windows cmd.exe NO trata la comilla simple como comilla, así que se la pasaba
// a ffmpeg como parte del nombre y la grabación del teléfono / cámara IP
// terminaba en un archivo llamado literalmente 'pinguus_phone_....mp4' o en un
// "No such file or directory".
static std::string ShQuote(const std::string& s) {
#if defined(_WIN32)
    // cmd.exe no tiene escape dentro de comillas dobles; lo único razonable es
    // quitar las que vengan en la ruta (que además Windows no permite en un
    // nombre de archivo).
    std::string r;
    for (char c : s) if (c != '"') r += c;
    return "\"" + r + "\"";
#else
    std::string r;
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return "'" + r + "'";
#endif
}


// ---------------------------------------------------------------------------
// Diálogos nativos de archivo
// ---------------------------------------------------------------------------

static int RunDialogCommand(const char* cmd, char* out, size_t outSz) {
#if defined(_WIN32)
    // Sin ventana de consola parpadeando por delante del diálogo.
    std::string captured;
    int code = RunCommandRead(cmd, captured);
    out[0] = '\0';
    size_t nl = captured.find_first_of("\r\n");
    std::string first = (nl == std::string::npos) ? captured : captured.substr(0, nl);
    snprintf(out, outSz, "%s", first.c_str());
    if (code == 0 && out[0] != '\0') return 1;
    if (code == 0 || code == 1) return 0;
    return -1;
#else
    FILE* p = POPEN(cmd, "r");
    if (p == nullptr) return -1;
    out[0] = '\0';
    if (fgets(out, (int)outSz, p) != nullptr) {
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    }
    int st = PCLOSE(p);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 127;
    if (code == 0 && out[0] != '\0') return 1;
    if (code == 0 || code == 1) return 0;
    return -1;
#endif
}

static int NativeSaveDialog(const char* title, const char* defaultName, char* out, size_t outSz) {
    char cmd[1024];
#if defined(_WIN32)
    // Diálogo nativo de comdlg32 (PlatformProc.cpp). Antes esto lanzaba
    // powershell y en Windows no se abría nada; ver PlatformProc.h.
    (void)cmd;
    return NativeSaveDialogWin(title, defaultName, out, outSz);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd),
             "osascript -e 'POSIX path of (choose file name with prompt \"%s\" default name \"%s\")' 2>/dev/null",
             title, defaultName);
    return RunDialogCommand(cmd, out, outSz);
#else
    snprintf(cmd, sizeof(cmd),
             "kdialog --getsavefilename \"$PWD/%s\" --title '%s' 2>/dev/null", defaultName, title);
    int r = RunDialogCommand(cmd, out, outSz);
    if (r >= 0) return r;
    snprintf(cmd, sizeof(cmd),
             "zenity --file-selection --save --title='%s' --filename=\"$PWD/%s\" 2>/dev/null", title, defaultName);
    return RunDialogCommand(cmd, out, outSz);
#endif
}

static int NativeOpenDialog(const char* title, const char* extPattern, char* out, size_t outSz) {
    char cmd[1024];
#if defined(_WIN32)
    (void)cmd;
    return NativeOpenDialogWin(title, extPattern, out, outSz);
#elif defined(__APPLE__)
    (void)extPattern;
    snprintf(cmd, sizeof(cmd),
             "osascript -e 'POSIX path of (choose file with prompt \"%s\")' 2>/dev/null", title);
    return RunDialogCommand(cmd, out, outSz);
#else
    snprintf(cmd, sizeof(cmd),
             "kdialog --getopenfilename \"$PWD\" '%s' --title '%s' 2>/dev/null", extPattern, title);
    int r = RunDialogCommand(cmd, out, outSz);
    if (r >= 0) return r;
    snprintf(cmd, sizeof(cmd),
             "zenity --file-selection --title='%s' --file-filter='%s' 2>/dev/null", title, extPattern);
    return RunDialogCommand(cmd, out, outSz);
#endif
}

static int NativeInputDialog(const char* title, const char* defaultText, char* out, size_t outSz) {
    char cmd[1024];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
             "powershell -NoProfile -STA -Command \"Add-Type -AssemblyName Microsoft.VisualBasic; "
             "[Microsoft.VisualBasic.Interaction]::InputBox('%s','Pinguus','%s')\"",
             title, defaultText);
    return RunDialogCommand(cmd, out, outSz);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd),
             "osascript -e 'text returned of (display dialog \"%s\" default answer \"%s\")' 2>/dev/null",
             title, defaultText);
    return RunDialogCommand(cmd, out, outSz);
#else
    snprintf(cmd, sizeof(cmd),
             "kdialog --inputbox '%s' '%s' 2>/dev/null", title, defaultText);
    int r = RunDialogCommand(cmd, out, outSz);
    if (r >= 0) return r;
    snprintf(cmd, sizeof(cmd),
             "zenity --entry --title='Pinguus' --text='%s' --entry-text='%s' 2>/dev/null", title, defaultText);
    return RunDialogCommand(cmd, out, outSz);
#endif
}

static void EnsureExtension(char* path, size_t sz, const char* ext) {
    size_t len = strlen(path);
    size_t elen = strlen(ext);
    if (len >= elen) {
        bool has = true;
        for (size_t i = 0; i < elen; i++) {
            char a = path[len - elen + i], b = ext[i];
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) { has = false; break; }
        }
        if (has) return;
    }
    if (len + elen + 1 < sz) strcat(path, ext);
}

// ---------------------------------------------------------------------------
// Carpeta temp/: donde va TODO lo que se graba
//
// Antes cada grabación caía en el directorio de trabajo, mezclada con el
// ejecutable y los proyectos. Ahora todo (vídeo exportado, audio exportado,
// cámara, micrófono, teléfono) aterriza en `temp/`, y al cerrar el programa se
// pregunta si conservarlo. Lo que se registra en g_sessionClips es sólo lo
// grabado EN ESTA SESIÓN: si se contesta que no, se borra eso y nada más — el
// material de sesiones anteriores no se toca nunca.
// ---------------------------------------------------------------------------
static const char* kTempDir = "temp";
static std::vector<std::string> g_sessionClips;

static void EnsureTempDir() {
#if defined(_WIN32)
    _mkdir(kTempDir);
#else
    mkdir(kTempDir, 0755);
#endif
}

// Ruta dentro de temp/, creando la carpeta si hace falta. Devuelve un puntero a
// un buffer estático rotatorio (como TextFormat de raylib): vale para pasarlo
// directo a fopen/snprintf, no para guardarlo.
static const char* TempPath(const char* name) {
    static char bufs[4][512];
    static int which = 0;
    EnsureTempDir();
    which = (which + 1) % 4;
    snprintf(bufs[which], sizeof(bufs[which]), "%s/%s", kTempDir, name);
    return bufs[which];
}

// Apunta un archivo como "grabado en esta sesión" para poder ofrecer borrarlo
// al salir. Se ignoran los que no están dentro de temp/ (un Guardar como... a
// la carpeta del usuario no es material desechable).
static void RegisterSessionClip(const char* path) {
    if (path == nullptr || path[0] == '\0') return;
    std::string p(path);
    if (p.rfind(std::string(kTempDir) + "/", 0) != 0) return;
    for (const std::string& e : g_sessionClips) if (e == p) return;
    g_sessionClips.push_back(p);
}

// Copia un archivo. Devuelve false si algo falló, y en ese caso NO deja un
// destino a medias: lo borra. Un .mp4 truncado es peor que ninguno, porque
// parece que está y no se abre.
static bool CopyFileTo(const char* from, const char* to) {
    if (strcmp(from, to) == 0) return true;
    FILE* a = fopen(from, "rb");
    if (a == nullptr) return false;
    FILE* b = fopen(to, "wb");
    if (b == nullptr) { fclose(a); return false; }

    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) {
        if (fwrite(buf, 1, n, b) != n) { ok = false; break; }   // disco lleno
    }
    if (ferror(a)) ok = false;
    // fclose puede fallar aunque los fwrite hayan ido bien: es donde se vacía
    // el búfer del sistema, y ahí es donde se entera uno de que no cabía.
    if (fclose(b) != 0) ok = false;
    fclose(a);
    if (!ok) remove(to);
    return ok;
}

// Refresca una textura de vista previa CON los píxeles nuevos en vez de
// destruirla y crear otra.
//
// Las vistas previas del teléfono llegan a 30 por segundo, y cada una hacía un
// UnloadTexture + LoadTextureFromImage: destruir y reservar memoria de GPU
// treinta veces por segundo, con el atasco de driver que eso trae. Mientras el
// tamaño no cambie —y no cambia, es la misma cámara— basta con reescribir los
// píxeles. Sólo se recrea si de verdad cambia el tamaño o el formato.
static void UpdatePreviewTex(Texture2D& tex, const Image& im) {
    if (tex.id != 0 && tex.width == im.width && tex.height == im.height &&
        tex.format == im.format) {
        UpdateTexture(tex, im.data);
        return;
    }
    if (tex.id != 0) UnloadTexture(tex);
    tex = LoadTextureFromImage(im);
}

#ifdef UI_SMOKE_TEST
// Contadores para MEDIR si saltarse las subidas repetidas sirve de algo, en vez
// de suponerlo. Sólo existen en el binario de pruebas.
static long long g_uploadsAsked = 0, g_uploadsDone = 0;
// Y cronómetros por fase, para saber DÓNDE se va el tiempo antes de optimizar
// nada. Sin esto, "optimizar" es tocar lo que a uno le suena caro.
static double g_tCollage = 0, g_tCapture = 0, g_tUi = 0, g_tPrevPhases = 0;
static long long g_collageAsked = 0, g_collageDrawn = 0;
// La MEDIA no sirve aquí: la prueba carga modelos, decodifica vídeos y llama a
// ffmpeg dentro del bucle, y esos poquísimos fotogramas de medio segundo se
// comen la media entera. Lo que dice cómo va el programa de verdad es la
// MEDIANA, o sea el fotograma corriente.
static std::vector<double> g_sCollage, g_sCapture, g_sFrame, g_s2D, g_s3D;
static double Median(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static int    g_frames = 0;
#endif

static bool MoveFileTo(const char* from, const char* to) {
    if (strcmp(from, to) == 0) return true;
    if (rename(from, to) == 0) return true;   // mismo disco: instantáneo
    // Distinto disco: copiar y borrar. El borrado va SÓLO si la copia salió
    // bien — antes se borraba pase lo que pase, así que un disco lleno a mitad
    // se llevaba por delante la grabación original. Silenciosamente.
    if (!CopyFileTo(from, to)) return false;
    remove(from);
    return true;
}

// ---------------------------------------------------------------------------
// ffmpeg: comprobarlo AL ARRANCAR y, en Windows, poder instalarlo desde aquí.
//
// Sin ffmpeg no se carga ningún vídeo que no sea .mpg y no se puede exportar
// nada. Hasta ahora eso sólo se notaba al intentar meter un vídeo y fallar, y
// el motivo se imprimía por una consola que en Windows NO EXISTE (se compila
// con -mwindows). Resultado: "arrastro archivos y no importa nada", sin más
// explicación. Ahora se avisa nada más abrir el programa.
//
// El botón de instalar sólo está en Windows y es DELIBERADAMENTE explícito: se
// descarga la compilación de gyan.dev que el propio LEEME ya recomienda, con
// curl.exe y tar.exe, que vienen con Windows 10 (1803) en adelante — o sea,
// sin instalar nada previo. En Linux/macOS se enseña el comando del gestor de
// paquetes, que es lo correcto ahí.
// ---------------------------------------------------------------------------
// (Sólo se usa en Windows: fuera de ahí ffmpeg viene del gestor de paquetes.)
[[maybe_unused]] static const char* kFfmpegUrl =
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";

static std::atomic<int>  g_ffmpegState{-1};   // -1 sin comprobar, 0 falta, 1 está
static std::atomic<bool> g_ffmpegBusy{false};
static std::mutex        g_ffmpegMtx;
static std::string       g_ffmpegNote;
static std::thread       g_ffmpegThread;

static void SetFfmpegNote(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_ffmpegMtx);
    g_ffmpegNote = s;
}
static std::string FfmpegNote() {
    std::lock_guard<std::mutex> lk(g_ffmpegMtx);
    return g_ffmpegNote;
}

static bool DetectFfmpeg() {
    std::string out;
#if defined(_WIN32)
    int rc = RunCommandRead("ffmpeg -version 2>nul", out);
#else
    int rc = RunCommandRead("ffmpeg -version 2>/dev/null", out);
#endif
    bool ok = (rc == 0) && out.find("ffmpeg version") != std::string::npos;
    g_ffmpegState = ok ? 1 : 0;
    return ok;
}

// El comando exacto de ESTA plataforma, para el botón de copiar. (En Windows
// no hay botón de copiar: hay uno que lo instala, así que aquí no se usa.)
[[maybe_unused]] static const char* FfmpegFixCommand() {
#if defined(_WIN32)
    return kFfmpegUrl;
#elif defined(__APPLE__)
    return "brew install ffmpeg";
#else
    return FileExists("/etc/debian_version") ? "sudo apt install ffmpeg"
                                             : "sudo pacman -S ffmpeg";
#endif
}

#if defined(_WIN32)
// Descarga y coloca ffmpeg.exe JUNTO al programa. En un hilo aparte: son unas
// decenas de MB y bloquear el bucle de dibujo dejaría la ventana congelada.
static void InstallFfmpegWindowsAsync() {
    if (g_ffmpegBusy) return;
    if (g_ffmpegThread.joinable()) g_ffmpegThread.join();
    g_ffmpegBusy = true;
    SetFfmpegNote("downloading ffmpeg... (this takes a minute)");
    g_ffmpegThread = std::thread([] {
        char cmd[1024];
        // tar.exe de Windows es bsdtar y descomprime .zip de sobra.
        snprintf(cmd, sizeof(cmd),
                 "curl -L --fail -o pinguus_ffmpeg.zip \"%s\" && "
                 "tar -xf pinguus_ffmpeg.zip && "
                 "for /d %%d in (ffmpeg-*-essentials_build) do "
                 "(copy /Y \"%%d\\bin\\ffmpeg.exe\" . >nul & "
                 "copy /Y \"%%d\\bin\\ffprobe.exe\" . >nul & rmdir /s /q \"%%d\")",
                 kFfmpegUrl);
        int rc = RunCommand(cmd);
        RunCommand("del /q pinguus_ffmpeg.zip 2>nul");
        if (rc == 0 && DetectFfmpeg()) SetFfmpegNote("ffmpeg installed - videos work now");
        else SetFfmpegNote("download failed - install it by hand (see LEEME-PRIMERO.txt)");
        g_ffmpegBusy = false;
    });
}
#endif

// ---------------------------------------------------------------------------
// Salida LIVE: memoria compartida entre el programa y una ventana espejo
// (segundo proceso del mismo binario con --live). Ideal para proyectar el
// collage en otro monitor mientras sigues trabajando.
// ---------------------------------------------------------------------------

struct LiveHeader {
    int magic;          // 0x50494E47 'PING'
    int w, h;
    unsigned int seq;   // impar = escribiendo; par = frame consistente
};
static const int kLiveMaxPixels = 1280 * 720; // igual en 720x1280
static const size_t kLiveShmSize = sizeof(LiveHeader) + (size_t)kLiveMaxPixels * 4;
static unsigned char* g_liveMem = nullptr;

static bool LiveShmOpen(bool create) {
    // El mapeo real vive en PlatformProc.cpp (necesita windows.h entero).
    g_liveMem = LiveShmMap(create, kLiveShmSize);
    return g_liveMem != nullptr;
}

static void LiveShmWrite(const void* rgba, int w, int h) {
    if (g_liveMem == nullptr) return;
    LiveHeader* hd = (LiveHeader*)g_liveMem;
    hd->seq |= 1;              // marcar "escribiendo"
    hd->magic = 0x50494E47;
    hd->w = w;
    hd->h = h;
    memcpy(g_liveMem + sizeof(LiveHeader), rgba, (size_t)w * h * 4);
    hd->seq = (hd->seq + 1) & ~1u; // frame consistente
}

// Proceso espejo: ventana que solo muestra el collage. Click = fullscreen.
static int RunLiveViewer() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(640, 360, "Pinguus LIVE (click: fullscreen)");
    SetTargetFPS(60);

    // Esperar a que el programa principal cree la memoria compartida.
    for (int tries = 0; tries < 100 && g_liveMem == nullptr; tries++) {
        if (!LiveShmOpen(false)) WaitTime(0.1);
    }

    Texture2D tex = {0};
    int tw = 0, th = 0;
    unsigned int lastSeq = 0xFFFFFFFF;
    std::vector<unsigned char> local((size_t)kLiveMaxPixels * 4);

    while (!WindowShouldClose()) {
        if (g_liveMem != nullptr) {
            LiveHeader* hd = (LiveHeader*)g_liveMem;
            if (hd->magic == 0x50494E47 && (hd->seq & 1u) == 0 && hd->seq != lastSeq &&
                hd->w > 0 && hd->h > 0 && hd->w * hd->h <= kLiveMaxPixels) {
                int w = hd->w, h = hd->h;
                memcpy(local.data(), g_liveMem + sizeof(LiveHeader), (size_t)w * h * 4);
                lastSeq = hd->seq;

                if (tex.id == 0 || tw != w || th != h) {
                    if (tex.id != 0) UnloadTexture(tex);
                    Image img = GenImageColor(w, h, BLACK); // RGBA, coincide con los datos
                    tex = LoadTextureFromImage(img);
                    UnloadImage(img);
                    tw = w;
                    th = h;
                }
                UpdateTexture(tex, local.data());
            }
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ToggleBorderlessWindowed();

        BeginDrawing();
        ClearBackground(BLACK);
        if (tex.id != 0) {
            float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
            float s = sw / tw;
            if (th * s > sh) s = sh / th;
            float dw = tw * s, dh = th * s;
            // El buffer viene de una RenderTexture (volteada): alto negativo.
            DrawTexturePro(tex, {0, 0, (float)tw, -(float)th},
                           {(sw - dw) / 2, (sh - dh) / 2, dw, dh}, {0, 0}, 0.0f, WHITE);
        } else {
            const char* msg = "Waiting for Pinguus LIVE feed...";
            DrawText(msg, (GetScreenWidth() - MeasureText(msg, 20)) / 2, GetScreenHeight() / 2 - 10, 20, GRAY);
        }
        EndDrawing();
    }

    if (tex.id != 0) UnloadTexture(tex);
    CloseWindow();
    return 0;
}

// ---------------------------------------------------------------------------
// Importación MIDI (SMF 0/1): mapea note-ons a las filas del tracker.
// ---------------------------------------------------------------------------

struct MidiNoteEv {
    int midiChannel;
    int row;
    int note;
};

static unsigned int MidiReadVarLen(const unsigned char* d, size_t n, size_t& pos) {
    unsigned int v = 0;
    while (pos < n) {
        unsigned char b = d[pos++];
        v = (v << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) break;
    }
    return v;
}

static bool ParseMidiFile(const char* path, std::vector<MidiNoteEv>& outNotes, float* outBpm) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 14 || size > 8 * 1024 * 1024) { fclose(f); return false; }
    std::vector<unsigned char> data((size_t)size);
    if (fread(data.data(), 1, (size_t)size, f) != (size_t)size) { fclose(f); return false; }
    fclose(f);

    auto u32 = [&](size_t p) { return ((unsigned)data[p] << 24) | ((unsigned)data[p + 1] << 16) | ((unsigned)data[p + 2] << 8) | data[p + 3]; };
    auto u16 = [&](size_t p) { return ((unsigned)data[p] << 8) | data[p + 1]; };

    if (memcmp(data.data(), "MThd", 4) != 0) return false;
    unsigned ntrks = u16(10);
    unsigned division = u16(12);
    if (division & 0x8000) return false; // formato SMPTE, no soportado
    if (division == 0) return false;

    size_t pos = 8 + u32(4);
    for (unsigned trk = 0; trk < ntrks && pos + 8 <= data.size(); trk++) {
        if (memcmp(&data[pos], "MTrk", 4) != 0) break;
        size_t trkLen = u32(pos + 4);
        size_t p = pos + 8;
        size_t end = p + trkLen;
        if (end > data.size()) end = data.size();

        unsigned long long absTick = 0;
        unsigned char running = 0;

        while (p < end) {
            absTick += MidiReadVarLen(data.data(), end, p);
            if (p >= end) break;
            unsigned char status = data[p];
            if (status & 0x80) { p++; }
            else { status = running; }
            if (status == 0) break;

            if (status == 0xFF) { // meta
                if (p >= end) break;
                unsigned char type = data[p++];
                unsigned int len = MidiReadVarLen(data.data(), end, p);
                if (type == 0x51 && len == 3 && p + 3 <= end && outBpm != nullptr) {
                    unsigned tempo = ((unsigned)data[p] << 16) | ((unsigned)data[p + 1] << 8) | data[p + 2];
                    if (tempo > 0) *outBpm = 60000000.0f / (float)tempo;
                }
                p += len;
                running = 0;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) { // sysex
                unsigned int len = MidiReadVarLen(data.data(), end, p);
                p += len;
                running = 0;
                continue;
            }

            running = status;
            unsigned char hi = status & 0xF0;
            int nData = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            if (p + nData > end) break;

            if (hi == 0x90 && data[p + 1] > 0) { // note-on con velocidad
                // fila = tick convertido a 16avos (division = ticks/negra)
                int row = (int)((absTick * 4 + division / 2) / division);
                if (row >= 0 && row < TRACKER_ROWS) {
                    outNotes.push_back({status & 0x0F, row, data[p]});
                }
            }
            p += nData;
        }
        pos = end;
    }
    return !outNotes.empty();
}

// ---------------------------------------------------------------------------
// Motor y colas
// ---------------------------------------------------------------------------

SequencerEngine g_engine;
CommandQueue<4096> g_commandQueue;

// Enlace con el TELÉFONO (cámara + micrófono por UDP). Ver PhoneLink.h y
// phone/pinguus_phone.py y la app Android. Escucha DESDE EL ARRANQUE para que
// la app del móvil pueda encontrarnos sola (panel DEV -> PHONE / CAMERA / MIC).
static PhoneLink g_phone;
static const int kPhonePort = 45813;

// Teléfono vía una APP EXISTENTE que sirve MJPEG por HTTP ("IP Webcam" y
// similares): no hace falta Termux ni scripts. Ver IpCamLink.h. Se autodetecta
// escaneando la red local desde el panel DEV -> PHONE.
// La app MJPEG tiene PRIORIDAD sobre el script UDP al grabar, porque no necesita
// nada instalado en el teléfono.
static IpCamLink g_ipcam;
static int g_ipcamPort = 8080;

// Reparte "Pinguus Cam" (android/build/PinguusCam.apk) por la red local, para
// que instalar la app sea "abre esta dirección en el móvil" en lugar de "busca
// el fichero y pásalo por cable". Ver ApkServer.h.
static ApkServer g_apkServer;
static const int  kApkPort = 45814;
static const char* kApkPath = "android/build/PinguusCam.apk";

// TAILSCALE: la vía recomendada para conectar el móvil. Ver Tailscale.h. Todo
// lo demás (servidor del APK, enlace UDP, app MJPEG) ya escucha en TODAS las
// interfaces, así que no hay que tocar ni un socket: basta con enseñar la
// dirección 100.x del tailnet en vez de la del Wi-Fi y el móvil puede estar
// donde sea. g_phoneRoute elige qué columna se dibuja (0 = Tailscale,
// 1 = Wi-Fi local) y se recuerda en controls.cfg.
static Tailscale g_tailscale;
static int  g_phoneRoute = 0;
// Las direcciones IP arrancan SIEMPRE ocultas y sólo se enseñan a petición: el
// panel se abre a menudo con la ventana compartida o grabándose, y la dirección
// de casa no tiene por qué acabar en un vídeo. No se guarda en disco a
// propósito — "mostrar" vale para este rato, no para siempre.
static bool g_showIp = false;
// Enseña una IP o la tapa, según g_showIp. Devuelve un buffer temporal de
// raylib (TextFormat), así que sirve para dibujar en el momento, no para
// guardar.
static const char* ShownIp(const std::string& ip) {
    if (ip.empty()) return "?";
    return g_showIp ? ip.c_str() : "***.***.***.***";
}
VisualSnapshotPublisher g_snapshotPublisher;
ScriptEngine g_scripts;         // mods en Lua (hilo principal)
std::vector<ModCellDef> g_modCells; // celdas personalizadas registradas por mods (Fase 2)

// ---------------------------------------------------------------------------
// Modelos 3D (.glb / .vrm) — hilo principal. Cada slot carga un Model de raylib
// con sus animaciones; una nota que use un "slot virtual" de modelo (id >=
// MODEL_SLOT_BASE) dispara la animación elegida, que se renderiza en el collage.
// ---------------------------------------------------------------------------
// Roles de hueso humanoide (VRM). Índices en ModelSlot::hbone.
enum HBone {
    HB_HIPS = 0, HB_SPINE, HB_CHEST, HB_NECK, HB_HEAD,
    HB_L_SHOULDER, HB_L_UPPERARM, HB_L_LOWERARM,
    HB_R_SHOULDER, HB_R_UPPERARM, HB_R_LOWERARM,
    HB_L_UPPERLEG, HB_L_LOWERLEG, HB_R_UPPERLEG, HB_R_LOWERLEG, HB_COUNT
};
// Un .vrma importado: por rol humanoide, los keyframes de rotación (local) + la
// rotación de reposo del nodo. Se retargetea a los huesos detectados del modelo.
struct VrmaTrack {
    bool has = false;
    Quaternion rest{0, 0, 0, 1};        // rotación LOCAL de reposo del nodo fuente
    Quaternion restGlobal{0, 0, 0, 1};  // rotación GLOBAL de reposo (cadena de padres)
    std::vector<float> times;
    std::vector<Quaternion> rots;
};
struct VrmaClip {
    std::string name;
    std::string path;    // para recargar con el proyecto
    float duration = 1.0f;
    bool  isBvh = false; // vino de un .bvh (mocap) en vez de un .vrma
    // Girar la animación 180° sobre Y al aplicarla. Los .vrma vienen siempre en
    // convención VRM 1.0 (mirando a +Z) y se corrigen solos, pero los .bvh de
    // mocap no tienen convención fija: según la herramienta que los exportó el
    // personaje mira a +Z o a -Z, y con la equivocada los brazos y piernas se
    // mueven al revés. Es un interruptor en el editor del modelo, no una
    // adivinanza: se ve al instante en la vista previa.
    bool  flipY = false;
    VrmaTrack track[HB_COUNT];
};

struct ModelSlot {
    bool loaded = false;
    Model model{};
    ModelAnimation* anims = nullptr;
    int animCount = 0;
    float scale = 1.0f;          // auto-ajuste para caber ~2 unidades
    Vector3 center{0, 0, 0};     // centro del bounding box
    std::string path;
    std::vector<std::string> animNames;
    // Transform editable por el usuario (editor de modelo, clic derecho).
    Vector3 pos{0, 0, 0};        // desplazamiento
    Vector3 rot{0, 0, 0};        // rotación en grados (X,Y,Z)
    float userScale = 1.0f;      // multiplicador sobre el auto-ajuste
    float baseYaw = 0.0f;        // orientación base (VRM 0.x mira a -Z -> 180°)
    bool toon = false;           // sombreado toon (cel) en vez de liso
    // --- Humanoide (VRM): huesos detectados por nombre + FK para animar el
    // esqueleto real de forma procedural (poses tipo anime sin archivos externos).
    bool humanoid = false;
    int hbone[16];               // índice de hueso por rol (HB_*), o -1
    std::vector<Transform> localBind; // transform local de reposo por hueso
    ModelAnimation procAnim{};   // 1 frame; se rellena con la pose calculada (FK)
    bool procAnimReady = false;
    std::vector<VrmaClip> clips; // animaciones .vrma importadas (retargeteadas)
    // TRIM de animación por anim (esquelética, humanoide, .vrma o procedural):
    // rango de frames [start, end) que se reproduce/loopea. end<=0 = hasta el
    // final. animLoop = loopear ese rango mientras la voz está activa (si no,
    // reproduce el rango UNA vez y se apaga, como un disparo normal).
    int  animTrimStart[MAX_MODEL_ANIMS] = {0};
    int  animTrimEnd[MAX_MODEL_ANIMS]   = {0}; // 0 = hasta el final
    bool animLoop[MAX_MODEL_ANIMS]      = {false};
};
static ModelSlot g_models[MAX_MODELS];

// Iluminación 3D global para los modelos (una luz direccional + ambiente).
static Shader g_lightShader{};
static Shader g_toonShader{};   // sombreado toon (cel) para el look anime
static int g_lightLocDir = -1, g_lightLocColor = -1, g_lightLocAmbient = -1, g_lightLocView = -1;
static int g_toonLocDir = -1, g_toonLocColor = -1, g_toonLocAmbient = -1, g_toonLocView = -1;
static Vector3 g_lightDir{-0.5f, -1.0f, -0.6f}; // dirección de la luz
static Color g_lightColor{255, 244, 224, 255};   // color de la luz
static float g_lightIntensity = 1.15f;
static float g_ambient = 0.35f;                   // luz ambiente (0..1)

struct ModelVoice {
    bool active = false;
    int id = -1;                 // bichoIndex estable del evento de video
    int model = -1, anim = -1;
    float frame = 0.0f;          // cursor de frame de la animación
};
static ModelVoice g_modelVoices[24];

static int modelAnimId(int model, int anim) { return MODEL_SLOT_BASE + model * MAX_MODEL_ANIMS + anim; }
static int modelOfId(int id) { return (id - MODEL_SLOT_BASE) / MAX_MODEL_ANIMS; }
static int animOfId(int id)  { return (id - MODEL_SLOT_BASE) % MAX_MODEL_ANIMS; }

// Animaciones PROCEDURALES (de transform), para modelos SIN animaciones
// esqueléticas propias — el caso típico de un .vrm. No tocan el esqueleto:
// mueven todo el modelo (posición/rotación/escala) en el tiempo, así cualquier
// personaje importado se puede animar y disparar con un notey.
static const char* kProcAnimNames[] = {"Spin", "Bounce", "Pulse", "Sway", "Hop", "Wobble"};
static const int kProcAnimCount = 6;
static const int kProcAnimFrames = 72; // ~1.2 s a 60 fps

static const char* kHumanoidAnimNames[] = {"Idle", "Wave", "Dance", "Nod", "March", "Cheer"};
static const int kHumanoidAnimCount = 6;
static const int kHumanoidAnimFrames = 120; // 2 s de loop

// Nombre humanoide VRMC por rol (para leer el mapa humanBones de un .vrma).
static const char* kHBoneVrmcName[HB_COUNT] = {
    "hips", "spine", "chest", "neck", "head",
    "leftShoulder", "leftUpperArm", "leftLowerArm",
    "rightShoulder", "rightUpperArm", "rightLowerArm",
    "leftUpperLeg", "leftLowerLeg", "rightUpperLeg", "rightLowerLeg"
};

static bool modelIsProcedural(const ModelSlot& s) { return s.loaded && s.animCount == 0; }
static bool modelIsHumanoid(const ModelSlot& s) { return s.loaded && s.animCount == 0 && s.humanoid; }
static int modelAnimTotal(const ModelSlot& s) {
    if (s.animCount > 0) return s.animCount;
    return s.humanoid ? (kHumanoidAnimCount + (int)s.clips.size()) : kProcAnimCount;
}

// Calcula el delta de transform de una animación procedural en progreso t (0..1).
static void ComputeProcAnim(int anim, float t, Vector3& pos, Vector3& rot, float& scaleMul) {
    pos = {0, 0, 0}; rot = {0, 0, 0}; scaleMul = 1.0f;
    const float PI2 = 6.2831853f;
    switch (anim) {
        case 0: rot.y = 360.0f * t; break;                          // Spin
        case 1: pos.y = sinf(t * 3.14159f) * 0.6f; break;           // Bounce
        case 2: scaleMul = 1.0f + sinf(t * 3.14159f) * 0.25f; break; // Pulse
        case 3: rot.z = sinf(t * PI2) * 20.0f; break;               // Sway
        case 4: pos.y = fabsf(sinf(t * PI2)) * 0.5f; break;         // Hop (x2)
        default: rot.x = sinf(t * PI2 * 1.5f) * 12.0f; break;       // Wobble
    }
}

static void UnloadModelSlot(int i) {
    if (i < 0 || i >= MAX_MODELS || !g_models[i].loaded) return;
    ModelSlot& s = g_models[i];
    if (s.procAnimReady && s.procAnim.framePoses) {
        RL_FREE(s.procAnim.framePoses[0]);
        RL_FREE(s.procAnim.framePoses);
    }
    if (s.anims) UnloadModelAnimations(s.anims, s.animCount);
    UnloadModel(s.model);
    s = ModelSlot();
}

// minúsculas + "contiene" (para casar nombres de huesos).
static bool nameHas(const char* n, const char* sub) {
    char low[80];
    int k = 0; for (; n[k] && k < 79; k++) low[k] = (char)tolower((unsigned char)n[k]); low[k] = 0;
    return strstr(low, sub) != nullptr;
}
static int roleOfBone(const ModelSlot& s, int bone);        // definidos más abajo
static Quaternion sampleTrack(const VrmaTrack& tr, float time);

// Huesos AUXILIARES (IK, twist, dedos, pechos...) que NO son el esqueleto
// humanoide principal — se saltan para no confundir la detección.
static bool isHelperBone(const char* n) {
    return nameHas(n, "cf_pv") || nameHas(n, "cf_d") || nameHas(n, "footik") || nameHas(n, "heelik") ||
           nameHas(n, "toepin") || nameHas(n, "toerotator") || nameHas(n, "footpin") || nameHas(n, "masterfoot") ||
           nameHas(n, "twist") || nameHas(n, "finger") || nameHas(n, "thumb") || nameHas(n, "bust") ||
           nameHas(n, "bnip") || nameHas(n, "_ik") || nameHas(n, "ik.") || nameHas(n, "headphone") || nameHas(n, "_acs_");
}
// Detecta huesos humanoides por convención de nombres. Soporta VRoid (J_Bip_*),
// Unity, Mixamo Y exports tipo Koikatsu ("Left arm"/"Left elbow"/"Left leg"/
// "Left knee"). Salta los huesos auxiliares/IK. Rellena s.hbone[] y marca
// s.humanoid si hay cadera + brazo + pierna.
static void detectHumanoid(ModelSlot& s) {
    for (int i = 0; i < HB_COUNT; i++) s.hbone[i] = -1;
    Model& m = s.model;
    for (int b = 0; b < m.boneCount; b++) {
        const char* n = m.bones[b].name;
        if (isHelperBone(n)) continue;
        bool L = nameHas(n, "left") || nameHas(n, "_l_") || nameHas(n, ":l") || nameHas(n, ".l");
        bool R = nameHas(n, "right") || nameHas(n, "_r_") || nameHas(n, ":r") || nameHas(n, ".r");
        auto set = [&](int role) { if (s.hbone[role] < 0) s.hbone[role] = b; };
        // Piernas antes que brazos; lo más específico primero.
        if (nameHas(n, "hips")) set(HB_HIPS);
        else if (nameHas(n, "upperleg") || nameHas(n, "upper leg") || nameHas(n, "upleg") || nameHas(n, "thigh") ||
                 (nameHas(n, "leg") && !nameHas(n, "lower") && !nameHas(n, "knee"))) set(L ? HB_L_UPPERLEG : R ? HB_R_UPPERLEG : HB_HIPS);
        else if (nameHas(n, "lowerleg") || nameHas(n, "lower leg") || nameHas(n, "knee") || nameHas(n, "shin") || nameHas(n, "calf"))
            set(L ? HB_L_LOWERLEG : R ? HB_R_LOWERLEG : HB_HIPS);
        else if (nameHas(n, "shoulder")) set(L ? HB_L_SHOULDER : HB_R_SHOULDER);
        else if (nameHas(n, "upperarm") || nameHas(n, "upper arm") ||
                 (nameHas(n, "arm") && !nameHas(n, "fore") && !nameHas(n, "lower"))) set(L ? HB_L_UPPERARM : HB_R_UPPERARM);
        else if (nameHas(n, "lowerarm") || nameHas(n, "lower arm") || nameHas(n, "forearm") || nameHas(n, "elbow"))
            set(L ? HB_L_LOWERARM : HB_R_LOWERARM);
        else if (nameHas(n, "head")) set(HB_HEAD);
        else if (nameHas(n, "neck")) set(HB_NECK);
        else if (nameHas(n, "chest")) set(HB_CHEST);
        else if (nameHas(n, "spine")) set(HB_SPINE);
    }
    s.humanoid = (s.hbone[HB_HIPS] >= 0 &&
                  (s.hbone[HB_L_UPPERARM] >= 0 || s.hbone[HB_R_UPPERARM] >= 0) &&
                  (s.hbone[HB_L_UPPERLEG] >= 0 || s.hbone[HB_R_UPPERLEG] >= 0));
}
// Precalcula el transform LOCAL de reposo de cada hueso y reserva la pose FK.
static void buildHumanoidRig(ModelSlot& s) {
    Model& m = s.model;
    s.localBind.assign(m.boneCount, Transform{});
    for (int i = 0; i < m.boneCount; i++) {
        Transform g = m.bindPose[i];
        int p = m.bones[i].parent;
        if (p < 0 || p >= m.boneCount) { g.scale = {1, 1, 1}; s.localBind[i] = g; continue; }
        Transform gp = m.bindPose[p];
        Quaternion invPR = QuaternionInvert(gp.rotation);
        Transform l;
        l.rotation = QuaternionMultiply(invPR, g.rotation);
        l.translation = Vector3RotateByQuaternion(Vector3Subtract(g.translation, gp.translation), invPR);
        l.scale = {1, 1, 1};
        s.localBind[i] = l;
    }
    s.procAnim.boneCount = m.boneCount;
    s.procAnim.frameCount = 1;
    s.procAnim.bones = m.bones; // comparte punteros (no liberar aquí)
    s.procAnim.framePoses = (Transform**)RL_MALLOC(sizeof(Transform*));
    s.procAnim.framePoses[0] = (Transform*)RL_MALLOC(sizeof(Transform) * m.boneCount);
    s.procAnimReady = true;
}
static Quaternion axisDeg(float x, float y, float z, float deg) {
    return QuaternionFromAxisAngle({x, y, z}, deg * DEG2RAD);
}
// Rotación local extra de un hueso, según la animación humanoide y el tiempo t.
static Quaternion humanoidDelta(ModelSlot& s, int bone, int anim, float t) {
    int role = -1;
    for (int r = 0; r < HB_COUNT; r++) if (s.hbone[r] == bone) { role = r; break; }
    if (role < 0) return QuaternionIdentity();
    float ph = t * 6.2831853f;              // fase del loop
    switch (anim) {
        case 0: // Idle: respiración/vaivén suave
            if (role == HB_SPINE) return axisDeg(1, 0, 0, sinf(ph) * 3.0f);
            if (role == HB_HEAD)  return axisDeg(0, 1, 0, sinf(ph * 0.5f) * 6.0f);
            if (role == HB_L_UPPERARM) return axisDeg(0, 0, 1, sinf(ph) * 4.0f);
            if (role == HB_R_UPPERARM) return axisDeg(0, 0, 1, -sinf(ph) * 4.0f);
            break;
        case 1: // Wave: brazo derecho arriba, antebrazo saluda
            if (role == HB_R_UPPERARM) return axisDeg(0, 0, 1, 120.0f);
            if (role == HB_R_LOWERARM) return axisDeg(0, 1, 0, sinf(ph * 2.0f) * 35.0f);
            break;
        case 2: // Dance: caderas y brazos alternos
            if (role == HB_HIPS) return axisDeg(0, 1, 0, sinf(ph) * 18.0f);
            if (role == HB_SPINE) return axisDeg(0, 0, 1, sinf(ph) * 12.0f);
            if (role == HB_L_UPPERARM) return axisDeg(0, 0, 1, 60.0f + sinf(ph) * 30.0f);
            if (role == HB_R_UPPERARM) return axisDeg(0, 0, 1, -60.0f - sinf(ph) * 30.0f);
            break;
        case 3: // Nod: cabeza asiente
            if (role == HB_HEAD) return axisDeg(1, 0, 0, sinf(ph) * 18.0f);
            if (role == HB_NECK) return axisDeg(1, 0, 0, sinf(ph) * 8.0f);
            break;
        case 4: // March: piernas y brazos alternos
            if (role == HB_L_UPPERLEG) return axisDeg(1, 0, 0, sinf(ph) * 35.0f);
            if (role == HB_R_UPPERLEG) return axisDeg(1, 0, 0, -sinf(ph) * 35.0f);
            if (role == HB_L_UPPERARM) return axisDeg(1, 0, 0, -sinf(ph) * 30.0f);
            if (role == HB_R_UPPERARM) return axisDeg(1, 0, 0, sinf(ph) * 30.0f);
            break;
        default: // Cheer: ambos brazos arriba, rebote
            if (role == HB_L_UPPERARM) return axisDeg(0, 0, 1, 150.0f + sinf(ph * 2.0f) * 10.0f);
            if (role == HB_R_UPPERARM) return axisDeg(0, 0, 1, -150.0f - sinf(ph * 2.0f) * 10.0f);
            if (role == HB_SPINE) return axisDeg(1, 0, 0, fabsf(sinf(ph)) * 6.0f);
            break;
    }
    return QuaternionIdentity();
}
// Cinemática directa: calcula la pose global de cada hueso y la deja en procAnim.
// anim < kHumanoidAnimCount = animación procedural; si no, un .vrma importado.
static void computeHumanoidPose(ModelSlot& s, int anim, float t) {
    if (!s.procAnimReady) return;
    Model& m = s.model;
    const VrmaClip* clip = nullptr;
    if (anim >= kHumanoidAnimCount) {
        int ci = anim - kHumanoidAnimCount;
        if (ci >= 0 && ci < (int)s.clips.size()) clip = &s.clips[ci];
    }
    static std::vector<Transform> global;
    global.assign(m.boneCount, Transform{});
    for (int i = 0; i < m.boneCount; i++) {
        Transform l = s.localBind[i];
        Quaternion delta;
        if (clip) {
            int role = roleOfBone(s, i);
            if (role >= 0 && clip->track[role].has) {
                const VrmaTrack& tr = clip->track[role];
                // 1) Delta en el marco LOCAL del hueso fuente (relativo a su reposo).
                Quaternion localDelta = QuaternionMultiply(QuaternionInvert(tr.rest), sampleTrack(tr, t * clip->duration));
                // 2) Pásalo al espacio MUNDO del rig fuente (conjugando por su reposo global).
                Quaternion worldDelta = QuaternionMultiply(QuaternionMultiply(tr.restGlobal, localDelta), QuaternionInvert(tr.restGlobal));
                // 3) Los .vrma se crean en convención VRM 1.0 (mira a +Z). Si el modelo
                //    destino es VRM 0.x (mira a -Z, baseYaw=180), la animación va rotada
                //    180° sobre Y: hay que corregirla o los brazos/piernas van al revés.
                //    Conjugar por Ry180 equivale a negar las componentes x y z.
                //    Un .bvh puede venir ya en la otra convención (flipY del clip), y
                //    entonces las dos vueltas se cancelan: por eso es un XOR y no un "o".
                if ((s.baseYaw != 0.0f) != clip->flipY)
                    worldDelta = (Quaternion){-worldDelta.x, worldDelta.y, -worldDelta.z, worldDelta.w};
                // 4) Llévalo al marco LOCAL del hueso DESTINO (su reposo global del bind).
                Quaternion grTgt = m.bindPose[i].rotation;
                delta = QuaternionMultiply(QuaternionMultiply(QuaternionInvert(grTgt), worldDelta), grTgt);
            }
            else delta = QuaternionIdentity();
        } else {
            delta = humanoidDelta(s, i, anim, t);
        }
        l.rotation = QuaternionMultiply(l.rotation, delta);
        int p = m.bones[i].parent;
        Transform g;
        if (p < 0 || p >= i) { g = l; }
        else {
            Transform gp = global[p];
            g.rotation = QuaternionMultiply(gp.rotation, l.rotation);
            g.translation = Vector3Add(gp.translation, Vector3RotateByQuaternion(l.translation, gp.rotation));
            g.scale = {1, 1, 1};
        }
        global[i] = g;
        s.procAnim.framePoses[0][i] = g;
    }
}

// Nº de frames de una animación (esquelética real, humanoide, .vrma o transform).
static int modelAnimFrames(const ModelSlot& s, int anim) {
    if (s.animCount > 0) return (anim >= 0 && anim < s.animCount) ? s.anims[anim].frameCount : 1;
    if (s.humanoid) {
        if (anim >= kHumanoidAnimCount) {
            int ci = anim - kHumanoidAnimCount;
            if (ci >= 0 && ci < (int)s.clips.size()) { int f = (int)(s.clips[ci].duration * 60.0f); return f > 1 ? f : 1; }
        }
        return kHumanoidAnimFrames;
    }
    return kProcAnimFrames;
}
// Frame inicial/final EFECTIVOS del trim de una animación (con clamps). El rango
// reproducido/looped es [start, end). end<=0 o fuera de rango = hasta el final.
static int animTrimStartF(const ModelSlot& s, int anim) {
    int fc = modelAnimFrames(s, anim);
    if (anim < 0 || anim >= MAX_MODEL_ANIMS || fc <= 1) return 0;
    int st = s.animTrimStart[anim];
    if (st < 0) st = 0;
    if (st > fc - 1) st = fc - 1;
    return st;
}
static int animTrimEndF(const ModelSlot& s, int anim) {
    int fc = modelAnimFrames(s, anim);
    if (anim < 0 || anim >= MAX_MODEL_ANIMS) return fc;
    int en = s.animTrimEnd[anim];
    if (en <= 0 || en > fc) en = fc;
    int st = animTrimStartF(s, anim);
    if (en <= st) en = fc;         // rango inválido -> al final
    return en;
}
// Prepara el modelo para dibujar la animación 'anim' en 'frame': actualiza el
// esqueleto (esquelética o humanoide) y devuelve el delta de transform (para
// las animaciones procedurales de transform).
static void PoseModel(ModelSlot& s, int anim, float frame, Vector3& pd, Vector3& rd, float& sd) {
    pd = {0, 0, 0}; rd = {0, 0, 0}; sd = 1.0f;
    if (s.animCount > 0) {
        if (anim >= 0 && anim < s.animCount) UpdateModelAnimation(s.model, s.anims[anim], (int)frame);
    } else if (s.humanoid && s.procAnimReady) {
        int fr = modelAnimFrames(s, anim);
        computeHumanoidPose(s, anim, fr > 0 ? frame / (float)fr : 0.0f);
        UpdateModelAnimation(s.model, s.procAnim, 0);
    } else {
        ComputeProcAnim(anim, frame / (float)kProcAnimFrames, pd, rd, sd);
    }
}

static void ApplyModelShader(ModelSlot& s); // definido más abajo

// Versión VRM del archivo: 0 = no-VRM, 1 = VRM 0.x, 2 = VRM 1.0. Los VRM 0.x
// miran hacia -Z (salen de espaldas con nuestra cámara) -> se rotan 180°.
static int vrmVersion(const char* path) {
    cgltf_options o = {};
    cgltf_data* d = nullptr;
    if (cgltf_parse_file(&o, path, &d) != cgltf_result_success) return 0;
    int v = 0;
    for (size_t i = 0; i < d->data_extensions_count; i++) {
        const char* n = d->data_extensions[i].name;
        if (n && strcmp(n, "VRMC_vrm") == 0) v = 2;
        else if (n && strcmp(n, "VRM") == 0 && v == 0) v = 1;
    }
    cgltf_free(d);
    return v;
}

// Rol humanoide (HB_*) de un hueso del modelo, o -1.
static int roleOfBone(const ModelSlot& s, int bone) {
    for (int r = 0; r < HB_COUNT; r++) if (s.hbone[r] == bone) return r;
    return -1;
}
// Muestrea una pista de rotación en 'time' (slerp entre keyframes).
static Quaternion sampleTrack(const VrmaTrack& tr, float time) {
    if (!tr.has || tr.times.empty()) return QuaternionIdentity();
    if (time <= tr.times.front()) return tr.rots.front();
    if (time >= tr.times.back()) return tr.rots.back();
    for (size_t i = 1; i < tr.times.size(); i++)
        if (time < tr.times[i]) {
            float a = (time - tr.times[i - 1]) / (tr.times[i] - tr.times[i - 1]);
            return QuaternionSlerp(tr.rots[i - 1], tr.rots[i], a);
        }
    return tr.rots.back();
}
// Busca el nodo de un hueso humanoide en el JSON de humanBones del .vrma.
static int vrmaNodeFor(const char* json, const char* name) {
    char key[80];
    snprintf(key, sizeof(key), "\"%s\":{\"node\":", name);
    const char* p = strstr(json, key);
    if (!p) return -1;
    return atoi(p + strlen(key));
}
// Carga un .vrma (animación VRM) y lo retargetea a los roles humanoides: por
// cada rol, toma la pista de rotación del nodo mapeado en humanBones. Devuelve
// false si no es un .vrma humanoide válido.
static bool loadVrmaClip(const char* path, VrmaClip& clip) {
    cgltf_options opt = {};
    cgltf_data* d = nullptr;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opt, d, path) != cgltf_result_success) { cgltf_free(d); return false; }
    const char* ext = nullptr;
    for (size_t i = 0; i < d->data_extensions_count; i++)
        if (d->data_extensions[i].name && strcmp(d->data_extensions[i].name, "VRMC_vrm_animation") == 0) ext = d->data_extensions[i].data;
    if (!ext || d->animations_count == 0) { cgltf_free(d); return false; }
    cgltf_animation* a = &d->animations[0];
    clip = VrmaClip();
    float dur = 0.0f;
    for (int role = 0; role < HB_COUNT; role++) {
        int nodeIdx = vrmaNodeFor(ext, kHBoneVrmcName[role]);
        if (nodeIdx < 0 || nodeIdx >= (int)d->nodes_count) continue;
        cgltf_node* node = &d->nodes[nodeIdx];
        Quaternion rest = node->has_rotation ? (Quaternion){node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]} : QuaternionIdentity();
        // Rotación GLOBAL de reposo del nodo = producto de las rotaciones locales
        // de reposo de toda su cadena de padres (raíz→nodo). Necesaria para pasar
        // el delta de la animación al ESPACIO MUNDO antes de retargetearlo.
        Quaternion restGlobal = QuaternionIdentity();
        for (cgltf_node* nn = node; nn; nn = nn->parent) {
            Quaternion lr = nn->has_rotation ? (Quaternion){nn->rotation[0], nn->rotation[1], nn->rotation[2], nn->rotation[3]} : QuaternionIdentity();
            restGlobal = QuaternionMultiply(lr, restGlobal);
        }
        for (size_t c = 0; c < a->channels_count; c++) {
            cgltf_animation_channel* ch = &a->channels[c];
            if (ch->target_node != node || ch->target_path != cgltf_animation_path_type_rotation) continue;
            cgltf_accessor* in = ch->sampler->input;
            cgltf_accessor* out = ch->sampler->output;
            int nf = (int)in->count;
            VrmaTrack& tr = clip.track[role];
            tr.has = true; tr.rest = rest; tr.restGlobal = restGlobal;
            tr.times.resize(nf); tr.rots.resize(nf);
            for (int f = 0; f < nf; f++) {
                float t = 0; cgltf_accessor_read_float(in, (cgltf_size)f, &t, 1);
                float q[4] = {0, 0, 0, 1}; cgltf_accessor_read_float(out, (cgltf_size)f, q, 4);
                tr.times[f] = t; tr.rots[f] = {q[0], q[1], q[2], q[3]};
                if (t > dur) dur = t;
            }
            break;
        }
    }
    clip.duration = dur > 0.01f ? dur : 1.0f;
    clip.name = GetFileNameWithoutExt(path);
    clip.path = path;
    cgltf_free(d);
    int have = 0; for (int r = 0; r < HB_COUNT; r++) if (clip.track[r].has) have++;
    return have >= 3; // al menos algunas pistas
}

// ---------------------------------------------------------------------------
// .BVH (captura de movimiento) — Mixamo, CMU, Rokoko, Blender, Perception
// Neuron... Es el formato que de verdad EXISTE en la calle: casi nadie exporta
// .vrma, pero animaciones .bvh gratis hay a millares. Se retargetea por la
// misma tubería que un .vrma; lo único propio es traducir el NOMBRE de cada
// joint a un rol humanoide.
// ---------------------------------------------------------------------------
// Normaliza un nombre de joint: minúsculas, sin el prefijo del rig
// ("mixamorig:Hips" -> "hips") y sin separadores ("Left arm" -> "leftarm").
static std::string bvhNormalize(const std::string& raw) {
    std::string s = raw;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    std::string out;
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-' || c == '.') continue;
        out += (char)tolower((unsigned char)c);
    }
    return out;
}
// Rol humanoide de un joint BVH, o -1 si no nos sirve (manos, pies, dedos...).
// OJO con la convención de nombres de mocap, que NO es la de VRM: ahí
// "LeftArm" es el brazo SUPERIOR y "LeftLeg" es la PANTORRILLA. Por eso esto
// no reutiliza detectHumanoid(), que trabaja sobre nombres de VRM/Unity.
static int bvhRoleOfJoint(const std::string& raw) {
    std::string n = bvhNormalize(raw);
    if (n.empty()) return -1;
    auto has = [&](const char* sub) { return n.find(sub) != std::string::npos; };
    // Dedos y auxiliares fuera antes que nada (hay rigs con "LeftHandThumb1").
    if (has("finger") || has("thumb") || has("index") || has("middle") ||
        has("ring") || has("pinky") || has("hand") || has("foot") || has("toe") ||
        has("eye") || has("jaw") || has("breast") || has("twist") || has("ik"))
        return -1;

    // Lado: "Left"/"Right" (Mixamo, CMU, Neuron) o el sufijo .L/.R/_l/_r de
    // Blender, que hay que mirar en el nombre SIN normalizar porque justo lo
    // que lo distingue son los separadores.
    bool L = has("left"), R = has("right");
    if (!L && !R) {
        std::string s = raw;
        size_t colon = s.rfind(':');
        if (colon != std::string::npos) s = s.substr(colon + 1);
        for (char& c : s) c = (char)tolower((unsigned char)c);
        if (s.size() >= 2) {
            std::string tail = s.substr(s.size() - 2);
            if (tail == ".l" || tail == "_l") L = true;
            else if (tail == ".r" || tail == "_r") R = true;
        }
    }
    if (has("hips") || n == "pelvis" || n == "root") return HB_HIPS;
    if (has("upperchest")) return HB_CHEST;
    if (has("chest") || n == "spine1" || n == "spine2") return HB_CHEST;
    if (has("spine")) return HB_SPINE;
    if (has("neck")) return HB_NECK;
    if (has("head")) return HB_HEAD;
    if (has("shoulder") || has("clavicle")) return L ? HB_L_SHOULDER : R ? HB_R_SHOULDER : -1;
    if (has("forearm") || has("lowerarm") || has("elbow")) return L ? HB_L_LOWERARM : R ? HB_R_LOWERARM : -1;
    if (has("upperarm") || has("arm")) return L ? HB_L_UPPERARM : R ? HB_R_UPPERARM : -1;
    if (has("upleg") || has("upperleg") || has("thigh")) return L ? HB_L_UPPERLEG : R ? HB_R_UPPERLEG : -1;
    if (has("lowerleg") || has("knee") || has("shin") || has("calf") || has("leg"))
        return L ? HB_L_LOWERLEG : R ? HB_R_LOWERLEG : -1;
    return -1;
}
// Carga un .bvh y lo deja listo para el mismo retargeting que un .vrma. En un
// BVH la postura de reposo es "todas las rotaciones a cero" (la pose la definen
// los OFFSET), así que las rotaciones de reposo local y global son la identidad
// — justo el caso que la tubería de .vrma ya trata.
static bool loadBvhClipAsHumanoid(const char* path, VrmaClip& clip) {
    BvhClip bc;
    if (!LoadBvhClip(path, bc)) return false;
    clip = VrmaClip();
    clip.isBvh = true;
    for (size_t j = 0; j < bc.joints.size(); j++) {
        const BvhJoint& bj = bc.joints[j];
        int role = bvhRoleOfJoint(bj.name);
        if (role < 0 || role >= HB_COUNT) continue;
        if (clip.track[role].has) continue;      // el primero que casa se queda el rol
        if (bj.rot.empty()) continue;
        VrmaTrack& tr = clip.track[role];
        tr.has = true;
        tr.rest = QuaternionIdentity();
        tr.restGlobal = QuaternionIdentity();
        int nf = (int)bj.rot.size();
        tr.times.resize(nf);
        tr.rots.resize(nf);
        for (int f = 0; f < nf; f++) {
            tr.times[f] = f * bc.frameTime;
            tr.rots[f] = (Quaternion){bj.rot[f].x, bj.rot[f].y, bj.rot[f].z, bj.rot[f].w};
        }
    }
    clip.duration = bc.duration();
    clip.name = GetFileNameWithoutExt(path);
    clip.path = path;
    int have = 0; for (int r = 0; r < HB_COUNT; r++) if (clip.track[r].has) have++;
    return have >= 3;
}
// Importa una animación humanoide de cualquiera de los dos formatos.
static bool loadHumanoidAnimFile(const char* path, VrmaClip& clip) {
    if (IsFileExtension(path, ".bvh")) return loadBvhClipAsHumanoid(path, clip);
    return loadVrmaClip(path, clip);
}

// Carga un .glb/.vrm (best-effort: un .vrm se carga como glTF — malla+texturas,
// sin lo específico de VRM). Auto-ajusta escala/centro por su bounding box.
static bool LoadModelSlot(int i, const char* path) {
    if (i < 0 || i >= MAX_MODELS) return false;
    UnloadModelSlot(i);
    // Un .vrm es un glTF binario (.glb) con extensiones de avatar. raylib elige
    // el cargador por la EXTENSIÓN, así que un .vrm hay que cargarlo como .glb:
    // copiamos los bytes a un archivo temporal .glb y cargamos ese.
    std::string tmpGlb;
    const char* loadPath = path;
    if (IsFileExtension(path, ".vrm")) {
        int sz = 0;
        unsigned char* data = LoadFileData(path, &sz);
        if (data && sz > 0) {
            tmpGlb = std::string(path) + ".tmp.glb";
            SaveFileData(tmpGlb.c_str(), data, sz);
            loadPath = tmpGlb.c_str();
        }
        if (data) UnloadFileData(data);
    }
    Model m = LoadModel(loadPath);
    if (m.meshCount == 0) { UnloadModel(m); if (!tmpGlb.empty()) remove(tmpGlb.c_str()); return false; }
    ModelSlot& s = g_models[i];
    s.model = m;
    s.loaded = true;              // para que ApplyModelShader opere sobre este slot
    ApplyModelShader(s);          // shader de iluminación (liso por defecto)
    int cnt = 0;
    s.anims = LoadModelAnimations(loadPath, &cnt);
    s.animCount = cnt;
    if (!tmpGlb.empty()) remove(tmpGlb.c_str());
    for (int a = 0; a < cnt && a < MAX_MODEL_ANIMS; a++) {
        const char* nm = s.anims[a].name;
        s.animNames.push_back((nm && nm[0]) ? nm : TextFormat("anim %d", a));
    }
    if (s.animCount > MAX_MODEL_ANIMS) s.animCount = MAX_MODEL_ANIMS;
    // Sin animaciones esqueléticas (típico en .vrm): si el rig es humanoide,
    // ofrece animaciones humanoides procedurales (mueven el esqueleto real); si
    // no, animaciones de transform.
    if (s.animCount == 0) {
        detectHumanoid(s);
        if (s.humanoid) {
            buildHumanoidRig(s);
            for (int a = 0; a < kHumanoidAnimCount; a++) s.animNames.push_back(kHumanoidAnimNames[a]);
        } else {
            for (int a = 0; a < kProcAnimCount; a++) s.animNames.push_back(kProcAnimNames[a]);
        }
    }
    BoundingBox bb = GetModelBoundingBox(m);
    Vector3 sz = {bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z};
    float maxd = fmaxf(sz.x, fmaxf(sz.y, sz.z));
    s.scale = maxd > 0.0001f ? 2.0f / maxd : 1.0f;
    s.center = {(bb.min.x + bb.max.x) * 0.5f, (bb.min.y + bb.max.y) * 0.5f, (bb.min.z + bb.max.z) * 0.5f};
    s.loaded = true;
    s.path = path;
    // VRM 0.x mira a -Z: se orienta 180° para quedar de frente a la cámara.
    if (vrmVersion(path) == 1) s.baseYaw = 180.0f;
    return true;
}

// Fija los uniformes de luz en AMBOS shaders (liso y toon).
static void SetLightUniforms(Vector3 viewPos) {
    Vector3 d = g_lightDir;
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 0.0001f) { d.x /= len; d.y /= len; d.z /= len; }
    float ld[3] = {d.x, d.y, d.z};
    float lc[3] = {g_lightColor.r / 255.0f * g_lightIntensity, g_lightColor.g / 255.0f * g_lightIntensity, g_lightColor.b / 255.0f * g_lightIntensity};
    float amb[3] = {g_ambient, g_ambient, g_ambient};
    float vp[3] = {viewPos.x, viewPos.y, viewPos.z};
    if (g_lightShader.id != 0) {
        if (g_lightLocDir >= 0) SetShaderValue(g_lightShader, g_lightLocDir, ld, SHADER_UNIFORM_VEC3);
        if (g_lightLocColor >= 0) SetShaderValue(g_lightShader, g_lightLocColor, lc, SHADER_UNIFORM_VEC3);
        if (g_lightLocAmbient >= 0) SetShaderValue(g_lightShader, g_lightLocAmbient, amb, SHADER_UNIFORM_VEC3);
        if (g_lightLocView >= 0) SetShaderValue(g_lightShader, g_lightLocView, vp, SHADER_UNIFORM_VEC3);
    }
    if (g_toonShader.id != 0) {
        if (g_toonLocDir >= 0) SetShaderValue(g_toonShader, g_toonLocDir, ld, SHADER_UNIFORM_VEC3);
        if (g_toonLocColor >= 0) SetShaderValue(g_toonShader, g_toonLocColor, lc, SHADER_UNIFORM_VEC3);
        if (g_toonLocAmbient >= 0) SetShaderValue(g_toonShader, g_toonLocAmbient, amb, SHADER_UNIFORM_VEC3);
        if (g_toonLocView >= 0) SetShaderValue(g_toonShader, g_toonLocView, vp, SHADER_UNIFORM_VEC3);
    }
}

// Asigna a los materiales del modelo el shader liso o toon según s.toon.
static void ApplyModelShader(ModelSlot& s) {
    Shader sh = s.toon ? g_toonShader : g_lightShader;
    if (sh.id == 0) return;
    for (int mm = 0; mm < s.model.materialCount; mm++) s.model.materials[mm].shader = sh;
}

// Dibuja un modelo aplicando su transform (usuario) + un delta procedural
// (posición/rotación/escala) sobre el auto-ajuste. offsetX desplaza horizontal.
static void DrawModelSlotAt(ModelSlot& s, float offsetX, Vector3 pd = {0, 0, 0}, Vector3 rd = {0, 0, 0}, float sd = 1.0f) {
    float scl = s.scale * s.userScale * sd;
    s.model.transform = MatrixRotateXYZ({DEG2RAD * (s.rot.x + rd.x), DEG2RAD * (s.rot.y + rd.y + s.baseYaw), DEG2RAD * (s.rot.z + rd.z)});
    Vector3 pos = {offsetX + s.pos.x + pd.x - s.center.x * scl, s.pos.y + pd.y - s.center.y * scl + 0.9f, s.pos.z + pd.z - s.center.z * scl};
    DrawModelEx(s.model, pos, {0.0f, 1.0f, 0.0f}, 0.0f, {scl, scl, scl}, WHITE);
}
VideoEventQueue<256> g_videoEventQueue;
VideoVoicePool g_videoVoicePool;

RtMidiOut* g_midiOut = nullptr;
RtMidiIn* g_midiIn = nullptr; // teclado/controlador MIDI de ENTRADA (opcional)

ma_encoder g_masterRecorder;
bool g_isRecording = false;

// Master output volume (0..1.5). Applied to the whole mix in the audio
// callback BEFORE recording, so both live sound and the recorded audio/video
// include it. A plain atomic float — set from the UI, read by the audio thread.
std::atomic<float> g_masterVol{1.0f};

void OnSequencerMidiNote(unsigned char note, unsigned char velocity, void* userData) {
    RtMidiOut* midi = (RtMidiOut*)userData;
    if (midi == nullptr) return;
    std::vector<unsigned char> msg = {144, note, velocity};
    midi->sendMessage(&msg);
}

void MiniaudioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;

    g_engine.drainCommands(g_commandQueue);
    g_engine.advance(frameCount);
    g_engine.renderAudio((float*)pOutput, frameCount, pDevice->playback.channels);
    g_engine.publishSnapshot(g_snapshotPublisher);
    g_engine.publishVideoEvents(g_videoEventQueue);

    // Master volume — applied before recording so the WAV/MP4 match what you hear.
    float mv = g_masterVol.load(std::memory_order_relaxed);
    if (mv != 1.0f) {
        float* out = (float*)pOutput;
        ma_uint32 n = frameCount * pDevice->playback.channels;
        for (ma_uint32 i = 0; i < n; i++) out[i] *= mv;
    }

    if (g_isRecording) {
        ma_encoder_write_pcm_frames(&g_masterRecorder, pOutput, frameCount, NULL);
    }
}

// ---------------------------------------------------------------------------
// Comandos hacia el hilo de audio
// ---------------------------------------------------------------------------

void RequestSpawnBicho(int x, int y, int dx, int dy, int sampleId, float tempoMul, bool muted, float volume, bool stopped = false) {
    Command cmd;
    cmd.type = CommandType::SpawnBicho;
    cmd.spawnX = x;
    cmd.spawnY = y;
    cmd.spawnDx = dx;
    cmd.spawnDy = dy;
    cmd.sampleId = sampleId;
    cmd.spawnTempoMul = tempoMul;
    cmd.mutedFlag = muted ? 1 : 0;
    cmd.stoppedFlag = stopped ? 1 : 0;
    cmd.volumeVal = volume;
    g_commandQueue.push(cmd);
}

void RequestSetBichoMuted(int poolIndex, bool muted) {
    Command cmd;
    cmd.type = CommandType::SetBichoMuted;
    cmd.bichoIndex = poolIndex;
    cmd.mutedFlag = muted ? 1 : 0;
    g_commandQueue.push(cmd);
}

void RequestSetBichoStopped(int poolIndex, bool stopped) {
    Command cmd;
    cmd.type = CommandType::SetBichoStopped;
    cmd.bichoIndex = poolIndex;
    cmd.stoppedFlag = stopped ? 1 : 0;
    g_commandQueue.push(cmd);
}

// Toca un slot en una voz "en vivo". colorId ya es el tono codificado
// (SemitoneToColorId del semitono); fx = 0/1/2/3/4 como en el tracker.
void RequestTriggerLive(int slot, unsigned char colorId, int fx) {
    Command cmd;
    cmd.type = CommandType::TriggerLive;
    cmd.sampleId = slot;
    cmd.colorId = colorId;
    cmd.modifier = fx;
    g_commandQueue.push(cmd);
}

// Igual, pero en la voz dedicada del Notey Controlable: re-dispara su mismo
// video (retrigger) en vez de multiplicarlo.
void RequestTriggerControllable(int slot, unsigned char colorId, int fx) {
    Command cmd;
    cmd.type = CommandType::TriggerControllable;
    cmd.sampleId = slot;
    cmd.colorId = colorId;
    cmd.modifier = fx;
    g_commandQueue.push(cmd);
}

// ---------------------------------------------------------------------------
// Modo BEATBOX (sampleadora de pads, estilo Roland SP-555)
//
// El hilo principal guarda su PROPIA copia de la configuración de los 64 pads
// (para dibujarla y para guardarla en el proyecto) y se la manda al motor con
// SetPadConfig. El patrón, en cambio, NO se duplica: lo graba el motor con su
// reloj de muestras y vuelve dentro de la foto — así lo que se graba es lo que
// se oyó, sin el desfase de un fotograma de dibujo.
// ---------------------------------------------------------------------------
static PadConfig g_pads[PAD_TOTAL];

// El TECLADO NUMÉRICO ya ES una rejilla de 4x4 — por eso es el mapeo por
// defecto y no una fila de letras: los dedos encuentran el pad sin mirar,
// que es la mitad de la gracia de una sampleadora.
//
//      7  8  9  -          pads  0  1  2  3
//      4  5  6  +                4  5  6  7
//      1  2  3 Ent                8  9 10 11
//      0  . /  *                 12 13 14 15
static const int kPadKeys[PAD_PER_BANK] = {
    KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_SUBTRACT,
    KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_ADD,
    KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_ENTER,
    KEY_KP_0, KEY_KP_DECIMAL, KEY_KP_DIVIDE, KEY_KP_MULTIPLY,
};
static const char* kPadKeyNames[PAD_PER_BANK] = {
    "7", "8", "9", "-",
    "4", "5", "6", "+",
    "1", "2", "3", "En",
    "0", ".", "/", "*",
};

// Mando: 16 pads no caben en los botones de un mando, así que van los 12 que
// se pueden pulsar sin mirar (cruceta, cuatro caras y cuatro gatillos) sobre
// los 12 primeros pads del banco. Los cuatro últimos se tocan con el ratón,
// con el teclado o cambiando de banco.
static const int kPadGamepadBtns[12] = {
    GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
    GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
    GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
    GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
    GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
    GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
};

// MIDI: la nota 36 (C1) es el pad 1 en casi todo lo que se vende con pads
// (MPC, Maschine, Launchpad en modo drum...), así que 36..51 son los 16 pads
// del banco visible. Un controlador con pads funciona sin configurar nada.
#define PAD_MIDI_BASE 36

static const char* kPadModeNames[3] = {"ONE-SHOT", "GATE", "LOOP"};
static const char* kPadModeShort[3] = {"1SHOT", "GATE", "LOOP"};
static const char* kPadQuantNames[5] = {"OFF", "1/16", "1/8", "1/4", "1/32"};

void RequestPadTrigger(int pad, float velocity) {
    Command cmd;
    cmd.type = CommandType::PadTrigger;
    cmd.padIndex = pad;
    cmd.padVelocity = velocity;
    g_commandQueue.push(cmd);
}

void RequestPadRelease(int pad) {
    Command cmd;
    cmd.type = CommandType::PadRelease;
    cmd.padIndex = pad;
    g_commandQueue.push(cmd);
}

void RequestPadStopAll() {
    Command cmd;
    cmd.type = CommandType::PadStopAll;
    g_commandQueue.push(cmd);
}

// Manda al motor el pad tal y como está en g_pads[pad].
void RequestSetPadConfig(int pad) {
    if (pad < 0 || pad >= PAD_TOTAL) return;
    const PadConfig& p = g_pads[pad];
    Command cmd;
    cmd.type = CommandType::SetPadConfig;
    cmd.padIndex = pad;
    cmd.sampleId = p.slot;
    cmd.padPitch = p.pitch;
    cmd.padVol = p.vol;
    cmd.padMode = p.mode;
    cmd.padChoke = p.choke;
    cmd.padFx = p.fx;
    g_commandQueue.push(cmd);
}

void RequestPadTransport(bool playing, bool recording, int quantize, int steps) {
    Command cmd;
    cmd.type = CommandType::SetPadTransport;
    cmd.padPlayFlag = playing ? 1 : 0;
    cmd.padRecFlag = recording ? 1 : 0;
    cmd.padQuant = quantize;
    cmd.padSteps = steps;
    g_commandQueue.push(cmd);
}

// pad < 0 borra el patrón entero; si no, solo la pista de ese pad.
void RequestClearPadPattern(int pad) {
    Command cmd;
    cmd.type = CommandType::ClearPadPattern;
    cmd.padIndex = pad;
    g_commandQueue.push(cmd);
}

void RequestSetPadStep(int tick, int pad, bool on) {
    Command cmd;
    cmd.type = CommandType::SetPadStep;
    cmd.padTick = tick;
    cmd.padIndex = pad;
    cmd.padOn = on ? 1 : 0;
    g_commandQueue.push(cmd);
}

void RequestSetMasterFx(int type, float x, float y, bool on) {
    Command cmd;
    cmd.type = CommandType::SetMasterFx;
    cmd.mfxType = type;
    cmd.mfxX = x;
    cmd.mfxY = y;
    cmd.mfxOn = on ? 1 : 0;
    g_commandQueue.push(cmd);
}

void RequestSetBichoVolume(int poolIndex, float volume) {
    Command cmd;
    cmd.type = CommandType::SetBichoVolume;
    cmd.bichoIndex = poolIndex;
    cmd.volumeVal = volume;
    g_commandQueue.push(cmd);
}

void RequestSetTempo(float bpm) {
    Command cmd;
    cmd.type = CommandType::SetTempo;
    cmd.newBpm = bpm;
    g_commandQueue.push(cmd);
}

// Los tres últimos parámetros son los atributos SUPERPUESTOS de la celda
// (espera, volumen y velocidad de la nota). Van con valor por defecto neutro
// para que las llamadas que no los usan pinten exactamente igual que antes.
void RequestPaintCell(int x, int y, unsigned char colorId, int sampleId,
                      ModifierType mod, int nextDx, int nextDy, float sustainSeconds,
                      float holdSeconds = 0.0f, float volMul = 1.0f, float timeMul = 1.0f) {
    Command cmd;
    cmd.type = CommandType::PaintCell;
    cmd.cellX = x;
    cmd.cellY = y;
    cmd.colorId = colorId;
    cmd.modifier = (int)mod;
    cmd.nextDx = nextDx;
    cmd.nextDy = nextDy;
    cmd.sustainSeconds = sustainSeconds;
    cmd.holdSeconds = holdSeconds;
    cmd.cellVolMul = volMul;
    cmd.cellTimeMul = timeMul;
    cmd.sampleId = sampleId;
    g_commandQueue.push(cmd);
}

// Teleporter cell: the engine's Teleport modifier jumps a notey to a fixed
// target cell. targetX/targetY point at the paired portal.
void RequestTeleportCell(int x, int y, int targetX, int targetY) {
    Command cmd;
    cmd.type = CommandType::PaintCell;
    cmd.cellX = x;
    cmd.cellY = y;
    cmd.colorId = 0; // no note
    cmd.modifier = (int)ModifierType::Teleport;
    cmd.targetX = targetX;
    cmd.targetY = targetY;
    g_commandQueue.push(cmd);
}

// Fase 3: pinta una celda COMPUESTA (lista de hasta 4 acciones ya resueltas).
void RequestPaintCompound(int x, int y, int count, const unsigned char* types, const int* as, const int* bs, const float* fs) {
    Command cmd;
    cmd.type = CommandType::PaintCompound;
    cmd.cellX = x;
    cmd.cellY = y;
    if (count > 4) count = 4;
    if (count < 0) count = 0;
    cmd.compCount = count;
    for (int i = 0; i < count; i++) {
        cmd.compType[i] = types[i];
        cmd.compA[i] = as[i];
        cmd.compB[i] = bs[i];
        cmd.compF[i] = fs[i];
    }
    g_commandQueue.push(cmd);
}

void RequestClearGrid() {
    Command cmd;
    cmd.type = CommandType::ClearGrid;
    g_commandQueue.push(cmd);
}

void RequestClearBichos() {
    Command cmd;
    cmd.type = CommandType::ClearBichos;
    g_commandQueue.push(cmd);
}

void RequestSetPaused(bool paused) {
    Command cmd;
    cmd.type = CommandType::SetPaused;
    cmd.pausedState = paused ? 1 : 0;
    g_commandQueue.push(cmd);
}

void RequestSetGridSize(int w, int h) {
    Command cmd;
    cmd.type = CommandType::SetGridSize;
    cmd.newGridW = w;
    cmd.newGridH = h;
    g_commandQueue.push(cmd);
}

void RequestTrackerCell(int ch, int row, int sampleId, unsigned char colorId, int fx) {
    Command cmd;
    cmd.type = CommandType::SetTrackerCell;
    cmd.trkChannel = ch;
    cmd.trkRow = row;
    cmd.sampleId = sampleId;
    cmd.colorId = colorId;
    cmd.modifier = fx;
    g_commandQueue.push(cmd);
}

void RequestClearTracker() {
    Command cmd;
    cmd.type = CommandType::ClearTracker;
    g_commandQueue.push(cmd);
}

void RequestLinearCell(int col, int row, int sampleId, unsigned char colorId, int fx) {
    Command cmd;
    cmd.type = CommandType::SetLinearCell;
    cmd.linCol = col;
    cmd.linRow = row;
    cmd.sampleId = sampleId;
    cmd.colorId = colorId;
    cmd.modifier = fx;
    g_commandQueue.push(cmd);
}

void RequestClearLinear() {
    Command cmd;
    cmd.type = CommandType::ClearLinear;
    g_commandQueue.push(cmd);
}

void RequestLinearParams(int length, bool loop) {
    Command cmd;
    cmd.type = CommandType::SetLinearParams;
    cmd.linLen = length;
    cmd.linLoop = loop ? 1 : 0;
    g_commandQueue.push(cmd);
}

void RequestResetPlayhead() {
    Command cmd;
    cmd.type = CommandType::ResetPlayhead;
    g_commandQueue.push(cmd);
}

void RequestPreviewPlay(int slot, long long a0, long long a1) {
    Command cmd;
    cmd.type = CommandType::PreviewPlay;
    cmd.sampleId = slot;
    cmd.previewA0 = a0;
    cmd.previewA1 = a1;
    g_commandQueue.push(cmd);
}

void RequestPreviewStop() {
    Command cmd;
    cmd.type = CommandType::PreviewStop;
    g_commandQueue.push(cmd);
}

// ---------------------------------------------------------------------------
// Paleta y herramientas
// ---------------------------------------------------------------------------
// Gamepad: acciones lógicas remapeables. Cada acción apunta a un botón físico
// (GAMEPAD_BUTTON_*) en un mapa que el usuario puede cambiar. Sirve para
// CUALQUIER control, no solo el 8BitDo.
// ---------------------------------------------------------------------------
enum GpAction {
    GA_TOGGLE_MODE = 0, // alterna NOTEYS <-> UI
    GA_CONTROLLABLE,    // entra/sale del "Notey Controlable"
    GA_PLACE_NOTE,      // pone una nota en el cursor
    GA_PLACE_NOTEY,     // suelta un notey en el cursor
    GA_ERASE,           // borra nota y notey en el cursor
    GA_PLAYSTOP,        // play / stop
    GA_NOTE_PREV,       // nota anterior (tono a colocar)
    GA_NOTE_NEXT,       // nota siguiente
    GA_COUNT
};
static const char* kGpActionNames[GA_COUNT] = {
    "Toggle NOTEYS/UI", "Controllable notey", "Place note", "Place notey",
    "Erase note/notey", "Play / Stop", "Note prev", "Note next"
};
// Mapa por defecto (layout tipo Xbox/8BitDo en X-input).
static const int kGpDefault[GA_COUNT] = {
    GAMEPAD_BUTTON_MIDDLE_RIGHT,   // Start
    GAMEPAD_BUTTON_MIDDLE_LEFT,    // Select
    GAMEPAD_BUTTON_RIGHT_FACE_DOWN,  // A
    GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, // B
    GAMEPAD_BUTTON_RIGHT_FACE_LEFT,  // X
    GAMEPAD_BUTTON_RIGHT_FACE_UP,    // Y
    GAMEPAD_BUTTON_LEFT_TRIGGER_1,   // LB
    GAMEPAD_BUTTON_RIGHT_TRIGGER_1,  // RB
};
// Nombre corto de un botón físico, para la UI de remapeo.
static const char* GpButtonName(int b) {
    switch (b) {
        case GAMEPAD_BUTTON_LEFT_FACE_UP: return "D-Up";
        case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "D-Right";
        case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "D-Down";
        case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "D-Left";
        case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Y/North";
        case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B/East";
        case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "A/South";
        case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "X/West";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "LB";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return "LT";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "RB";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return "RT";
        case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Select";
        case GAMEPAD_BUTTON_MIDDLE_RIGHT: return "Start";
        case GAMEPAD_BUTTON_MIDDLE: return "Guide";
        case GAMEPAD_BUTTON_LEFT_THUMB: return "L3";
        case GAMEPAD_BUTTON_RIGHT_THUMB: return "R3";
        default: return "?";
    }
}

// -------- Piano-note pitch system --------
// The palette is now a chromatic octave (12 notes C..B). A painted cell stores
// an ABSOLUTE semitone (note index + octave*12), so it plays like a real note
// on a keyboard. The octave selector shifts which octave you paint in.
static const int NOTE_COUNT = 12;
static const char* kNoteNames[NOTE_COUNT] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

struct PaletteEntry {
    Color displayColor;
    const char* label; // note name
};
std::vector<PaletteEntry> g_palette; // 12 note colors

void BuildDefaultPalette() {
    // A chromatic color wheel: 12 evenly spaced hues.
    for (int i = 0; i < NOTE_COUNT; i++) {
        Color c = ColorFromHSV((float)i * 360.0f / NOTE_COUNT, 0.62f, 0.92f);
        g_palette.push_back({c, kNoteNames[i]});
    }
}

// Absolute semitone -> engine colorId (semitone 0 = original playback speed).
static unsigned char SemitoneToColorId(int semi) {
    int c = semi + PITCH_BASE;
    if (c < 1) c = 1;
    if (c > 255) c = 255;
    return (unsigned char)c;
}

// Semitone -> "C4", "F#3", "B-1"... (octave 0 = the un-shifted pitch).
static const char* SemitoneName(int semi) {
    int n = ((semi % 12) + 12) % 12;
    int oct = (int)floorf(semi / 12.0f);
    return TextFormat("%s%d", kNoteNames[n], oct);
}

static const int TOOL_ARROW = 12;
static const int TOOL_SUSTAIN = 13;   // HOLD
static const int TOOL_FX = 14;
static const int TOOL_VOL = 15;       // volumen de la nota
static const int TOOL_TIME = 16;      // cámara lenta / rápida de la nota
static const int TOOL_ARP = 17;
// La celda MELODY va justo detrás del arpegio porque hace lo mismo —estampar
// varias notas de una vez— sólo que las notas salen de escuchar un sonido en
// vez de una lista fija de acordes.
static const int TOOL_MELODY = 18;
static const int TOOL_TELEPORT = 19;
static const int TOOL_MUTE = 20;
static const int TOOL_ERASE = 21;
static const int PALETTE_CELLS = 22;
static const int TOOL_MODCELL = 100; // celda personalizada de un mod (fuera de la paleta fija)
static const int NUM_TELE_IDS = 8; // paired teleporters 0..7

// Arpeggiator "stamp": one click lays a run of note cells to the right,
// spelling a chord — the notey walking through plays the arpeggio.
struct ArpPat { const char* name; int n; int steps[8]; };
static const ArpPat kArpPatterns[] = {
    {"maj",   4, {0, 4, 7, 12}},
    {"min",   4, {0, 3, 7, 12}},
    {"oct",   2, {0, 12}},
    {"up-dn", 6, {0, 4, 7, 12, 7, 4}},
    {"5th",   3, {0, 7, 12}},
    {"7th",   4, {0, 4, 7, 10}},
};
static const int kArpCount = 6;

// Old 8-slot palette semitones, for loading pre-v6 projects.
static const int kOldPaletteSemis[8] = {-5, -3, -1, 0, 2, 4, 5, 7};

static const int   kDirDx[4] = {1, 0, -1, 0};
static const int   kDirDy[4] = {0, 1, 0, -1};
static const float kDirRot[4] = {0.0f, 90.0f, 180.0f, 270.0f};
static const char* kDirName[4] = {"right", "down", "left", "up"};

// HOLD. La última opción es "off": vuelve a quitar la espera de una celda,
// que es la única forma de deshacerla sin borrar la nota que hay debajo.
static const float kSustainChoices[] = {0.5f, 1.0f, 2.0f, 4.0f, 0.0f};
static const char* kSustainLabels[]  = {"0.5s", "1s", "2s", "4s", "off"};
static const int   kSustainCount = 5;

// Volumen de la nota. 100% es el valor neutro, así que también sirve de "off".
static const float kVolChoices[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f};
static const int   kVolCount = 10;

// Cámara lenta / rápida de la nota: multiplica la velocidad de reproducción,
// así que la imagen y el sonido se estiran juntos (efecto cinta). x1 = off.
static const float kTimeChoices[] = {1.0f, 0.25f, 0.5f, 0.75f, 1.5f, 2.0f, 4.0f};
static const char* kTimeLabels[]  = {"x1", "x1/4", "x1/2", "x3/4", "x1.5", "x2", "x4"};
static const int   kTimeCount = 7;

static const float kSpeedChoices[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static const char* kSpeedLabels[]  = {"1/4", "1/2", "x1", "x2", "x4"};
static const int   kSpeedCount = 5;

static const int          kFxCount = 4;
static const char*        kFxNames[4] = {"REVERB", "ECHO", "REVERSE", "CHORUS"};
static const char*        kFxShort[4] = {"RV", "EC", "RS", "CH"};
static const ModifierType kFxMod[4]   = {ModifierType::ReverbFx, ModifierType::EchoFx, ModifierType::Reverse, ModifierType::ChorusFx};
// Tracker fx code = fxTypeIdx+1 (1=reverb 2=echo 3=reverse 4=chorus).

static const float kSceneDurations[] = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
static const int   kSceneDurationCount = 6;

static const int kGridPresetW[4] = {16, 32, 48, 64};
static const int kGridPresetH[4] = {12, 24, 36, 48};

// ---------------------------------------------------------------------------
// Escenas, slots y estado de UI
// ---------------------------------------------------------------------------

static const int MAX_GW = 64;
static const int MAX_GH = 48;
static const int MAX_CELLS = MAX_GW * MAX_GH;
static int g_gridW = 32;
static int g_gridH = 24;

// Current octave for painting notes (each step = 12 semitones). Baked into
// each painted cell, so changing it later doesn't move existing notes.
static int g_octave = 0;
static const int kMinOctave = -4;
static const int kMaxOctave = 4;

// Banco: slots 0..63 = CLIPS de video (números 1..64),
// slots 64..127 = SAMPLES de audio (A1..D16). Se navegan por páginas de 16.
static const int NUM_CLIPS = 64;
static const int NUM_SAMPLES = 64;
static const int MAX_SLOTS = 128;
static const int SAMPLE_BASE = 64;
static const int PAGE_SIZE = 16;
static const int NUM_PAGES = 4;

static const int MAX_SCENES = 10;

enum CellKind : unsigned char { CELL_EMPTY = 0, CELL_COLOR, CELL_ARROW, CELL_SUSTAIN, CELL_MUTE, CELL_FX, CELL_TELEPORT, CELL_MODCELL };
struct MirrorCell {
    unsigned char kind = CELL_EMPTY;
    Color color = {0, 0, 0, 255};
    int clip = 0;
    int dir = 0;        // CELL_ARROW: direction | CELL_TELEPORT: teleporter id
    float sust = 1.0f;
    int fxType = 0;
    int pitchIdx = 0;
    int modCellId = -1; // CELL_MODCELL: índice en g_modCells (Fase 2)

    // Atributos SUPERPUESTOS, independientes de `kind`: se pueden combinar
    // entre sí y con un FX en la misma celda.
    //
    // hold > 0 en una celda VACÍA (kind == CELL_EMPTY) es un SILENCIO: el
    // notey se para ahí ese tiempo sin disparar nada. Por eso una celda no
    // está "en blanco" sólo por tener kind == CELL_EMPTY — usa cellIsBlank().
    float hold = 0.0f;  // segundos de espera (0 = ninguna)
    float vol  = 1.0f;  // volumen de la nota (0.1..1)
    float tmul = 1.0f;  // velocidad de la nota (0.25..4, 1 = normal)
};

// Una celda está realmente vacía cuando no tiene ni tipo ni ningún atributo
// superpuesto. Comprobar sólo `kind == CELL_EMPTY` se dejaría fuera los
// silencios, que se dibujan y se guardan como cualquier otra celda.
static inline bool cellIsBlank(const MirrorCell& c) {
    return c.kind == CELL_EMPTY && c.hold <= 0.0f && c.vol >= 1.0f && c.tmul == 1.0f;
}

// Los atributos de volumen y velocidad sólo tienen sentido sobre una nota;
// HOLD, en cambio, vale en cualquier celda (ver arriba).
static inline bool cellHasNote(const MirrorCell& c) {
    return c.kind == CELL_COLOR || c.kind == CELL_FX || c.kind == CELL_SUSTAIN;
}

// Distinct colors for teleporter ids so paired portals are easy to spot.
static const Color kTeleColors[8] = {
    {90, 200, 255, 255}, {255, 150, 60, 255}, {180, 120, 255, 255}, {120, 230, 140, 255},
    {255, 90, 160, 255}, {230, 220, 80, 255}, {90, 230, 220, 255}, {230, 110, 110, 255},
};

struct BugSpawn {
    int x = 0, y = 0, dx = 1, dy = 0;
    int clip = 0;
    float tempoMul = 1.0f;
    bool muted = false;  // still moves, but doesn't trigger clips/samples
    float volume = 1.0f; // per-notey gain (0..1.5)
    bool stopped = false; // frozen in place (per-notey play/stop)
};

struct Scene {
    MirrorCell cells[MAX_CELLS];
    std::vector<BugSpawn> bugs;
    float duration = 0.0f;
};

static std::vector<Scene> g_scenes;
static int g_curScene = 0;
static MirrorCell* g_mirror = nullptr;

struct TrkCell {
    int sample = -1;
    int pitchIdx = 3;
    int fx = 0;
};
static TrkCell g_tracker[TRACKER_CHANNELS][TRACKER_ROWS];

// Espejo del modo lineal en el hilo principal (para dibujar y guardar).
// Funciona como el canvas: cada celda recuerda su clip/sample, su TONO (baked
// como semitono absoluto igual que las celdas del canvas) y su efecto FX. Las
// filas son CARRILES (no tonos): puedes añadir o quitar filas.
struct LinCell {
    int sample = -1;      // -1 = celda vacía
    int pitchIdx = 3;     // semitono absoluto (nota + octava*12)
    unsigned char fx = 0; // 0=nada, 1=reverb, 2=echo, 3=reverse, 4=chorus
};
static LinCell g_linear[LINEAR_COLS][LINEAR_ROWS];
static int g_linearLength = 32;   // columnas del loop
static bool g_linearLoop = true;
static int g_linearRows = 4;      // carriles visibles/activos (1..LINEAR_ROWS)

static std::string g_slotPath[MAX_SLOTS];
static int g_slotTrimStart[MAX_SLOTS] = {0};
static int g_slotTrimLen[MAX_SLOTS] = {0};
// Optional image/GIF that overrides a slot's visual while keeping its audio.
static std::string g_slotVisualPath[MAX_SLOTS];
// Optional Pure Data (.pd) insert effect per slot.
static std::string g_slotPdPath[MAX_SLOTS];

// Optional GLSL fragment shader (video effect) per slot. The user writes the
// .fs/.glsl separately and imports it here — same idea as the Pd patches, but
// for the VIDEO of a clip in the collage instead of its audio. The shader gets
// raylib's usual `texture0`/`fragTexCoord`/`fragColor`, plus two custom
// uniforms we feed each frame: `float time` (seconds) and `vec2 resolution`.
static std::string g_slotShaderPath[MAX_SLOTS];
static Shader g_slotShader[MAX_SLOTS];       // id==0 while unused
static bool   g_slotShaderOn[MAX_SLOTS] = {false};
static int    g_slotShaderLocTime[MAX_SLOTS];
static int    g_slotShaderLocRes[MAX_SLOTS];

struct ClipFX {
    bool flipX = false;
    bool zoomPulse = false;
    bool rotate = false;
    bool center = false;
    float scale = 1.0f;
    int layer = 4;
    bool move = false;
    float ax = 0.2f, ay = 0.5f;
    float bx = 0.8f, by = 0.5f;
    // --- Colocación libre y aspecto de la capa ---
    // `place` es la tercera forma de situar un clip, junto a CENTER (siempre en
    // medio) y al reparto al azar de siempre: la posición EXACTA que elija el
    // usuario arrastrándolo sobre el fotograma. Va en 0..1 del fotograma
    // exportado, no en píxeles, para que cambiar de 16:9 a 9:16 no lo descoloque.
    bool  place = false;
    float posX = 0.5f, posY = 0.5f;
    float rotDeg = 0.0f;      // giro FIJO, distinto de `rotate` (que da vueltas)
    float opacity = 1.0f;     // 0 = invisible, 1 = opaco
    int   blend = BLEND_FX_NORMAL;
};
static ClipFX g_clipFX[MAX_SLOTS];

static int g_exportW = 1280;
static int g_exportH = 720;

// Efecto de vídeo analógico sobre la mezcla final (ver NtscFX.h). El shader se
// compila la primera vez que se enciende, no al arrancar: quien no lo use no
// paga nada.
static NtscFX     g_ntsc;
static NtscShader g_ntscShader;

static char  g_status[512] = "";
static float g_statusTimer = 0.0f;

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    g_statusTimer = 4.0f;
    printf("UI: %s\n", g_status);
}

// -------- Tema de UI (personalizable) --------
// Un puñado de colores clave que el usuario puede cambiar (presets en el panel
// DEV). Se aplican a las superficies más visibles: fondo, tiras/paneles, y los
// botones (UIButton). El texto blanco sigue legible en todos los presets (por
// eso son todos oscuros).
struct UITheme {
    const char* name;
    Color bg;         // ClearBackground
    Color panel;      // tiras/paneles
    Color button;     // base de UIButton
    Color buttonHover;
    Color border;     // bordes
    Color accent;     // resaltados (BPM, activo...)
};
static const UITheme kThemes[] = {
    {"Midnight", {12, 12, 18, 255},  {24, 25, 36, 255},  {46, 48, 64, 255},  {72, 74, 100, 255},  {110, 112, 140, 255}, {120, 230, 140, 255}},
    {"Slate",    {20, 22, 26, 255},  {32, 35, 42, 255},  {52, 56, 66, 255},  {78, 84, 98, 255},   {104, 110, 126, 255}, {120, 185, 235, 255}},
    {"Grape",    {18, 12, 24, 255},  {34, 26, 46, 255},  {58, 46, 74, 255},  {88, 68, 110, 255},  {120, 100, 150, 255}, {210, 140, 255, 255}},
    {"Ember",    {20, 14, 12, 255},  {38, 28, 24, 255},  {64, 48, 42, 255},  {98, 72, 60, 255},   {132, 100, 84, 255},  {255, 170, 90, 255}},
    {"Forest",   {10, 18, 14, 255},  {22, 34, 28, 255},  {42, 62, 50, 255},  {66, 96, 78, 255},   {96, 128, 108, 255},  {150, 235, 160, 255}},
};
static const int kThemeCount = 5;
// Tema PROPIO editable (g_themeIdx == kThemeCount lo activa). El usuario elige
// el color de cada parte de la UI en el panel DEV. El punto de partida se
// guarda aparte para poder VOLVER a él: es fácil arrastrar los deslizadores
// hasta dejar la interfaz ilegible (texto blanco sobre fondo blanco) y entonces
// ya no se ve ni el botón con el que arreglarlo.
static const UITheme kCustomDefault = {"Custom", {12, 12, 18, 255}, {24, 25, 36, 255}, {46, 48, 64, 255}, {72, 74, 100, 255}, {110, 112, 140, 255}, {120, 230, 140, 255}};
static UITheme g_customTheme = kCustomDefault;
static int g_themeIdx = 0;
static UITheme g_theme = kThemes[0];

// Aplica el tema activo (preset o el propio) a g_theme.
static void ApplyTheme() {
    g_theme = (g_themeIdx >= 0 && g_themeIdx < kThemeCount) ? kThemes[g_themeIdx] : g_customTheme;
}
// Acceso a las 6 partes de un UITheme por índice, para editarlas en bucle.
static Color* ThemePart(UITheme& t, int i) {
    switch (i) {
        case 0: return &t.bg; case 1: return &t.panel; case 2: return &t.button;
        case 3: return &t.buttonHover; case 4: return &t.border; default: return &t.accent;
    }
}
static const char* kThemePartNames[6] = {"Background", "Panels", "Buttons", "Button hover", "Borders", "Accent"};

static bool UIButton(Rectangle r, const char* label, int fontSize = 14) {
    Vector2 m = GetMousePosition();
    bool hover = CheckCollisionPointRec(m, r);
    Color bg = hover ? g_theme.buttonHover : g_theme.button;
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, g_theme.border);
    int tw = MeasureText(label, fontSize);
    DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - fontSize) / 2), fontSize, RAYWHITE);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool IsAudioFile(const char* path) {
    return IsFileExtension(path, ".wav") || IsFileExtension(path, ".mp3") ||
           IsFileExtension(path, ".flac") || IsFileExtension(path, ".ogg");
}

static bool IsVideoFile(const char* path) {
    return IsFileExtension(path, ".mp4") || IsFileExtension(path, ".mov") ||
           IsFileExtension(path, ".webm") || IsFileExtension(path, ".avi") ||
           IsFileExtension(path, ".mkv") || IsFileExtension(path, ".mpg") ||
           IsFileExtension(path, ".mpeg");
}

// Still images and animated GIFs — loaded as visual clips (no audio).
static bool IsImageFile(const char* path) {
    return IsFileExtension(path, ".png") || IsFileExtension(path, ".jpg") ||
           IsFileExtension(path, ".jpeg") || IsFileExtension(path, ".bmp") ||
           IsFileExtension(path, ".gif");
}

// Any file that produces visual content (goes in the CLIP bar).
static bool IsVisualFile(const char* path) {
    return IsVideoFile(path) || IsImageFile(path);
}

// Decodes a still image or animated GIF into a contiguous RGB24 frame buffer.
// IMPORTANT: for an animated GIF, LoadImageAnim returns ALL frames packed in
// .data but .width/.height are ONE frame's size. Running ImageFormat on that
// whole buffer only converts one frame's worth and reads out of bounds — the
// bug that made GIFs fail. So we convert EACH frame separately here.
// Decodes to RGBA (4 channels) so image/GIF TRANSPARENCY is preserved — the
// alpha lets the collage show whatever's behind the transparent areas instead
// of a black box.
static bool DecodeImageOrGif(const char* path, int maxSide,
                             std::vector<unsigned char>& outRGB,
                             int& outW, int& outH, int& outFrames, double& outFps) {
    if (IsFileExtension(path, ".gif")) {
        int frames = 1;
        Image anim = LoadImageAnim(path, &frames);
        if (anim.data == nullptr || frames < 1) return false;
        int fw = anim.width, fh = anim.height;
        int bpp = GetPixelDataSize(1, 1, anim.format);
        if (bpp <= 0) { UnloadImage(anim); return false; }

        int tw = fw, th = fh;
        if (fw > maxSide || fh > maxSide) {
            float s = (float)maxSide / (fw > fh ? fw : fh);
            tw = (int)(fw * s); th = (int)(fh * s);
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
        }

        outRGB.resize((size_t)frames * tw * th * 4);
        for (int f = 0; f < frames; f++) {
            Image one;
            one.data = (unsigned char*)anim.data + (size_t)f * fw * fh * bpp;
            one.width = fw;
            one.height = fh;
            one.mipmaps = 1;
            one.format = anim.format;
            Image copy = ImageCopy(one); // owns exactly one frame's bytes
            ImageFormat(&copy, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (copy.width != tw || copy.height != th) ImageResize(&copy, tw, th);
            memcpy(outRGB.data() + (size_t)f * tw * th * 4, copy.data, (size_t)tw * th * 4);
            UnloadImage(copy);
        }
        UnloadImage(anim);
        outW = tw; outH = th; outFrames = frames; outFps = 14.0;
        return true;
    } else {
        Image img = LoadImage(path);
        if (img.data == nullptr) return false;
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8); // keep alpha
        if (img.width > maxSide || img.height > maxSide) {
            float s = (float)maxSide / (img.width > img.height ? img.width : img.height);
            ImageResize(&img, (int)(img.width * s), (int)(img.height * s));
        }
        outW = img.width; outH = img.height; outFrames = 1; outFps = 1.0;
        outRGB.assign((unsigned char*)img.data,
                      (unsigned char*)img.data + (size_t)img.width * img.height * 4);
        UnloadImage(img);
        return true;
    }
}

// Loads an image/GIF as a standalone visual clip (no audio) in a slot. RGBA.
static bool LoadImageIntoSlot(int slot, const char* path, int maxSide) {
    std::vector<unsigned char> rgb;
    int w, h, frames;
    double fps;
    if (!DecodeImageOrGif(path, maxSide, rgb, w, h, frames, fps)) return false;
    return g_engine.getInstrumentBank().LoadVisualFrames(slot, rgb.data(), frames, w, h, fps, 4);
}

// Replaces a slot's VISUAL with an image/GIF, keeping any existing audio. RGBA.
static bool ReplaceVisualIntoSlot(int slot, const char* path, int maxSide) {
    std::vector<unsigned char> rgb;
    int w, h, frames;
    double fps;
    if (!DecodeImageOrGif(path, maxSide, rgb, w, h, frames, fps)) return false;
    return g_engine.getInstrumentBank().ReplaceVisualFrames(slot, rgb.data(), frames, w, h, fps, 4);
}

static void DrawArrowGlyph(float cx, float cy, float radius, int dir, Color c) {
    DrawPoly({cx, cy}, 3, radius, kDirRot[dir], c);
}

static void DrawMuteGlyph(float cx, float cy, float radius, Color c) {
    DrawCircleLines((int)cx, (int)cy, radius, c);
    DrawLineEx({cx - radius * 0.7f, cy + radius * 0.7f}, {cx + radius * 0.7f, cy - radius * 0.7f}, 2, c);
}

// Portal glyph: two concentric rings (a teleporter swirl).
static void DrawTeleGlyph(float cx, float cy, float radius, Color c) {
    DrawCircleLines((int)cx, (int)cy, radius, c);
    DrawCircleLines((int)cx, (int)cy, radius * 0.55f, c);
}

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // Pinguus le pasa fotogramas a ffmpeg por una tubería. Si ffmpeg se cae a
    // mitad (no está instalado, disco lleno, un códec que no traga), la tubería
    // se rompe y el SIGPIPE por defecto MATA el proceso: se perdía la sesión
    // entera sin un solo mensaje. Ignorándolo, el fwrite devuelve EPIPE y el
    // fallo se puede contar.
    signal(SIGPIPE, SIG_IGN);
#endif

    // Modo espejo: solo muestra el collage (para otro monitor/proyector).
    if (argc > 1 && strcmp(argv[1], "--live") == 0) {
        return RunLiveViewer();
    }

    // ------------------------- Layout -------------------------
    const int screenWidth = 1280;
    const int screenHeight = 720;
    const int leftPanelWidth = 800;
    const int rightPanelWidth = screenWidth - leftPanelWidth;
    const int tabsH = 24;
    const int viewY0 = tabsH;
    const int viewH = 520;
    const int row1Y = 544, row2Y = 576, row3Y = 608;
    const int paletteY = 640;
    const int paletteHeight = screenHeight - paletteY;

    InitWindow(screenWidth, screenHeight, "Pinguus - Plunderphonics Painter");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    RenderTexture2D collageRT = LoadRenderTexture(g_exportW, g_exportH);
    // Segundo destino para la pasada de vídeo analógico. Se reserva siempre
    // (cuesta una textura) para no tener que crearlo y destruirlo cada vez
    // que se enciende o se apaga el efecto en mitad de una grabación.
    RenderTexture2D postRT = LoadRenderTexture(g_exportW, g_exportH);
    // Reloj propio del collage: se dibuja a 30 fps aunque la ventana vaya a 60.
    float collageAccum = 0.0f;
    bool  collageFirst = true;   // el primero sí, para no enseñar basura
    RenderTexture2D modelPreviewRT = LoadRenderTexture(360, 360); // preview del editor de modelo

    // Shader de iluminación para los modelos 3D (Blinn-Phong: 1 luz direccional
    // + ambiente). Se compila desde memoria; sus uniformes se fijan cada frame.
    {
        const char* vs =
            "#version 330\n"
            "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec3 vertexNormal; in vec4 vertexColor;\n"
            "uniform mat4 mvp; uniform mat4 matModel; uniform mat4 matNormal;\n"
            "out vec3 fragPosition; out vec2 fragTexCoord; out vec4 fragColor; out vec3 fragNormal;\n"
            "void main(){ fragPosition=vec3(matModel*vec4(vertexPosition,1.0)); fragTexCoord=vertexTexCoord;\n"
            "  fragColor=vertexColor; fragNormal=normalize(vec3(matNormal*vec4(vertexNormal,1.0)));\n"
            "  gl_Position=mvp*vec4(vertexPosition,1.0); }";
        const char* fs =
            "#version 330\n"
            "in vec3 fragPosition; in vec2 fragTexCoord; in vec4 fragColor; in vec3 fragNormal;\n"
            "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
            "uniform vec3 lightDir; uniform vec3 lightColor; uniform vec3 ambient; uniform vec3 viewPos;\n"
            "out vec4 finalColor;\n"
            "void main(){ vec4 texel=texture(texture0,fragTexCoord);\n"
            "  vec3 n=normalize(fragNormal); vec3 l=normalize(-lightDir);\n"
            "  float diff=max(dot(n,l),0.0);\n"
            "  vec3 v=normalize(viewPos-fragPosition); vec3 h=normalize(l+v);\n"
            "  float spec=pow(max(dot(n,h),0.0),24.0)*0.25;\n"
            "  vec3 light=ambient + lightColor*diff + lightColor*spec;\n"
            "  vec3 col=texel.rgb*fragColor.rgb*colDiffuse.rgb*light;\n"
            "  finalColor=vec4(col, texel.a*fragColor.a*colDiffuse.a); }";
        g_lightShader = LoadShaderFromMemory(vs, fs);
        g_lightLocDir = GetShaderLocation(g_lightShader, "lightDir");
        g_lightLocColor = GetShaderLocation(g_lightShader, "lightColor");
        g_lightLocAmbient = GetShaderLocation(g_lightShader, "ambient");
        g_lightLocView = GetShaderLocation(g_lightShader, "viewPos");

        // Toon (cel-shading): difuso en BANDAS + luz de borde (rim) para el look anime.
        const char* tfs =
            "#version 330\n"
            "in vec3 fragPosition; in vec2 fragTexCoord; in vec4 fragColor; in vec3 fragNormal;\n"
            "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
            "uniform vec3 lightDir; uniform vec3 lightColor; uniform vec3 ambient; uniform vec3 viewPos;\n"
            "out vec4 finalColor;\n"
            "void main(){ vec4 texel=texture(texture0,fragTexCoord);\n"
            "  vec3 n=normalize(fragNormal); vec3 l=normalize(-lightDir);\n"
            "  float d=max(dot(n,l),0.0);\n"
            "  float band = d>0.62 ? 1.0 : (d>0.28 ? 0.62 : 0.34);\n"   // 3 niveles
            "  vec3 v=normalize(viewPos-fragPosition);\n"
            "  float rim=pow(1.0-max(dot(n,v),0.0),3.0)*0.45;\n"
            "  vec3 light=ambient + lightColor*band + lightColor*rim;\n"
            "  vec3 col=texel.rgb*fragColor.rgb*colDiffuse.rgb*light;\n"
            "  finalColor=vec4(col, texel.a*fragColor.a*colDiffuse.a); }";
        g_toonShader = LoadShaderFromMemory(vs, tfs);
        g_toonLocDir = GetShaderLocation(g_toonShader, "lightDir");
        g_toonLocColor = GetShaderLocation(g_toonShader, "lightColor");
        g_toonLocAmbient = GetShaderLocation(g_toonShader, "ambient");
        g_toonLocView = GetShaderLocation(g_toonShader, "viewPos");
    }

    g_engine.setGridSize(g_gridW, g_gridH);
    g_engine.setSampleRate(44100);
    g_engine.setMidiCallback(OnSequencerMidiNote, nullptr);
    BuildDefaultPalette();

    g_scenes.reserve(MAX_SCENES);
    g_scenes.emplace_back();
    g_curScene = 0;
    g_mirror = g_scenes[0].cells;

    try {
        g_midiOut = new RtMidiOut();
        g_midiOut->openVirtualPort("Puerto Salida Dibujo Musical");
    } catch (...) {
        g_midiOut = nullptr;
    }

    // MIDI de ENTRADA: crea el cliente pero NO abre puerto aquí — el puerto real
    // se elige más abajo (evitando el "Midi Through" virtual) o desde el panel
    // DEVICES. Se sondea por polling en el hilo principal (getMessage), no por
    // callback (empujar comandos desde otro hilo rompería la cola SPSC).
    try {
        g_midiIn = new RtMidiIn();
    } catch (...) {
        g_midiIn = nullptr;
    }

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate        = 44100;
    deviceConfig.dataCallback      = MiniaudioDataCallback;
    // A period that is a multiple of Pd's 64-sample block, so per-slot Pure
    // Data effects process whole blocks with no leftover tail.
    deviceConfig.periodSizeInFrames = 512;

    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        CloseWindow();
        return -1;
    }

    // Pure Data engine for per-slot audio effects (.pd patches).
    g_engine.getPd().init(44100);

    // Auto-carga silenciosa de assets/ si existe.
    {
        BeginDrawing();
        ClearBackground(g_theme.bg);
        const char* bootMsg = "PINGUUS - loading...";
        DrawText(bootMsg, (screenWidth - MeasureText(bootMsg, 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
        EndDrawing();

        if (DirectoryExists("assets")) {
            FilePathList files = LoadDirectoryFiles("assets");
            std::vector<std::string> mediaPaths;
            for (unsigned int i = 0; i < files.count; i++) {
                if (IsAudioFile(files.paths[i]) || IsVisualFile(files.paths[i])) {
                    mediaPaths.push_back(files.paths[i]);
                }
            }
            UnloadDirectoryFiles(files);
            std::sort(mediaPaths.begin(), mediaPaths.end());

            int clipSlot = 0, sampleSlot = SAMPLE_BASE;
            for (const std::string& path : mediaPaths) {
                if (IsAudioFile(path.c_str())) {
                    if (sampleSlot >= MAX_SLOTS) continue;
                    if (g_engine.getInstrumentBank().LoadAudioOnly(sampleSlot, path.c_str())) {
                        g_slotPath[sampleSlot] = path;
                        sampleSlot++;
                    }
                } else {
                    if (clipSlot >= NUM_CLIPS) continue;
                    bool ok = IsImageFile(path.c_str())
                                  ? LoadImageIntoSlot(clipSlot, path.c_str(), g_transcodeMaxSide)
                                  : g_engine.getInstrumentBank().LoadVideo(clipSlot, path.c_str(), true);
                    if (ok) {
                        g_slotPath[clipSlot] = path;
                        clipSlot++;
                    }
                }
            }
            int total = clipSlot + (sampleSlot - SAMPLE_BASE);
            if (total > 0) SetStatus("Loaded %d file(s)", total);
        }
    }

    ma_device_start(&device);

    // El motor arranca con su propia cuantización (ninguna) y la interfaz con
    // la suya (1/16): se sincronizan de entrada, para que lo que dice el botón
    // sea lo que hace el motor antes incluso de tocarlo.
    RequestPadTransport(false, false, 1, 16);

    float bpmDisplay = 120.0f;
    int selectedTool = 3;
    int lastColorIndex = 3;
    int selectedClipSlot = 0;
    int selectedSampleSlot = SAMPLE_BASE;
    int clipPage = 0, samplePage = 0; // páginas de 16 (0..3)
    int activeBar = 0;
    int selectedSpeedIdx = 2;
    int arrowDir = 0;
    int sustainIdx = 1;
    int fxTypeIdx = 0;
    int volIdx = 5;    // 50%: el 100% no cambiaría nada al colocarlo
    int timeIdx = 2;   // x1/2: lo mismo, x1 sería no hacer nada
    int arpIdx = 0;
    int teleId = 0;
    int selectedModCell = -1; // celda de mod elegida para pintar (TOOL_MODCELL)
    int lastPaintX = -1, lastPaintY = -1;
    bool paused = false;
    bool songMode = false;
    float sceneTimeLeft = 0.0f;
    int activeView = 0;
    bool showNoteyList = false; // left-side panel to list/mute noteys

    // ---------------- Modo BEATBOX (vista 3) ----------------
    int  padBank = 0;              // banco visible (0..3)
    int  padSel = 0;               // pad seleccionado en el inspector (0..15 del banco)
    int  padQuant = 1;             // cuantización al grabar: 0 off, 1 1/16, 2 1/8, 3 1/4, 4 1/32
    int  padSteps = 16;            // largo del bucle, en semicorcheas
    bool padPatPlaying = false;
    bool padRecArm = false;
    // FX maestro: dos mandos (x, y) que se manejan con un pad XY. `mfxLatch`
    // deja el efecto encendido al soltar; sin latch es momentáneo, como el
    // botón de FX de una sampleadora de verdad.
    int   mfxType = MFX_FILTER;
    float mfxX = 0.5f, mfxY = 0.35f;
    bool  mfxOn = false, mfxLatch = false;
    bool  padXYDragging = false;
    // Qué pads están pulsados AHORA y POR QUÉ, para saber cuándo soltar una
    // nota en modo GATE. Es una máscara y no un booleano porque el mismo pad
    // puede estar pulsado a la vez con la tecla y con el ratón (o con el mando
    // y el controlador MIDI): la nota solo se suelta cuando se han soltado
    // TODOS, no cuando se suelta el primero.
    enum { PADSRC_KEY = 1, PADSRC_MOUSE = 2, PADSRC_GP = 4, PADSRC_MIDI = 8 };
    unsigned char padHeld[PAD_TOTAL] = {0};

    auto padPress = [&](int g, int src, float vel) {
        if (g < 0 || g >= PAD_TOTAL || g_pads[g].empty()) return;
        RequestPadTrigger(g, vel);
        padHeld[g] |= (unsigned char)src;
    };
    auto padLift = [&](int g, int src) {
        if (g < 0 || g >= PAD_TOTAL || !(padHeld[g] & src)) return;
        padHeld[g] &= (unsigned char)~src;
        if (padHeld[g] == 0) RequestPadRelease(g);
    };

    // Gamepad: dos modos alternables con un botón — NOTEYS (mover un cursor en
    // el grid y soltar/borrar noteys) y UI (transporte, tempo, vista, slot...).
    int gpMode = 0;               // 0 = NOTEYS, 1 = UI
    float gpCurX = -1, gpCurY = -1; // cursor del grid en celdas (-1 = sin iniciar)
    int gpSpawnDir = 0;           // dirección con la que se sueltan los noteys
    int gpIndex = -1;             // índice del gamepad activo (escaneado 0..3)
    int gpMap[GA_COUNT];          // remapeo: acción -> botón físico
    for (int i = 0; i < GA_COUNT; i++) gpMap[i] = kGpDefault[i];
    int gpRemapAction = -1;       // acción esperando captura de botón (-1 = ninguna)
    bool gpControllable = false;  // "Notey Controlable": moverse dispara celdas
    int gpPrevCellX = -1, gpPrevCellY = -1; // última celda del cursor (para trigger al entrar)
    bool gpRStickLatchX = false, gpRStickLatchY = false; // edge del stick derecho
    // MIDI de entrada: última nota/mensaje (para HUD y diagnóstico).
    int midiLastNote = -1;
    float midiHudTimer = 0.0f;
    int midiInPort = -1;                       // puerto MIDI abierto (-1 = ninguno)
    unsigned char midiLastBytes[3] = {0, 0, 0}; // último mensaje recibido (diagnóstico)
    int midiLastLen = 0;
    bool showDevices = false;     // panel DEVICES (elegir puerto MIDI / ver gamepad)
    int  devTab = 0;              // 0 = mandos y tema, 1 = teléfono / cámara / micro
    // ¿Puede un móvil grabar en un slot por su cuenta, sin que nadie toque el
    // PC? Es la gracia de la función (el del móvil no depende del que está
    // sentado aquí), pero por Tailscale ese móvil puede estar en cualquier
    // parte, así que hay un interruptor para cuando estás compartiendo pantalla
    // o simplemente no quieres que te toquen los slots. Se guarda en controls.cfg.
    bool allowPhoneRec = true;
    bool showMods = false;        // panel MODS (cargar/recargar scripts .lua)
    bool showNtsc = false;        // panel VHS (efecto de vídeo analógico final)

    // ---- Banco de MELODÍAS (la celda MELODY de la paleta) ----
    // Ocho melodías sacadas de escuchar un sonido. Funcionan como el acorde del
    // arpegio: eliges cuál, y al pinchar en el lienzo se estampa. El panel es su
    // editor — analizar, ver, renombrar y borrar.
    MelodyClip g_melodies[MELODY_BANK_SIZE];
    int   melIdx = 0;                       // cuál de las ocho está elegida
    bool  showMelody = false;               // el editor del banco
    int   melSrcSlot = SAMPLE_BASE;         // de dónde sale el audio a analizar
    PitchToNotes::Options melOpt;
    PitchToNotes::Result  melResult;        // lo último analizado, aún sin guardar
    bool  melAnalysed = false;
    bool  melQuantize = true;
    // Cuánto tiempo vale UNA casilla del lienzo. Es lo que decide si una
    // melodía larga cabe: a 1/16 un tarareo de seis segundos son cuarenta y
    // cinco casillas y no entra en ninguna rejilla; a 1/4 son doce y entra de
    // sobra, sonando igual pero al cuádruple de casilla.
    int   melDiv = 4;                       // 4 = 1/16, 2 = 1/8, 1 = 1/4
    std::string melNote;                    // resumen del último análisis
    bool modelEditorOpen = false; // editor de modelo 3D (clic derecho en un slot)
    int modelEditorSlot = -1;
    int modelEditorAnim = 0;
    float modelEditorFrame = 0.0f;

    // Abre (o cierra, port<0) un puerto MIDI de entrada por índice.
    auto openMidiPort = [&](int port) {
        if (!g_midiIn) return;
        try {
            g_midiIn->closePort();
            int n = (int)g_midiIn->getPortCount();
            if (port >= 0 && port < n) {
                g_midiIn->openPort((unsigned)port);
                g_midiIn->ignoreTypes(false, false, false); // recibir realtime (start/stop)
                midiInPort = port;
                SetStatus("MIDI in: %s", g_midiIn->getPortName((unsigned)port).c_str());
            } else {
                midiInPort = -1;
            }
        } catch (...) { midiInPort = -1; }
    };

    // Al arrancar, elige el primer puerto que NO sea "Midi Through" (el puerto
    // virtual de ALSA que nunca recibe nada del teclado — la causa típica de
    // "no funciona"). Si no hay otro, abre el 0.
    if (g_midiIn) {
        try {
            int n = (int)g_midiIn->getPortCount();
            int pick = -1;
            for (int i = 0; i < n; i++) {
                std::string nm = g_midiIn->getPortName((unsigned)i);
                // Salta el "Midi Through" virtual de ALSA y nuestro PROPIO puerto
                // de salida (loopback), que no son un teclado real.
                if (nm.find("Through") != std::string::npos || nm.find("through") != std::string::npos) continue;
                if (nm.find("Dibujo Musical") != std::string::npos) continue;
                pick = i; break;
            }
            if (pick >= 0) openMidiPort(pick); // si no hay teclado real, queda cerrado
        } catch (...) {}
    }

    // Dispositivos de grabación (cámara/micro) elegibles desde el panel DEV.
#if defined(_WIN32)
    std::string camDevice = "";            // nombre dshow, p.ej. "Integrated Camera"
    std::string micDevice = "";            // nombre dshow del micrófono
#else
    std::string camDevice = "/dev/video0"; // Linux v4l2
    std::string micDevice = "default";     // Linux pulse source (o "default")
#endif
    std::vector<std::string> micSources;   // fuentes de audio detectadas
    std::vector<std::string> camSources;   // cámaras detectadas (solo Windows)

    // Enumera las fuentes de audio (micrófonos) del sistema.
    //
    // WINDOWS: los nombres de dispositivo de DirectShow son distintos en cada
    // PC ("Integrated Camera", "Cámara HD", "Micrófono (Realtek)"...), así que
    // no se pueden dejar escritos en el código — que es exactamente lo que
    // hacía antes recordCamMic, y por eso grabar desde cámara sólo funcionaba
    // de casualidad. ffmpeg sabe listarlos; los parseamos de su salida.
    auto refreshMicSources = [&]() {
        micSources.clear();
        camSources.clear();
#if defined(_WIN32)
        // ffmpeg escribe la lista en STDERR y termina con error (no hay input),
        // que es lo normal para este comando.
        std::string listing;
        RunCommandRead("ffmpeg -hide_banner -list_devices true -f dshow -i dummy 2>&1", listing);
        {
            bool inAudio = false;
            size_t pos = 0;
            char ln[1024];
            while (pos < listing.size()) {
                size_t nl = listing.find('\n', pos);
                if (nl == std::string::npos) nl = listing.size();
                snprintf(ln, sizeof(ln), "%.*s", (int)(nl - pos), listing.c_str() + pos);
                pos = nl + 1;
                // Formato: [dshow @ ...] "Nombre del dispositivo" (video|audio)
                if (strstr(ln, "DirectShow audio devices")) { inAudio = true; continue; }
                if (strstr(ln, "DirectShow video devices")) { inAudio = false; continue; }
                const char* q1 = strchr(ln, '"');
                if (!q1) continue;
                const char* q2 = strchr(q1 + 1, '"');
                if (!q2) continue;
                std::string name(q1 + 1, q2 - q1 - 1);
                if (name.empty()) continue;
                // ffmpeg repite cada dispositivo con su "alternative name"
                // (una cadena larguísima tipo @device_pnp_...); nos quedamos
                // con el legible.
                if (name.compare(0, 8, "@device_") == 0) continue;
                if (strstr(ln, "(audio)") || (inAudio && !strstr(ln, "(video)"))) micSources.push_back(name);
                else camSources.push_back(name);
            }
        }
        // Primer arranque: elige lo primero que haya, para que "Record" funcione
        // sin pasar por el panel.
        if (camDevice.empty() && !camSources.empty()) camDevice = camSources[0];
        if (micDevice.empty() && !micSources.empty()) micDevice = micSources[0];
#elif !defined(__APPLE__)
        FILE* p = popen("pactl list short sources 2>/dev/null", "r");
        if (p) {
            char ln[512];
            while (fgets(ln, sizeof(ln), p)) {
                char idx[64], name[256];
                if (sscanf(ln, "%63s %255s", idx, name) == 2) micSources.push_back(name);
            }
            pclose(p);
        }
#endif
    };
    refreshMicSources();

    // Mando + dispositivos de grabación: se persisten en controls.cfg.
    // Preguntar DÓNDE guardar al terminar una toma.
    //
    // Por defecto no se pregunta y la toma cae en temp/: encadenar tomas sin que
    // un diálogo se meta por medio es justo lo que se quiere mientras se busca
    // la buena. Pero cuando SÍ sale la buena hay que poder sacarla de ahí, y
    // antes no había forma — se quedaba enterrada en temp/ para siempre. Así que
    // ahora hay las dos cosas: este interruptor para que pregunte siempre, y un
    // botón SAVE AS que aparece con la última toma para llevársela cuando toque.
    bool recAskWhere = false;

    auto saveControls = [&]() {
        FILE* f = fopen("controls.cfg", "w");
        if (!f) return;
        for (int i = 0; i < GA_COUNT; i++) fprintf(f, "gpmap %d %d\n", i, gpMap[i]);
        fprintf(f, "cam %s\n", camDevice.c_str());
        fprintf(f, "mic %s\n", micDevice.c_str());
        fprintf(f, "theme %d\n", g_themeIdx);
        // Teléfono-cámara (app MJPEG): recuerda la IP elegida.
        if (!g_ipcam.host().empty()) fprintf(f, "phonecam %s %d\n", g_ipcam.host().c_str(), g_ipcamPort);
        // Vía preferida para el teléfono (0 = Tailscale, 1 = Wi-Fi local). La
        // VISIBILIDAD de las IPs no se guarda a propósito: siempre oculta al abrir.
        fprintf(f, "phoneroute %d\n", g_phoneRoute);
        fprintf(f, "phonerec %d\n", allowPhoneRec ? 1 : 0);
        fprintf(f, "recaskwhere %d\n", recAskWhere ? 1 : 0);
        // Tema propio: 6 partes x RGB.
        fprintf(f, "customtheme");
        for (int i = 0; i < 6; i++) { Color* c = ThemePart(g_customTheme, i); fprintf(f, " %d %d %d", c->r, c->g, c->b); }
        fprintf(f, "\n");
        fclose(f);
    };
    auto loadControls = [&]() {
        FILE* f = fopen("controls.cfg", "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            int a, b;
            char s[256];
            int v[18];
            if (sscanf(line, "gpmap %d %d", &a, &b) == 2 && a >= 0 && a < GA_COUNT) gpMap[a] = b;
            else if (sscanf(line, "cam %255s", s) == 1) camDevice = s;
            else if (sscanf(line, "mic %255s", s) == 1) micDevice = s;
            else if (sscanf(line, "phonecam %255s %d", s, &a) == 2) { g_ipcamPort = a > 0 ? a : 8080; g_ipcam.setHost(s, g_ipcamPort); }
            else if (sscanf(line, "theme %d", &a) == 1 && a >= 0 && a <= kThemeCount) { g_themeIdx = a; }
            else if (sscanf(line, "phoneroute %d", &a) == 1 && a >= 0 && a <= 1) { g_phoneRoute = a; }
            else if (sscanf(line, "phonerec %d", &a) == 1) { allowPhoneRec = (a != 0); }
            else if (sscanf(line, "recaskwhere %d", &a) == 1) { recAskWhere = (a != 0); }
            else if (sscanf(line, "customtheme %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                            &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],
                            &v[9],&v[10],&v[11],&v[12],&v[13],&v[14],&v[15],&v[16],&v[17]) == 18) {
                for (int i = 0; i < 6; i++) { Color* c = ThemePart(g_customTheme, i);
                    c->r = (unsigned char)v[i*3]; c->g = (unsigned char)v[i*3+1]; c->b = (unsigned char)v[i*3+2]; c->a = 255; }
            }
        }
        fclose(f);
        // Migración: VDO.Ninja creaba una cámara y un micrófono VIRTUALES y los
        // dejaba guardados aquí. Al quitarlo esos dispositivos ya no aparecen
        // nunca, y un controls.cfg viejo seguía apuntándoles: grabar fallaba con
        // un error de ffmpeg que no dice nada. Se vuelve a lo de siempre.
        if (micDevice == "pinguus_vdo.monitor") micDevice = "default";
#if !defined(_WIN32)
        if (camDevice.rfind("/dev/video", 0) == 0 && !FileExists(camDevice.c_str()))
            camDevice = "/dev/video0";
#endif
        ApplyTheme();
    };
    loadControls();

    // El enlace con el teléfono escucha DESDE EL ARRANQUE. Antes había que
    // acordarse de pulsar "Start UDP link" antes de que la app del móvil
    // pudiera hablar; ahora la app hace un broadcast de descubrimiento, Pinguus
    // le contesta con el nombre del PC y el usuario no teclea ninguna IP.
    // Es un socket UDP ocioso: no cuesta nada tenerlo abierto.
    g_phone.start(kPhonePort);

    // Se comprueba UNA vez al arrancar y el aviso se pinta encima de todo
    // mientras falte: es la causa número uno de "he metido un vídeo y no pasa
    // nada", y antes sólo se sabía leyendo una consola que en Windows no hay.
    DetectFfmpeg();

    // Estado del tailnet una vez, en segundo plano: así el panel DEV -> PHONE
    // enseña la verdad la primera vez que se abre, en vez de "comprobando...".
    // Son dos órdenes de sólo lectura (`tailscale --version` y `status`) en un
    // hilo aparte; si Tailscale no está instalado fallan al instante.
    g_tailscale.refreshAsync();

    int noteyScroll = 0;
    bool liveOn = false;
    float liveAccum = 0.0f;

#ifdef UI_SMOKE_TEST
    bool startupOpen = false;
#else
    bool startupOpen = true;
#endif

    // Modelos 3D como "contenido" pintable: activeBar==2 usa un slot virtual de
    // modelo (id >= MODEL_SLOT_BASE). selectedModel = qué modelo muestra sus
    // animaciones; selectedModelAnim = el id modelo+animación elegido para pintar.
    int selectedModel = -1;
    int selectedModelAnim = -1;
    auto activeSlot = [&]() {
        if (activeBar == 2 && selectedModelAnim >= MODEL_SLOT_BASE) return selectedModelAnim;
        return activeBar == 1 ? selectedSampleSlot : selectedClipSlot;
    };

    // Clips -> "1".."64"; samples -> "A1".."D16"; modelos -> "Mm:a".
    auto slotLabel = [&](int slot) -> const char* {
        if (isModelSlot(slot)) return TextFormat("M%d:%d", modelOfId(slot), animOfId(slot));
        if (slot >= SAMPLE_BASE) {
            int idx = slot - SAMPLE_BASE;
            return TextFormat("%c%d", 'A' + idx / PAGE_SIZE, idx % PAGE_SIZE + 1);
        }
        return TextFormat("%d", slot + 1);
    };

    // --- Editor de recorte (video o audio) ---
    bool editorOpen = false;
    bool editorIsAudio = false;
    int editorSlot = 0;
    int edStart = 0, edEnd = 1; // frames de video o muestras PCM (audio)
    float edCursor = 0.0f;
    int edDrag = 0;
    bool edMoveDragging = false;
    bool edPlaying = false; // el editor está reproduciendo audio (play/stop)
    std::vector<float> edWaveMin, edWaveMax; // forma de onda cacheada (audio)

    // --- Grabación / exportación ---
    bool recording = false;
    bool recVideo = false;
    FILE* recPipe = nullptr;
    float recAccum = 0.0f;
    float recSeconds = 0.0f;
    char recAudioPath[256] = "";
    char recVideoTmp[256] = "";
    char recFinalPath[256] = "";
    bool recChoiceOpen = false;
    bool recChoiceVideo = false;
    // Al cerrar: se pregunta qué hacer con el proyecto y con lo grabado. El
    // bucle principal ya no termina por WindowShouldClose(), sino por quitNow.
    bool quitDialogOpen = false;
    bool quitNow = false;
    // Empty-slot chooser: load a file OR record from camera/mic.
    bool slotChoiceOpen = false;
    int slotChoiceSlot = 0;
    bool slotChoiceVideo = false;
    int camRecSeconds = 4;

    // Live preview of the phone camera (DEV -> PHONE panel).
    Texture2D phonePrevTex = {0};
    uint64_t phonePrevVer = 0;

    // --- Undo / Redo ---
    struct UndoState {
        std::vector<MirrorCell> cells;
        std::vector<BugSpawn> bugs;
    };
    std::vector<UndoState> undoStack, redoStack;
    const size_t kMaxUndo = 40;

    std::unordered_map<int, Texture2D> textureCache;
    // Cached waveform envelope per sample slot (keyed to totalFrames so it
    // recomputes if the slot is reloaded/trimmed). For the hover preview.
    std::unordered_map<int, std::pair<unsigned long long, std::vector<float>>> smpEnvCache;

    auto createVideoTexture = [](int w, int h, int channels) -> Texture2D {
        int fmt = channels == 4 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        Image img;
        img.data = calloc((size_t)w * h * channels, 1);
        img.width = w;
        img.height = h;
        img.mipmaps = 1;
        img.format = fmt;
        Texture2D tex = LoadTextureFromImage(img);
        UnloadImage(img);
        return tex;
    };

    // Returns a cached texture matching the slot's current size AND pixel
    // format (RGB video vs RGBA image/GIF); recreates it if either changed.
    // Qué (slot, fotograma) hay subido YA en la textura de cada slot. El collage
    // recorre las voces activas en cada fotograma de dibujo y subía la imagen
    // siempre; con vídeo a 30 fps y pantalla a 60, la mitad de esas subidas
    // mandaban a la GPU exactamente los mismos píxeles. Y con varias voces del
    // mismo clip en la misma posición, otra vez lo mismo por cada una.
    //
    // Se sigue subiendo cuando el fotograma cambia de verdad, así que dos voces
    // del mismo slot en puntos distintos siguen saliendo cada una con el suyo.
    std::unordered_map<int, int> textureFrame;   // slot -> fotograma subido
    auto uploadSlotFrame = [&](Texture2D& tex, int slot, int frameIdx,
                               const InstrumentSource& src) {
#ifdef UI_SMOKE_TEST
        g_uploadsAsked++;
#endif
        auto it = textureFrame.find(slot);
        if (it != textureFrame.end() && it->second == frameIdx) return;   // ya está
#ifdef UI_SMOKE_TEST
        g_uploadsDone++;
#endif
        UpdateTexture(tex, src.videoFrames[frameIdx].rgb);
        textureFrame[slot] = frameIdx;
    };

    auto getOrCreateTexture = [&](int sampleId, int w, int h, int channels) -> Texture2D& {
        int fmt = channels == 4 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        auto it = textureCache.find(sampleId);
        if (it != textureCache.end()) {
            if (it->second.width == w && it->second.height == h && it->second.format == fmt)
                return it->second;
            UnloadTexture(it->second);
            textureCache.erase(it);
        }
        textureFrame.erase(sampleId);   // textura nueva: lo subido ya no vale
        textureCache[sampleId] = createVideoTexture(w, h, channels);
        return textureCache[sampleId];
    };

    auto setTempo = [&](float bpm) {
        if (bpm < 20.0f) bpm = 20.0f;
        if (bpm > 300.0f) bpm = 300.0f;
        bpmDisplay = bpm;
        RequestSetTempo(bpmDisplay);
    };

    auto clipState = [&](int slot) -> int {
        const InstrumentSource& src = g_engine.getInstrumentBank().at(slot);
        if (src.hasVideo()) return 2;
        if (src.hasAudio()) return 1;
        return 0;
    };

    auto bankRamMB = [&]() -> float {
        size_t total = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            const InstrumentSource& s = g_engine.getInstrumentBank().at(i);
            total += s.videoFrames.size() * (size_t)s.videoWidth * (size_t)s.videoHeight * s.videoChannels;
            total += (size_t)s.audio.totalFrames * sizeof(float);
        }
        return (float)total / (1024.0f * 1024.0f);
    };

    // Hover preview popup (above the toolbar): a frame for clips/images/GIFs,
    // a waveform for samples. Lets you see what a slot holds without loading it.
    auto drawSlotPreview = [&](int slot, int mx) {
        const InstrumentSource& src = g_engine.getInstrumentBank().at(slot);
        const int boxW = 210;
        int boxX = mx - boxW / 2;
        if (boxX < 4) boxX = 4;
        if (boxX + boxW > leftPanelWidth - 4) boxX = leftPanelWidth - 4 - boxW;

        if (src.hasVideo()) {
            const int boxH = 132;
            int boxY = row1Y - boxH - 6;
            DrawRectangle(boxX, boxY, boxW, boxH, (Color){0, 0, 0, 235});
            DrawRectangleLines(boxX, boxY, boxW, boxH, (Color){120, 120, 150, 255});
            Texture2D& tex = getOrCreateTexture(slot, src.videoWidth, src.videoHeight, src.videoChannels);
            uploadSlotFrame(tex, slot, 0, src);
            float availW = boxW - 8, availH = boxH - 24;
            float s = availW / tex.width;
            if (tex.height * s > availH) s = availH / tex.height;
            float dw = tex.width * s, dh = tex.height * s;
            DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height},
                           {boxX + (boxW - dw) / 2, (float)boxY + 4, dw, dh}, {0, 0}, 0.0f, WHITE);
            const char* kind = src.hasAudio() ? "video" : (src.videoFrames.size() > 1 ? "clip/gif" : "image");
            DrawText(TextFormat("%s  %s  %dx%d", slotLabel(slot), kind, src.videoWidth, src.videoHeight),
                     boxX + 6, boxY + boxH - 16, 11, RAYWHITE);
        } else if (src.hasAudio()) {
            const int boxH = 78;
            int boxY = row1Y - boxH - 6;
            DrawRectangle(boxX, boxY, boxW, boxH, (Color){0, 0, 0, 235});
            DrawRectangleLines(boxX, boxY, boxW, boxH, (Color){120, 120, 150, 255});
            int cols = boxW - 10;
            auto& entry = smpEnvCache[slot];
            if (entry.first != src.audio.totalFrames || (int)entry.second.size() != cols) {
                entry.first = src.audio.totalFrames;
                entry.second.assign(cols, 0.0f);
                for (int c = 0; c < cols; c++) {
                    unsigned long long a = src.audio.totalFrames * c / cols;
                    unsigned long long b = src.audio.totalFrames * (c + 1) / cols;
                    if (b <= a) b = a + 1;
                    if (b > src.audio.totalFrames) b = src.audio.totalFrames;
                    float peak = 0.0f;
                    for (unsigned long long s2 = a; s2 < b; s2++) {
                        float v = fabsf(src.audio.pPCM[s2]);
                        if (v > peak) peak = v;
                    }
                    entry.second[c] = peak;
                }
            }
            float midY = boxY + 4 + (boxH - 24) / 2.0f;
            for (int c = 0; c < cols; c++) {
                float hh = entry.second[c] * ((boxH - 24) / 2.0f);
                DrawLine(boxX + 5 + c, (int)(midY - hh), boxX + 5 + c, (int)(midY + hh), (Color){110, 190, 240, 255});
            }
            DrawText(TextFormat("%s  sample  %.2fs", slotLabel(slot), src.audio.totalFrames / 44100.0f),
                     boxX + 6, boxY + boxH - 16, 11, RAYWHITE);
        }
    };

    auto loadFileIntoSlot = [&](int slot, const char* path) -> bool {
        BeginDrawing();
        ClearBackground(g_theme.bg);
        const char* loadingMsg = "Loading file... decoding to RAM";
        DrawText(loadingMsg, (screenWidth - MeasureText(loadingMsg, 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
        EndDrawing();

        ma_device_stop(&device);
        bool ok = IsAudioFile(path)
                      ? g_engine.getInstrumentBank().LoadAudioOnly(slot, path)
                      : IsImageFile(path)
                          ? LoadImageIntoSlot(slot, path, g_transcodeMaxSide)
                          : g_engine.getInstrumentBank().LoadVideo(slot, path, true);
        ma_device_start(&device);

        if (ok) {
            g_slotPath[slot] = path;
            g_slotTrimStart[slot] = 0;
            g_slotTrimLen[slot] = 0;
            g_slotVisualPath[slot].clear(); // a fresh load drops any visual override
            auto it = textureCache.find(slot);
            if (it != textureCache.end()) {
                UnloadTexture(it->second);
                textureCache.erase(it);
            }
        }
        return ok;
    };

    // Records `camRecSeconds` from the webcam (+mic) or just the mic via
    // ffmpeg into a timestamped file, then loads it into the slot (clip =
    // audio+video, sample = audio only). Blocking capture (UI shows a notice).
    auto recordCamMic = [&](int slot, bool withVideo) {
        int secs = camRecSeconds;

        // ¿Puede alguien más tener la cámara cogida? Preguntárselo al sistema
        // ANTES vale más que el "Device or resource busy" que soltaba ffmpeg,
        // que no dice quién la tiene ni qué hacer. Pasa de verdad: una grabación
        // anterior que se quedó colgada sigue reteniendo el dispositivo, y todos
        // los intentos siguientes fallan sin explicación hasta reiniciar.
#if !defined(_WIN32) && !defined(__APPLE__)
        if (withVideo && camDevice.rfind("/dev/", 0) == 0) {
            int fd = open(camDevice.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0 && errno == EBUSY) {
                // EBUSY en una webcam virtual (v4l2loopback con exclusive_caps=1)
                // significa DOS cosas muy distintas, y decir la que no es manda
                // al usuario a buscar donde no hay nada:
                //   * hay otro programa leyéndola  -> hay que soltarla;
                //   * no hay NADIE emitiendo aún   -> sólo hay que esperar.
                // Se distinguen mirando quién la tiene abierta aparte de
                // nosotros; el productor (GStreamer) no cuenta como lector.
                std::string who;
                RunCommandRead(("fuser " + camDevice + " 2>/dev/null").c_str(), who);
                if (who.empty())
                    SetStatus("Nothing is being sent to %s yet - it is a virtual camera and "
                              "no program is feeding it; start its source, then record",
                              camDevice.c_str());
                else
                    SetStatus("%s is busy - another program has it open%s%s",
                              camDevice.c_str(),
                              who.empty() ? "" : " (PIDs:", who.empty() ? "" : who.c_str());
                printf("Pinguus: %s esta ocupado (EBUSY). Quien lo tiene abierto:\n", camDevice.c_str());
                fflush(stdout);
                RunCommand(("fuser -v " + camDevice + " 2>&1").c_str());
                return;
            }
            if (fd >= 0) close(fd);
        }
#endif

        char ts[32];
        time_t t = time(nullptr);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&t));
        char outPath[256];
        snprintf(outPath, sizeof(outPath), "%s",
                 TempPath(TextFormat(withVideo ? "pinguus_rec_%s.mp4" : "pinguus_rec_%s.wav", ts)));

        char cmd[768];
#if defined(_WIN32)
        // Los nombres dshow varían en cada PC, así que vienen del panel DEV
        // (refreshMicSources los enumera con ffmpeg). Si el usuario no ha
        // elegido nada todavía, no inventamos un nombre: se lo decimos.
        if (withVideo && camDevice.empty()) {
            SetStatus("Pick a camera first: DEV -> PHONE / CAMERA / MIC");
            return;
        }
        if (micDevice.empty()) {
            SetStatus("Pick a microphone first: DEV -> PHONE / CAMERA / MIC");
            return;
        }
        if (withVideo)
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f dshow -i video=\"%s\":audio=\"%s\" -t %d -c:v libx264 -preset veryfast -pix_fmt yuv420p \"%s\" -loglevel error", camDevice.c_str(), micDevice.c_str(), secs, outPath);
        else
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f dshow -i audio=\"%s\" -t %d \"%s\" -loglevel error", micDevice.c_str(), secs, outPath);
#elif defined(__APPLE__)
        if (withVideo)
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f avfoundation -framerate 30 -i \"0:0\" -t %d -c:v libx264 -preset veryfast -pix_fmt yuv420p \"%s\" -loglevel error", secs, outPath);
        else
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f avfoundation -i \":0\" -t %d \"%s\" -loglevel error", secs, outPath);
#else
        // Dispositivos elegibles en el panel DEV (por defecto /dev/video0 y "default").
        if (withVideo)
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f v4l2 -framerate 30 -video_size 640x480 -i %s -f pulse -i %s -t %d -c:v libx264 -preset veryfast -pix_fmt yuv420p '%s' -loglevel error", camDevice.c_str(), micDevice.c_str(), secs, outPath);
        else
            snprintf(cmd, sizeof(cmd), "ffmpeg -y -f pulse -i %s -t %d '%s' -loglevel error", micDevice.c_str(), secs, outPath);
#endif

        BeginDrawing();
        ClearBackground(g_theme.bg);
        const char* rmsg = withVideo ? TextFormat("Recording %ds from CAMERA + MIC...", secs)
                                     : TextFormat("Recording %ds from MIC...", secs);
        DrawText(rmsg, (screenWidth - MeasureText(rmsg, 22)) / 2, screenHeight / 2 - 11, 22, (Color){255, 120, 120, 255});
        EndDrawing();

        ma_device_stop(&device);
        // Tope de reloj: secs de grabación + margen para que ffmpeg cierre el
        // archivo. Sin esto, una cámara que no entrega fotogramas deja a ffmpeg
        // esperando indefinidamente Y a Pinguus congelado dentro de la llamada.
        int ret = RunCommandTimeout(cmd, secs + 12);
        bool ok = false;
        if (ret == 0 && FileExists(outPath)) {
            ok = withVideo ? g_engine.getInstrumentBank().LoadVideo(slot, outPath, true)
                           : g_engine.getInstrumentBank().LoadAudioOnly(slot, outPath);
        }
        ma_device_start(&device);

        if (ok) {
            g_slotPath[slot] = outPath;
            g_slotVisualPath[slot].clear();
            g_slotTrimStart[slot] = 0;
            g_slotTrimLen[slot] = 0;
            RegisterSessionClip(outPath);
            auto it = textureCache.find(slot);
            if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
            SetStatus("Recorded %ds into %s (saved as %s)", secs, slotLabel(slot), outPath);
        } else {
            remove(outPath);
            if (ret == RUNCMD_TIMED_OUT)
                SetStatus("No video arrived from %s - is anything actually sending? (recording cancelled)",
                          camDevice.c_str());
            else
                SetStatus("Camera/mic record failed - check the device (see console)");
        }
    };

    // Records `camRecSeconds` from the PHONE's live UDP stream (camera + mic
    // sent by phone/pinguus_phone.py) into a slot. Shows the live preview while
    // capturing, then muxes the accumulated JPEG frames + PCM into a file with
    // ffmpeg and loads it — reusing the same LoadVideo/LoadAudioOnly path as the
    // local recorder. Click / ESC stops early.
    // Records from a phone running an MJPEG app (IP Webcam & co): ffmpeg reads
    // the /video + /audio.wav URLs directly, so there's nothing to reassemble.
    // Runs ffmpeg on a worker thread so the live preview keeps drawing.
    auto recordFromIpCam = [&](int slot, bool withVideo) {
        int secs = camRecSeconds;
        char ts[32]; time_t t = time(nullptr);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&t));
        char outPath[256];
        snprintf(outPath, sizeof(outPath), "%s",
                 TempPath(TextFormat(withVideo ? "pinguus_phone_%s.mp4" : "pinguus_phone_%s.wav", ts)));
        std::string vurl = g_ipcam.videoUrl(), aurl = g_ipcam.audioUrl();

        char cmd[1024];
        if (withVideo)
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y -i %s -i %s -t %d -c:v libx264 -preset veryfast -pix_fmt yuv420p "
                "-c:a aac -shortest %s -loglevel error",
                ShQuote(vurl).c_str(), ShQuote(aurl).c_str(), secs, ShQuote(outPath).c_str());
        else
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y -i %s -t %d %s -loglevel error",
                ShQuote(aurl).c_str(), secs, ShQuote(outPath).c_str());

        std::atomic<bool> done{false};
        std::atomic<int> rc{-1};
        std::string cmdStr = cmd;
        std::thread worker([&done, &rc, cmdStr] { rc = RunCommand(cmdStr.c_str()); done = true; });

        double t0 = GetTime();
        Texture2D prev = {0}; uint64_t pv = 0;
        while (!done) {
            std::vector<uint8_t> jpg; uint64_t v = 0;
            if (withVideo && g_ipcam.latestFrame(jpg, v) && v != pv) {
                pv = v;
                Image im = LoadImageFromMemory(".jpg", jpg.data(), (int)jpg.size());
                if (im.data) { UpdatePreviewTex(prev, im); UnloadImage(im); }
            }
            BeginDrawing();
            ClearBackground(g_theme.bg);
            if (prev.id) {
                float pw = (float)prev.width, ph = (float)prev.height;
                float sc = fminf((screenWidth * 0.6f) / pw, (screenHeight * 0.6f) / ph);
                Rectangle dst = {(screenWidth - pw * sc) / 2, (screenHeight - ph * sc) / 2 - 20, pw * sc, ph * sc};
                DrawTexturePro(prev, {0, 0, pw, ph}, dst, {0, 0}, 0.0f, WHITE);
                DrawRectangleLinesEx(dst, 2, (Color){120, 230, 140, 255});
            }
            int remain = secs - (int)(GetTime() - t0);
            if (remain < 0) remain = 0;
            const char* rmsg = withVideo ? TextFormat("Recording %ds from PHONE app (camera + mic)... %d", secs, remain)
                                         : TextFormat("Recording %ds from PHONE app (mic)... %d", secs, remain);
            DrawText(rmsg, (screenWidth - MeasureText(rmsg, 22)) / 2, (int)(screenHeight * 0.85f), 22, (Color){255, 150, 150, 255});
            EndDrawing();
        }
        worker.join();
        if (prev.id) UnloadTexture(prev);

        bool ok = false;
        ma_device_stop(&device);
        if (rc == 0 && FileExists(outPath))
            ok = withVideo ? g_engine.getInstrumentBank().LoadVideo(slot, outPath, true)
                           : g_engine.getInstrumentBank().LoadAudioOnly(slot, outPath);
        ma_device_start(&device);

        if (ok) {
            g_slotPath[slot] = outPath;
            g_slotVisualPath[slot].clear();
            g_slotTrimStart[slot] = 0;
            g_slotTrimLen[slot] = 0;
            RegisterSessionClip(outPath);
            auto it = textureCache.find(slot);
            if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
            SetStatus("Recorded %ds from phone app into %s (%s)", secs, slotLabel(slot), outPath);
        } else {
            remove(outPath);
            SetStatus("Phone app record failed - is the stream still running? (DEV -> PHONE)");
        }
    };

    // `phoneKey` = 0 means "whichever phone is selected in the panel". A phone
    // that asks for its OWN recording passes its key instead, so it records
    // itself even while the PC is previewing somebody else — which is the
    // whole point of letting the phone drive.
    // `secs` = 0 means "use the length chosen on the PC".
    auto recordFromPhoneDev = [&](int slot, bool withVideo, uint64_t phoneKey, int secsWanted) {
        // Prefer the MJPEG app if it's connected (nothing to install on the
        // phone) — but only for recordings the PC started. A request that came
        // FROM a phone must record THAT phone, never some other camera.
        if (phoneKey == 0 && g_ipcam.connected()) { recordFromIpCam(slot, withVideo); return; }
        if (!g_phone.isRunning()) g_phone.start(kPhonePort);
        if (phoneKey == 0 && !g_phone.connected()) {
            SetStatus("No phone streaming yet - open DEV -> PHONE / CAMERA / MIC");
            return;
        }
        int secs = secsWanted > 0 ? secsWanted : camRecSeconds;
        g_phone.startRecording(phoneKey);
        double t0 = GetTime();
        Texture2D prev = {0}; uint64_t pv = 0;
        int lastRemainSent = -1;
        while (GetTime() - t0 < secs) {
            std::vector<uint8_t> jpg; uint64_t v = 0;
            if (withVideo && (phoneKey ? g_phone.latestFrameOf(phoneKey, jpg, v)
                                     : g_phone.latestFrame(jpg, v)) && v != pv) {
                pv = v;
                Image im = LoadImageFromMemory(".jpg", jpg.data(), (int)jpg.size());
                if (im.data) { UpdatePreviewTex(prev, im); UnloadImage(im); }
            }
            BeginDrawing();
            ClearBackground(g_theme.bg);
            if (prev.id) {
                float pw = (float)prev.width, ph = (float)prev.height;
                float sc = fminf((screenWidth * 0.6f) / pw, (screenHeight * 0.6f) / ph);
                Rectangle dst = {(screenWidth - pw * sc) / 2, (screenHeight - ph * sc) / 2 - 20, pw * sc, ph * sc};
                DrawTexturePro(prev, {0, 0, pw, ph}, dst, {0, 0}, 0.0f, WHITE);
                DrawRectangleLinesEx(dst, 2, (Color){120, 230, 140, 255});
            }
            int remain = secs - (int)(GetTime() - t0);
            // Cuenta atrás EN EL MÓVIL, para quien graba con el teléfono en la
            // mano y no ve la pantalla del PC. Sólo cuando el segundo cambia:
            // mandarlo cada fotograma serían 60 paquetes por segundo para nada.
            if (phoneKey && remain != lastRemainSent) {
                lastRemainSent = remain;
                g_phone.sendRecStatus(phoneKey, PhoneLink::kStatRecording, remain, 0, slot, "");
            }
            const char* rmsg = withVideo ? TextFormat("Recording %ds from PHONE (camera + mic)... %d", secs, remain)
                                         : TextFormat("Recording %ds from PHONE (mic)... %d", secs, remain);
            DrawText(rmsg, (screenWidth - MeasureText(rmsg, 22)) / 2, (int)(screenHeight * 0.85f), 22, (Color){255, 150, 150, 255});
            DrawText("click or ESC to stop early", (screenWidth - MeasureText("click or ESC to stop early", 14)) / 2, (int)(screenHeight * 0.85f) + 30, 14, (Color){170, 170, 190, 255});
            EndDrawing();
            if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || WindowShouldClose()) break;
        }
        if (prev.id) UnloadTexture(prev);

        std::vector<std::vector<uint8_t>> frames;
        std::vector<int16_t> audio; double fps = 15.0;
        g_phone.stopRecording(frames, audio, fps);
        if (fps < 1.0) fps = 1.0;
        if (fps > 60.0) fps = 60.0;

        char ts[32]; time_t t = time(nullptr);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&t));
        char outPath[256];
        snprintf(outPath, sizeof(outPath), "%s",
                 TempPath(TextFormat(withVideo ? "pinguus_phone_%s.mp4" : "pinguus_phone_%s.wav", ts)));
        std::string pcmPath = TempPath(TextFormat("pinguus_phone_%s.pcm", ts));

        // Write the captured PCM to a temp raw file (mono s16le @44100).
        bool haveAudio = !audio.empty();
        if (haveAudio) {
            FILE* af = fopen(pcmPath.c_str(), "wb");
            if (af) { fwrite(audio.data(), sizeof(int16_t), audio.size(), af); fclose(af); }
            else haveAudio = false;
        }

        bool ok = false;
        if (withVideo && !frames.empty()) {
            // Feed the JPEG frames to ffmpeg via image2pipe; mux with the PCM.
            char cmd[1024];
            // -f mjpeg (NOT image2pipe) reads the concatenated JPEG frames as a
            // proper MJPEG video stream from stdin.
            if (haveAudio)
                snprintf(cmd, sizeof(cmd),
                    "ffmpeg -y -f mjpeg -framerate %.3f -i - -f s16le -ar 44100 -ac 1 -i %s "
                    "-c:v libx264 -preset veryfast -pix_fmt yuv420p -shortest %s -loglevel error",
                    fps, ShQuote(pcmPath).c_str(), ShQuote(outPath).c_str());
            else
                snprintf(cmd, sizeof(cmd),
                    "ffmpeg -y -f mjpeg -framerate %.3f -i - -c:v libx264 -preset veryfast -pix_fmt yuv420p %s -loglevel error",
                    fps, ShQuote(outPath).c_str());
            BeginDrawing(); ClearBackground(g_theme.bg);
            DrawText("Encoding phone clip...", (screenWidth - MeasureText("Encoding phone clip...", 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
            EndDrawing();
            FILE* pipe = POPEN(cmd, POPEN_WB);
            if (pipe) {
                for (auto& f : frames) fwrite(f.data(), 1, f.size(), pipe);
                int rc = PCLOSE(pipe);
                (void)rc;
                ma_device_stop(&device);
                if (FileExists(outPath)) ok = g_engine.getInstrumentBank().LoadVideo(slot, outPath, true);
                ma_device_start(&device);
            }
        } else if (!withVideo && haveAudio) {
            char cmd[768];
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y -f s16le -ar 44100 -ac 1 -i %s %s -loglevel error",
                ShQuote(pcmPath).c_str(), ShQuote(outPath).c_str());
            int ret = RunCommand(cmd);
            ma_device_stop(&device);
            if (ret == 0 && FileExists(outPath)) ok = g_engine.getInstrumentBank().LoadAudioOnly(slot, outPath);
            ma_device_start(&device);
        }
        remove(pcmPath.c_str());

        if (ok) {
            g_slotPath[slot] = outPath;
            g_slotVisualPath[slot].clear();
            g_slotTrimStart[slot] = 0;
            g_slotTrimLen[slot] = 0;
            RegisterSessionClip(outPath);
            auto it = textureCache.find(slot);
            if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
            SetStatus("Recorded %ds from phone into %s (%s)", secs, slotLabel(slot), outPath);
            if (phoneKey)
                g_phone.sendRecStatus(phoneKey, PhoneLink::kStatDone, 0, 0, slot,
                                      std::string("Saved into ") + slotLabel(slot));
        } else {
            remove(outPath);
            SetStatus("Phone record failed - got %d frames, %zu audio samples", (int)frames.size(), audio.size());
            if (phoneKey)
                g_phone.sendRecStatus(phoneKey, PhoneLink::kStatFailed, 0, 0, slot,
                                      frames.empty() && audio.empty()
                                          ? "Nothing arrived - is the phone still streaming?"
                                          : "The PC could not encode the take (is ffmpeg installed?)");
        }
    };

    // Lo de siempre: graba del móvil SELECCIONADO, con la duración elegida en
    // el PC. Es lo que usan el selector de slot vacío y el botón del panel.
    auto recordFromPhone = [&](int slot, bool withVideo) {
        recordFromPhoneDev(slot, withVideo, 0, 0);
    };

    // Swaps a slot's visual for an image/GIF, keeping its audio. Opens a file
    // dialog. Used from the clip and sample editors ("replace/set visual").
    auto replaceSlotVisual = [&](int slot) {
        char path[512] = "";
        int r = NativeOpenDialog("Replace visual with image / GIF...",
                                 "*.gif *.png *.jpg *.jpeg *.bmp", path, sizeof(path));
        if (r != 1) return;
        ma_device_stop(&device);
        bool ok = ReplaceVisualIntoSlot(slot, path, g_transcodeMaxSide);
        ma_device_start(&device);
        if (ok) {
            g_slotVisualPath[slot] = path;
            auto it = textureCache.find(slot);
            if (it != textureCache.end()) {
                UnloadTexture(it->second);
                textureCache.erase(it);
            }
            SetStatus("Visual replaced with %s (audio kept)", GetFileName(path));
        } else {
            SetStatus("Could not load %s", GetFileName(path));
        }
    };

    // Undo a visual override: a CLIP reloads its original video/image (and
    // re-applies its trim); a SAMPLE just drops the added picture, keeps audio.
    auto revertSlotVisual = [&](int slot) {
        if (g_slotVisualPath[slot].empty()) {
            SetStatus("This slot has no image/GIF override to remove");
            return;
        }
        g_slotVisualPath[slot].clear();
        ma_device_stop(&device);
        if (slot >= SAMPLE_BASE) {
            g_engine.getInstrumentBank().ClearVisual(slot);
            ma_device_start(&device);
            SetStatus("Visual removed from sample %s", slotLabel(slot));
        } else {
            bool ok = false;
            if (!g_slotPath[slot].empty()) {
                ok = IsImageFile(g_slotPath[slot].c_str())
                         ? LoadImageIntoSlot(slot, g_slotPath[slot].c_str(), g_transcodeMaxSide)
                         : g_engine.getInstrumentBank().LoadVideo(slot, g_slotPath[slot].c_str(), true);
                if (ok && g_slotTrimLen[slot] > 0 && !IsImageFile(g_slotPath[slot].c_str())) {
                    g_engine.getInstrumentBank().SetVideoTrim(slot, g_slotTrimStart[slot],
                                                             g_slotTrimStart[slot] + g_slotTrimLen[slot]);
                }
            }
            ma_device_start(&device);
            SetStatus(ok ? "Restored original visual of clip %s" : "Could not restore clip %s", slotLabel(slot));
        }
        auto it = textureCache.find(slot);
        if (it != textureCache.end()) {
            UnloadTexture(it->second);
            textureCache.erase(it);
        }
    };

    // Load a Pure Data / PlugData (.pd) patch as this slot's insert effect.
    // Opens a file dialog; audio device stopped while the DSP graph builds.
    auto loadPdEffect = [&](int slot) {
        char path[512] = "";
        int r = NativeOpenDialog("Load Pure Data / PlugData effect (.pd)...", "*.pd", path, sizeof(path));
        if (r != 1) return;
        ma_device_stop(&device);
        bool ok = g_engine.getPd().loadPatch(slot, path);
        ma_device_start(&device);
        if (ok) {
            g_slotPdPath[slot] = path;
            SetStatus("Pd effect loaded on %s: %s", slotLabel(slot), GetFileName(path));
        } else {
            SetStatus("Could not load Pd patch %s", GetFileName(path));
        }
    };

    auto clearPdEffect = [&](int slot) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        if (g_slotPdPath[slot].empty()) { SetStatus("No Pd effect on this slot"); return; }
        ma_device_stop(&device);
        g_engine.getPd().clearPatch(slot);
        ma_device_start(&device);
        g_slotPdPath[slot].clear();
        SetStatus("Pd effect removed from %s", slotLabel(slot));
    };

    // Compile a GLSL fragment shader from disk onto a slot's video. Returns
    // false if the file can't be compiled/linked. Caches the two custom
    // uniform locations (time, resolution) for cheap per-frame updates.
    auto loadShaderFromFile = [&](int slot, const char* path) -> bool {
        Shader sh = LoadShader(0, path); // default vertex + this fragment
        if (sh.id == 0) return false;    // compile/link failed (see console)
        if (g_slotShaderOn[slot]) UnloadShader(g_slotShader[slot]);
        g_slotShader[slot] = sh;
        g_slotShaderOn[slot] = true;
        g_slotShaderPath[slot] = path;
        g_slotShaderLocTime[slot] = GetShaderLocation(sh, "time");
        g_slotShaderLocRes[slot] = GetShaderLocation(sh, "resolution");
        return true;
    };

    auto loadShaderEffect = [&](int slot) {
        char path[512] = "";
        int r = NativeOpenDialog("Load GLSL video shader (.fs/.glsl)...", "*.fs *.glsl *.frag", path, sizeof(path));
        if (r != 1) return;
        if (loadShaderFromFile(slot, path))
            SetStatus("GLSL shader on %s: %s", slotLabel(slot), GetFileName(path));
        else
            SetStatus("Shader failed to compile: %s (see console)", GetFileName(path));
    };

    auto clearShaderEffect = [&](int slot) {
        if (!g_slotShaderOn[slot]) { SetStatus("No shader on this slot"); return; }
        UnloadShader(g_slotShader[slot]);
        g_slotShaderOn[slot] = false;
        g_slotShaderPath[slot].clear();
        SetStatus("Shader removed from %s", slotLabel(slot));
    };

    // Empties a slot back to blank: unloads its media, Pd patch, shader, visual
    // override, trim and clip-FX, and drops its cached texture — so it reads as
    // an empty slot again (click to load/record something new). Cells/noteys
    // still pointing at it simply play nothing.
    auto clearSlot = [&](int slot) {
        ma_device_stop(&device);
        g_engine.getInstrumentBank().Clear(slot);
        g_engine.getPd().clearPatch(slot);
        ma_device_start(&device);
        if (g_slotShaderOn[slot]) { UnloadShader(g_slotShader[slot]); g_slotShaderOn[slot] = false; }
        g_slotPath[slot].clear();
        g_slotVisualPath[slot].clear();
        g_slotPdPath[slot].clear();
        g_slotShaderPath[slot].clear();
        g_slotTrimStart[slot] = 0;
        g_slotTrimLen[slot] = 0;
        g_clipFX[slot] = ClipFX();
        auto it = textureCache.find(slot);
        if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
        // Los pads del modo BEATBOX que apuntaban a este slot se quedan
        // vacíos: si no, seguirían pintados como si tuvieran sonido.
        for (int p = 0; p < PAD_TOTAL; p++) {
            if (g_pads[p].slot != slot) continue;
            g_pads[p] = PadConfig();
            RequestSetPadConfig(p);
        }
        SetStatus("Slot %s emptied", slotLabel(slot));
    };

    // Los tres atributos superpuestos viajan en TODA celda, así que una nota
    // puede llevar a la vez efecto, espera, volumen y velocidad; y una celda
    // sin nota con espera es un silencio.
    auto sendCellToEngine = [&](int x, int y, const MirrorCell& mc) {
        const float h = mc.hold, v = mc.vol, t = mc.tmul;
        switch (mc.kind) {
            case CELL_COLOR:
                RequestPaintCell(x, y, SemitoneToColorId(mc.pitchIdx), mc.clip, ModifierType::None, 0, 0, 0, h, v, t);
                break;
            case CELL_ARROW:
                RequestPaintCell(x, y, 0, 0, ModifierType::DirectionChange, kDirDx[mc.dir], kDirDy[mc.dir], 0, h, v, t);
                break;
            case CELL_SUSTAIN:
                // Sólo llega aquí al cargar proyectos antiguos, donde la espera
                // era un modificador. Se manda por el campo nuevo para que la
                // celda pueda además llevar un FX.
                RequestPaintCell(x, y, SemitoneToColorId(mc.pitchIdx), mc.clip, ModifierType::None, 0, 0, 0,
                                 h > 0.0f ? h : mc.sust, v, t);
                break;
            case CELL_MUTE:
                RequestPaintCell(x, y, 0, 0, ModifierType::Silence, 0, 0, 0, h, v, t);
                break;
            case CELL_FX:
                RequestPaintCell(x, y, SemitoneToColorId(mc.pitchIdx), mc.clip, kFxMod[mc.fxType], 0, 0, 0, h, v, t);
                break;
            case CELL_TELEPORT: {
                // Directional A -> B (Portal-style). fxType 0 = entrance A,
                // 1 = exit B. A notey entering A jumps to B; B is just a
                // destination (does nothing on its own).
                if (mc.fxType != 0) { // this is an exit (B): no teleport
                    RequestPaintCell(x, y, 0, 0, ModifierType::None, 0, 0, 0);
                    break;
                }
                int id = mc.dir, tx = -1, ty = -1;
                for (int yy = 0; yy < g_gridH && tx < 0; yy++) {
                    for (int xx = 0; xx < g_gridW; xx++) {
                        const MirrorCell& o = g_mirror[yy * g_gridW + xx];
                        if (o.kind == CELL_TELEPORT && o.dir == id && o.fxType == 1) { tx = xx; ty = yy; break; }
                    }
                }
                if (tx >= 0) RequestTeleportCell(x, y, tx, ty);
                else RequestPaintCell(x, y, 0, 0, ModifierType::None, 0, 0, 0); // A with no B yet
                break;
            }
            case CELL_MODCELL: {
                // Fase 2/3: celda de mod -> lista de primitivas nativas (por-paso).
                if (mc.modCellId < 0 || mc.modCellId >= (int)g_modCells.size()) {
                    RequestPaintCell(x, y, 0, 0, ModifierType::None, 0, 0, 0);
                    break;
                }
                const ModCellDef& d = g_modCells[mc.modCellId];
                unsigned char types[4]; int as[4], bs[4]; float fs[4];
                int nc = 0;
                for (const ModCellAction& a : d.actions) {
                    if (nc >= 4) break;
                    as[nc] = 0; bs[nc] = 0; fs[nc] = 0.0f;
                    switch (a.behavior) {
                        case 1: types[nc] = CA_HOLD; fs[nc] = a.seconds; break;
                        case 2: types[nc] = CA_MUTE; break;
                        case 3: { // teleport por desplazamiento relativo (resuelto a absoluto)
                            int tx = x + a.tdx, ty = y + a.tdy;
                            tx = tx < 0 ? 0 : (tx >= g_gridW ? g_gridW - 1 : tx);
                            ty = ty < 0 ? 0 : (ty >= g_gridH ? g_gridH - 1 : ty);
                            types[nc] = CA_TELEPORT; as[nc] = tx; bs[nc] = ty; break;
                        }
                        case 4: types[nc] = CA_FX; as[nc] = a.fx; break;
                        case 5: types[nc] = CA_NOTE; as[nc] = SemitoneToColorId(a.semitone);
                                bs[nc] = (a.slot < 0 || a.slot >= MAX_SLOTS) ? activeSlot() : a.slot; break;
                        default: types[nc] = CA_TURN; as[nc] = kDirDx[a.dir & 3]; bs[nc] = kDirDy[a.dir & 3]; break;
                    }
                    nc++;
                }
                RequestPaintCompound(x, y, nc, types, as, bs, fs);
                break;
            }
            default:
                // Celda vacía: sin nota. Si lleva espera, es un SILENCIO.
                RequestPaintCell(x, y, 0, 0, ModifierType::None, 0, 0, 0, h, v, t);
                break;
        }
    };

    // Re-send every teleporter of a given id so paired targets stay in sync
    // after any portal is added or removed.
    auto resendTeleGroup = [&](int id) {
        for (int y = 0; y < g_gridH; y++) {
            for (int x = 0; x < g_gridW; x++) {
                const MirrorCell& mc = g_mirror[y * g_gridW + x];
                if (mc.kind == CELL_TELEPORT && mc.dir == id) sendCellToEngine(x, y, mc);
            }
        }
    };

    // Places one portal of an id: first placement is entrance A, second is
    // exit B; a third relocates A. Keeps at most one A and one B per id.
    auto placeTeleport = [&](int x, int y, int id) {
        int prevId = -1;
        {
            const MirrorCell& cur = g_mirror[y * g_gridW + x];
            if (cur.kind == CELL_TELEPORT) prevId = cur.dir;
        }
        int aX = -1, aY = -1, bX = -1;
        for (int yy = 0; yy < g_gridH; yy++) {
            for (int xx = 0; xx < g_gridW; xx++) {
                if (xx == x && yy == y) continue;
                const MirrorCell& o = g_mirror[yy * g_gridW + xx];
                if (o.kind != CELL_TELEPORT || o.dir != id) continue;
                if (o.fxType == 0) { aX = xx; aY = yy; } else { bX = xx; }
            }
        }
        int role;
        if (aX < 0) {
            role = 0;                 // no entrance yet -> A
        } else if (bX < 0) {
            role = 1;                 // have A, need B
        } else {
            role = 0;                 // both exist -> relocate the entrance
            g_mirror[aY * g_gridW + aX] = MirrorCell();
            RequestPaintCell(aX, aY, 0, 0, ModifierType::None, 0, 0, 0);
        }

        MirrorCell& mc = g_mirror[y * g_gridW + x];
        mc = MirrorCell();
        mc.kind = CELL_TELEPORT;
        mc.dir = id;
        mc.fxType = role;

        resendTeleGroup(id);
        if (prevId >= 0 && prevId != id) resendTeleGroup(prevId);
        SetStatus(role == 0 ? "Portal %d: entrance A placed (now place exit B)"
                            : "Portal %d: exit B placed (A -> B ready)", id + 1);
    };

    auto activateScene = [&](int idx) {
        if (idx < 0 || idx >= (int)g_scenes.size()) return;
        g_curScene = idx;
        g_mirror = g_scenes[idx].cells;
        RequestClearBichos();
        RequestClearGrid();
        for (int y = 0; y < g_gridH; y++) {
            for (int x = 0; x < g_gridW; x++) {
                const MirrorCell& mc = g_mirror[y * g_gridW + x];
                if (!cellIsBlank(mc)) sendCellToEngine(x, y, mc);
            }
        }
        for (const BugSpawn& b : g_scenes[idx].bugs) {
            RequestSpawnBicho(b.x, b.y, b.dx, b.dy, b.clip, b.tempoMul, b.muted, b.volume, b.stopped);
        }
        sceneTimeLeft = g_scenes[idx].duration;
    };

    auto captureState = [&]() -> UndoState {
        UndoState s;
        s.cells.assign(g_mirror, g_mirror + MAX_CELLS);
        s.bugs = g_scenes[g_curScene].bugs;
        return s;
    };

    auto pushUndo = [&]() {
        undoStack.push_back(captureState());
        if (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
        redoStack.clear();
    };

    auto restoreState = [&](const UndoState& s) {
        for (int i = 0; i < MAX_CELLS; i++) g_mirror[i] = s.cells[i];
        g_scenes[g_curScene].bugs = s.bugs;
        activateScene(g_curScene);
    };

    auto doUndo = [&]() {
        if (undoStack.empty()) { SetStatus("Nothing to undo"); return; }
        redoStack.push_back(captureState());
        restoreState(undoStack.back());
        undoStack.pop_back();
        SetStatus("Undo");
    };

    auto doRedo = [&]() {
        if (redoStack.empty()) { SetStatus("Nothing to redo"); return; }
        undoStack.push_back(captureState());
        restoreState(redoStack.back());
        redoStack.pop_back();
        SetStatus("Redo");
    };

    auto clearUndoHistory = [&]() {
        undoStack.clear();
        redoStack.clear();
    };

    auto spawnBug = [&](int x, int y, int dx, int dy, int clip, float tempoMul) {
        g_scenes[g_curScene].bugs.push_back({x, y, dx, dy, clip, tempoMul, false, 1.0f});
        RequestSpawnBicho(x, y, dx, dy, clip, tempoMul, false, 1.0f);
    };

    auto deleteBugAt = [&](int cellX, int cellY, const SnapshotBuffer& snapshot) {
        for (int i = 0; i < snapshot.count; i++) {
            const BichoVisual& v = snapshot.bichos[i];
            if (v.isActive && v.x == cellX && v.y == cellY) {
                pushUndo();
                std::vector<BugSpawn>& bugs = g_scenes[g_curScene].bugs;
                if (i < (int)bugs.size()) bugs.erase(bugs.begin() + i);
                activateScene(g_curScene);
                return;
            }
        }
    };

    auto clearPaint = [&]() {
        pushUndo();
        RequestClearGrid();
        for (int i = 0; i < MAX_CELLS; i++) g_mirror[i] = MirrorCell();
        SetStatus("Paint cleared");
    };

    auto clearBugs = [&]() {
        pushUndo();
        g_scenes[g_curScene].bugs.clear();
        RequestClearBichos();
        SetStatus("All noteys removed");
    };

    auto togglePause = [&]() {
        paused = !paused;
        RequestSetPaused(paused);
        SetStatus(paused ? "Paused - PLAY (or SPACE) resumes" : "Playing");
    };

    // semitone is the absolute pitch (0 = original speed).
    auto trackerPlace = [&](int ch, int row, int sample, int semitone, int fx) {
        g_tracker[ch][row] = {sample, semitone, fx};
        RequestTrackerCell(ch, row, sample, sample >= 0 ? SemitoneToColorId(semitone) : 0, fx);
    };

    auto trackerClearAll = [&]() {
        for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
            for (int r = 0; r < TRACKER_ROWS; r++) g_tracker[ch][r] = TrkCell();
        }
        RequestClearTracker();
        SetStatus("Tracker cleared");
    };

    // Coloca (o borra, sample<0) una nota del modo lineal. Funciona como el
    // canvas: el clip viene del slot activo y el TONO del color/nota elegido;
    // ambos quedan pegados a la celda. sample<0 -> celda vacía.
    auto linearPlace = [&](int col, int row, int sample, int pitchIdx, int fx) {
        if (col < 0 || col >= LINEAR_COLS || row < 0 || row >= LINEAR_ROWS) return;
        if (sample < 0) {
            g_linear[col][row] = LinCell();
            RequestLinearCell(col, row, -1, 0, 0);
        } else {
            g_linear[col][row] = {sample, pitchIdx, (unsigned char)fx};
            RequestLinearCell(col, row, sample, SemitoneToColorId(pitchIdx), fx);
        }
    };

    // Aplica/actualiza un efecto FX SOLO sobre celdas que ya tienen nota; en
    // vacías no hace nada (pedido del usuario). Conserva clip y tono.
    auto linearApplyFx = [&](int col, int row, int fx) {
        if (col < 0 || col >= LINEAR_COLS || row < 0 || row >= LINEAR_ROWS) return;
        LinCell& c = g_linear[col][row];
        if (c.sample < 0) return; // celda vacía: no se pone FX
        c.fx = (unsigned char)fx;
        RequestLinearCell(col, row, c.sample, SemitoneToColorId(c.pitchIdx), fx);
    };

    auto linearClearAll = [&]() {
        for (int col = 0; col < LINEAR_COLS; col++)
            for (int row = 0; row < LINEAR_ROWS; row++) g_linear[col][row] = LinCell();
        RequestClearLinear();
        SetStatus("Linear cleared");
    };

    // Cambia el número de carriles visibles. Al reducir, borra las filas que
    // quedan ocultas (mirror + motor) para que no sigan sonando.
    auto linearSetRows = [&](int n) {
        if (n < 1) n = 1;
        if (n > LINEAR_ROWS) n = LINEAR_ROWS;
        for (int row = n; row < g_linearRows; row++) {
            for (int col = 0; col < LINEAR_COLS; col++) {
                if (g_linear[col][row].sample >= 0) {
                    g_linear[col][row] = LinCell();
                    RequestLinearCell(col, row, -1, 0, 0);
                }
            }
        }
        g_linearRows = n;
    };

    // Importa un .mid: primeras 2 compases (32 filas de 16avos), hasta 4
    // canales MIDI mapeados a los 4 canales del tracker.
    auto importMidi = [&](const char* path) {
        std::vector<MidiNoteEv> notes;
        float midiBpm = 0.0f;
        if (!ParseMidiFile(path, notes, &midiBpm)) {
            SetStatus("Could not parse %s as MIDI", GetFileName(path));
            return;
        }

        // Canales MIDI en orden de aparición -> canales del tracker.
        int chanMap[16];
        for (int i = 0; i < 16; i++) chanMap[i] = -1;
        int used = 0;
        for (const MidiNoteEv& ev : notes) {
            if (chanMap[ev.midiChannel] < 0 && used < TRACKER_CHANNELS) {
                chanMap[ev.midiChannel] = used++;
            }
        }

        int placed = 0;
        for (const MidiNoteEv& ev : notes) {
            int tch = chanMap[ev.midiChannel];
            if (tch < 0) continue;

            // MIDI note -> absolute semitone directly (MIDI 60 / C4 = 0).
            int semi = ev.note - 60;
            if (semi < -48) semi = -48;
            if (semi > 48) semi = 48;

            // Channel sample: A1..A4 if loaded, else the selected one.
            int sample = SAMPLE_BASE + tch;
            if (clipState(sample) != 1) sample = selectedSampleSlot;

            trackerPlace(tch, ev.row, sample, semi, 0);
            placed++;
        }

        if (midiBpm > 20.0f && midiBpm < 300.0f) setTempo(midiBpm);
        SetStatus("MIDI: %d note(s) on %d channel(s)%s", placed, used,
                  midiBpm > 0 ? TextFormat(" | BPM %.0f", midiBpm) : "");
    };

    auto duplicateScene = [&]() {
        if ((int)g_scenes.size() >= MAX_SCENES) {
            SetStatus("Scene limit reached (%d)", MAX_SCENES);
            return;
        }
        Scene copy = g_scenes[g_curScene];
        g_scenes.insert(g_scenes.begin() + g_curScene + 1, copy);
        clearUndoHistory();
        activateScene(g_curScene + 1);
        SetStatus("Scene duplicated");
    };

    auto deleteScene = [&]() {
        if (g_scenes.size() <= 1) {
            g_scenes[0] = Scene();
            clearUndoHistory();
            activateScene(0);
            SetStatus("Scene cleared (last one can't be removed)");
            return;
        }
        g_scenes.erase(g_scenes.begin() + g_curScene);
        int next = g_curScene;
        if (next >= (int)g_scenes.size()) next = (int)g_scenes.size() - 1;
        clearUndoHistory();
        activateScene(next);
        SetStatus("Scene removed");
    };

    auto moveScene = [&](int dir) {
        int other = g_curScene + dir;
        if (other < 0 || other >= (int)g_scenes.size()) return;
        std::swap(g_scenes[g_curScene], g_scenes[other]);
        g_curScene = other;
        g_mirror = g_scenes[g_curScene].cells;
        SetStatus("Scene moved to position %d", other + 1);
    };

    auto askSceneDuration = [&](int idx) {
        char def[32];
        snprintf(def, sizeof(def), "%.1f", g_scenes[idx].duration);
        char answer[64];
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "Scene %d duration in seconds (0 = off)", idx + 1);
        int r = NativeInputDialog(prompt, def, answer, sizeof(answer));
        if (r == 1) {
            float d = (float)atof(answer);
            if (d < 0.0f) d = 0.0f;
            if (d > 600.0f) d = 600.0f;
            g_scenes[idx].duration = d;
            if (idx == g_curScene) sceneTimeLeft = d;
            SetStatus(d > 0 ? "Scene %d: %.1fs in SONG mode" : "Scene %d: off in SONG mode", idx + 1, d);
        } else if (r == -1) {
            float cur = g_scenes[idx].duration;
            int di = 0;
            for (int d = 0; d < kSceneDurationCount; d++) {
                if (cur == kSceneDurations[d]) { di = d; break; }
            }
            g_scenes[idx].duration = kSceneDurations[(di + 1) % kSceneDurationCount];
            if (idx == g_curScene) sceneTimeLeft = g_scenes[idx].duration;
        }
    };

    // Opens the slot editor. Editor kind follows the BAR, not the content: a
    // CLIP slot (< SAMPLE_BASE) gets the video/frame editor; a SMP slot gets
    // the waveform editor. Either way you can replace the visual with an
    // image/GIF from inside the editor.
    auto openEditor = [&](int slot) {
        const InstrumentSource& src = g_engine.getInstrumentBank().at(slot);
        bool wantAudio = (slot >= SAMPLE_BASE);
        if (!wantAudio && src.hasVideo()) {
            editorIsAudio = false;
            // Start the handles at the CURRENT trim (the full clip stays in
            // RAM — trimming is non-destructive, so you can widen/move it).
            edStart = src.videoTrimStart();
            edEnd = src.videoTrimEnd();
        } else if (wantAudio && src.hasAudio()) {
            editorIsAudio = true;
            edStart = (int)src.audioTrimStart();
            edEnd = (int)src.audioTrimEnd();
            // Forma de onda min/max por columna (se dibuja en el editor).
            const int cols = 880;
            edWaveMin.assign(cols, 0.0f);
            edWaveMax.assign(cols, 0.0f);
            unsigned long long totalS = src.audio.totalFrames;
            for (int cIdx = 0; cIdx < cols; cIdx++) {
                unsigned long long a = totalS * cIdx / cols;
                unsigned long long b = totalS * (cIdx + 1) / cols;
                if (b <= a) b = a + 1;
                if (b > totalS) b = totalS;
                float mn = 1.0f, mx = -1.0f;
                for (unsigned long long s = a; s < b; s++) {
                    float v = src.audio.pPCM[s];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                edWaveMin[cIdx] = mn;
                edWaveMax[cIdx] = mx;
            }
        } else {
            SetStatus("Slot %s is empty", slotLabel(slot));
            return;
        }
        editorOpen = true;
        editorSlot = slot;
        edCursor = 0.0f;
        edDrag = 0;
        edPlaying = false;
        RequestPreviewStop();
    };

    // Stop the trim editor's audio preview (plays the slot's audio in the
    // selected range, looped, at original pitch — so you can hear it).
    auto stopEdPreview = [&]() {
        if (edPlaying) { RequestPreviewStop(); edPlaying = false; }
    };

    auto toggleVideoMode = [&]() {
        if (recording) {
            SetStatus("Stop recording before changing video mode");
            return;
        }
        bool portrait = g_exportW > g_exportH;
        g_exportW = portrait ? 720 : 1280;
        g_exportH = portrait ? 1280 : 720;
        UnloadRenderTexture(collageRT);
        collageRT = LoadRenderTexture(g_exportW, g_exportH);
        UnloadRenderTexture(postRT);
        postRT = LoadRenderTexture(g_exportW, g_exportH);
        SetStatus(portrait ? "Video mode: VERTICAL 9:16 (phone reels)" : "Video mode: NORMAL 16:9");
    };

    auto toggleLive = [&]() {
        if (!liveOn) {
            if (g_liveMem == nullptr && !LiveShmOpen(true)) {
                SetStatus("Could not create shared memory for LIVE");
                return;
            }
            char cmd[700];
#if defined(_WIN32)
            snprintf(cmd, sizeof(cmd), "start \"\" \"%s\" --live", argv[0]);
#else
            snprintf(cmd, sizeof(cmd), "\"%s\" --live >/dev/null 2>&1 &", argv[0]);
#endif
            RunCommand(cmd);
            liveOn = true;
            SetStatus("LIVE window opened - drag it to another screen, click it for fullscreen");
        } else {
            liveOn = false;
            SetStatus("LIVE feed stopped (close the LIVE window when done)");
        }
    };

    // ------------------- Grabación -------------------
    auto startRecording = [&](bool withVideo) {
        if (recording) return;

        // Los intermedios tambien viven en temp/: asi la carpeta de trabajo no
        // se llena de restos si ffmpeg se queda a medias.
        snprintf(recAudioPath, sizeof(recAudioPath), "%s", TempPath("pinguus_tmp_audio.wav"));
        if (withVideo) {
            snprintf(recVideoTmp, sizeof(recVideoTmp), "%s", TempPath("pinguus_tmp_video.mp4"));
        }

        ma_device_stop(&device);
        ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, 44100);
        if (ma_encoder_init_file(recAudioPath, &cfg, &g_masterRecorder) != MA_SUCCESS) {
            ma_device_start(&device);
            SetStatus("Could not create %s", recAudioPath);
            return;
        }

        if (withVideo) {
            // popen() SIEMPRE devuelve una tubería válida: lo que arranca es el
            // shell, y que dentro no exista ffmpeg no se sabe hasta que sale con
            // 127. Para entonces ya hemos escrito en una tubería rota y el
            // proceso se muere de un SIGPIPE — el programa se cerraba de golpe
            // al pulsar grabar en un equipo sin ffmpeg. Comprobarlo antes.
            if (g_ffmpegState == 0) {
                ma_encoder_uninit(&g_masterRecorder);
                // El encoder ya creó el .wav vacío: borrarlo, o temp/ se va
                // llenando de restos de 44 bytes cada vez que se intenta grabar
                // sin ffmpeg.
                remove(recAudioPath);
                ma_device_start(&device);
                SetStatus("Cannot record video: ffmpeg is missing (see the warning at the bottom)");
                return;
            }
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -y -f rawvideo -pix_fmt rgba -s %dx%d -r 30 -i - "
                     "-vf vflip -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p "
                     "%s -loglevel quiet",
                     g_exportW, g_exportH, recVideoTmp);
            recPipe = POPEN(cmd, POPEN_WB);
            if (recPipe == nullptr) {
                ma_encoder_uninit(&g_masterRecorder);
                remove(recAudioPath);
                ma_device_start(&device);
                SetStatus("Could not start ffmpeg (is it installed?)");
                return;
            }
        }

        recording = true;
        recVideo = withVideo;
        recSeconds = 0.0f;
        recAccum = 0.0f;
        g_isRecording = true;
        ma_device_start(&device);
        SetStatus(withVideo ? "Recording video + audio... press again to stop"
                            : "Recording audio... press again to stop");
    };

    // Lleva la última toma a donde el usuario diga. COPIA en vez de mover: si el
    // destino está en otro disco un rename fallaría, y además dejar el original
    // en temp/ significa que un fallo a mitad no pierde la grabación.
    auto saveTakeAs = [&]() {
        if (recFinalPath[0] == '\0') { SetStatus("There is no take to save yet"); return; }
        if (!FileExists(recFinalPath)) {
            SetStatus("The take is no longer in temp/ - it may have been deleted");
            recFinalPath[0] = '\0';
            return;
        }
        const char* base = GetFileName(recFinalPath);
        char dest[512] = "";
        int r = NativeSaveDialog("Save this take as...", base, dest, sizeof(dest));
        if (r != 1 || dest[0] == '\0') return;

        // Si el usuario no puso extensión, se le pone la que le toca: sin ella
        // el reproductor no sabe qué es y parece que el archivo salió mal.
        const char* wantExt = strstr(base, ".mp4") ? ".mp4" : ".wav";
        if (!strstr(dest, wantExt)) {
            const size_t n = strlen(dest);
            if (n + 5 < sizeof(dest)) snprintf(dest + n, sizeof(dest) - n, "%s", wantExt);
        }

        if (CopyFileTo(recFinalPath, dest)) SetStatus("Saved: %s", dest);
        else SetStatus("Could not write to %s (no permission, or the disk is full)", dest);
    };

    auto stopRecording = [&]() {
        if (!recording) return;

        ma_device_stop(&device);
        g_isRecording = false;
        ma_encoder_uninit(&g_masterRecorder);
        ma_device_start(&device);
        recording = false;

        char ts[32];
        time_t t = time(nullptr);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&t));

        if (recVideo) {
            if (recPipe != nullptr) {
                PCLOSE(recPipe);
                recPipe = nullptr;
            }
            BeginDrawing();
            ClearBackground(g_theme.bg);
            const char* muxMsg = "Exporting video...";
            DrawText(muxMsg, (screenWidth - MeasureText(muxMsg, 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
            EndDrawing();

            // Nada de dialogo "Guardar como" al parar: la grabacion aterriza
            // SIEMPRE en temp/ y ahi se queda. Al cerrar el programa se
            // pregunta si conservarla, que es cuando se sabe si valia o no.
            char outPath[256];
            snprintf(outPath, sizeof(outPath), "%s", TempPath(TextFormat("pinguus_video_%s.mp4", ts)));

            char cmd[768];
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -y -i %s -i %s -c:v copy -c:a aac -b:a 192k -shortest %s -loglevel quiet",
                     recVideoTmp, recAudioPath, outPath);
            int ret = RunCommand(cmd);
            remove(recVideoTmp);
            remove(recAudioPath);
            if (ret != 0) {
                SetStatus("Video export failed (see console)");
                return;
            }
            snprintf(recFinalPath, sizeof(recFinalPath), "%s", outPath);
            RegisterSessionClip(outPath);
            SetStatus("Video recorded: %s (%.1fs) - press SAVE AS to keep it elsewhere",
                      outPath, recSeconds);
            if (recAskWhere) saveTakeAs();
        } else {
            char outPath[256];
            snprintf(outPath, sizeof(outPath), "%s", TempPath(TextFormat("pinguus_audio_%s.wav", ts)));
            if (MoveFileTo(recAudioPath, outPath)) {
                snprintf(recFinalPath, sizeof(recFinalPath), "%s", outPath);
                RegisterSessionClip(outPath);
                SetStatus("Audio recorded: %s (%.1fs) - press SAVE AS to keep it elsewhere",
                          outPath, recSeconds);
                if (recAskWhere) saveTakeAs();
            } else {
                SetStatus("Could not move the recording into %s/", kTempDir);
            }
        }
    };

    // ------------------- Guardado / carga -------------------
    auto saveProject = [&](const char* path) {
        FILE* f = fopen(path, "w");
        if (f == nullptr) {
            SetStatus("Could not write %s", path);
            return;
        }
        // v9: las líneas `cell` llevan tres campos más (espera, volumen y
        // velocidad de la nota). El cargador sigue aceptando las de 8 y 9.
        fprintf(f, "SIMTUNES_PROJECT 9\n");
        fprintf(f, "octave %d\n", g_octave);
        fprintf(f, "master %.3f\n", g_masterVol.load());
        fprintf(f, "bpm %.2f\n", bpmDisplay);
        fprintf(f, "grid %d %d\n", g_gridW, g_gridH);
        fprintf(f, "videomode %d\n", g_exportH > g_exportW ? 1 : 0);
        // Efecto de vídeo analógico: es una propiedad de CÓMO SALE el proyecto,
        // así que viaja con él. Una sola línea con los catorce mandos.
        fprintf(f, "ntsc %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.1f\n",
                g_ntsc.enabled ? 1 : 0, g_ntsc.noise, g_ntsc.lumaNoise, g_ntsc.chromaNoise,
                g_ntsc.snow, g_ntsc.chromaDelay, g_ntsc.phaseNoise, g_ntsc.ringing,
                g_ntsc.sharpen, g_ntsc.edgeWave, g_ntsc.edgeWaveSpeed, g_ntsc.headSwitch,
                g_ntsc.chromaLoss, g_ntsc.scanlines, g_ntsc.tapeBlur, g_ntsc.seed);
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (!g_slotPath[i].empty()) {
                fprintf(f, "clip %d trim %d %d path %s\n", i, g_slotTrimStart[i], g_slotTrimLen[i], g_slotPath[i].c_str());
            }
            if (!g_slotVisualPath[i].empty()) {
                fprintf(f, "visual %d %s\n", i, g_slotVisualPath[i].c_str());
            }
            if (!g_slotPdPath[i].empty()) {
                fprintf(f, "pd %d %s\n", i, g_slotPdPath[i].c_str());
            }
            if (g_slotShaderOn[i] && !g_slotShaderPath[i].empty()) {
                fprintf(f, "shader %d %s\n", i, g_slotShaderPath[i].c_str());
            }
            const ClipFX& fx = g_clipFX[i];
            if (fx.flipX || fx.zoomPulse || fx.rotate || fx.center || fx.move ||
                fx.scale != 1.0f || fx.layer != 4) {
                fprintf(f, "fx %d %d %d %d %d %.2f layer %d move %d %.3f %.3f %.3f %.3f\n",
                        i, fx.flipX ? 1 : 0, fx.zoomPulse ? 1 : 0, fx.rotate ? 1 : 0, fx.center ? 1 : 0,
                        fx.scale, fx.layer, fx.move ? 1 : 0, fx.ax, fx.ay, fx.bx, fx.by);
            }
            // Colocación libre, giro fijo, transparencia y modo de fusión. Van
            // en una línea APARTE y no ampliando la de `fx` para que un
            // proyecto nuevo siga abriéndose en una versión vieja (que
            // simplemente no reconoce esta clave y la salta) y al revés.
            {
                const ClipFX& fx = g_clipFX[i];
                if (fx.place || fx.rotDeg != 0.0f || fx.opacity != 1.0f || fx.blend != BLEND_FX_NORMAL) {
                    fprintf(f, "fx2 %d %d %.4f %.4f %.2f %.3f %d\n", i, fx.place ? 1 : 0,
                            fx.posX, fx.posY, fx.rotDeg, fx.opacity, fx.blend);
                }
            }
        }
        for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
            for (int r = 0; r < TRACKER_ROWS; r++) {
                const TrkCell& c = g_tracker[ch][r];
                if (c.sample >= 0) {
                    fprintf(f, "trk %d %d %d %d %d\n", ch, r, c.sample, c.pitchIdx, c.fx);
                }
            }
        }
        // Modo lineal: parámetros (largo, loop, carriles) y una línea por nota.
        // Modelos 3D cargados (se recargan antes que las escenas, para que las
        // celdas que los referencian los encuentren).
        for (int i = 0; i < MAX_MODELS; i++) {
            if (g_models[i].loaded && !g_models[i].path.empty()) {
                const ModelSlot& ms = g_models[i];
                fprintf(f, "model %d %.3f %.3f %.3f %.1f %.1f %.1f %.3f %d %s\n", i,
                        ms.pos.x, ms.pos.y, ms.pos.z, ms.rot.x, ms.rot.y, ms.rot.z, ms.userScale, ms.toon ? 1 : 0, ms.path.c_str());
                // Animaciones humanoides importadas (.vrma o .bvh). Se guardan
                // como `anim <slot> <flip> <ruta>`: el formato se deduce de la
                // extensión al recargar, y flip conserva la corrección de giro
                // que el usuario haya puesto a mano en un mocap.
                for (const VrmaClip& vc : ms.clips)
                    if (!vc.path.empty()) fprintf(f, "anim %d %d %s\n", i, vc.flipY ? 1 : 0, vc.path.c_str());
                // Trim/loop por animación (solo los que no están por defecto).
                int total = modelAnimTotal(ms);
                for (int a = 0; a < total && a < MAX_MODEL_ANIMS; a++)
                    if (ms.animTrimStart[a] != 0 || ms.animTrimEnd[a] != 0 || ms.animLoop[a])
                        fprintf(f, "modeltrim %d %d %d %d %d\n", i, a, ms.animTrimStart[a], ms.animTrimEnd[a], ms.animLoop[a] ? 1 : 0);
            }
        }
        fprintf(f, "linparams %d %d %d\n", g_linearLength, g_linearLoop ? 1 : 0, g_linearRows);
        for (int col = 0; col < LINEAR_COLS; col++) {
            for (int row = 0; row < LINEAR_ROWS; row++) {
                const LinCell& c = g_linear[col][row];
                if (c.sample >= 0) {
                    fprintf(f, "lin %d %d %d %d %d\n", col, row, c.sample, c.pitchIdx, c.fx);
                }
            }
        }
        // --- Banco de melodías ---
        // Una línea por melodía ocupada y otra por nota. Van con el proyecto
        // porque son material creativo: la melodía que tarareaste es tan tuya
        // como lo que pintaste, y perderla al cerrar sería absurdo.
        for (int i = 0; i < MELODY_BANK_SIZE; i++) {
            const MelodyClip& mc = g_melodies[i];
            if (mc.empty()) continue;
            fprintf(f, "melody %d %s\n", i, mc.name.empty() ? "-" : mc.name.c_str());
            for (const auto& n : mc.notes)
                fprintf(f, "melnote %d %d %d %d %.3f\n", i, n.step, n.semitone, n.lenSteps, n.peak);
        }

        // --- Modo BEATBOX ---
        // Solo se escriben los pads OCUPADOS y los pasos que tienen algo: un
        // proyecto sin sampleadora no engorda ni una línea, y un .smt viejo
        // sigue cargando igual (estas claves simplemente no aparecen).
        fprintf(f, "padtransport %d %d %d %.3f %.3f %d\n",
                padSteps, padQuant, mfxType, mfxX, mfxY, mfxLatch ? 1 : 0);
        for (int i = 0; i < PAD_TOTAL; i++) {
            const PadConfig& p = g_pads[i];
            if (p.empty()) continue;
            fprintf(f, "pad %d %d %d %.3f %d %d %d\n",
                    i, p.slot, (int)p.pitch, p.vol, (int)p.mode, (int)p.choke, (int)p.fx);
        }
        {
            // El patrón vive en el motor: se lee de la última foto publicada.
            const SnapshotBuffer& psnap = g_snapshotPublisher.read();
            for (int t = 0; t < SNAP_PAD_TICKS; t++) {
                if (psnap.padPattern[t] == 0ULL) continue;
                fprintf(f, "padstep %d %llu\n", t, (unsigned long long)psnap.padPattern[t]);
            }
        }

        for (int s = 0; s < (int)g_scenes.size(); s++) {
            const Scene& sc = g_scenes[s];
            fprintf(f, "scene %d duration %.2f\n", s, sc.duration);
            for (int y = 0; y < g_gridH; y++) {
                for (int x = 0; x < g_gridW; x++) {
                    const MirrorCell& mc = sc.cells[y * g_gridW + x];
                    // cellIsBlank, no `kind == CELL_EMPTY`: un silencio es una
                    // celda sin tipo pero con espera, y hay que guardarla.
                    if (cellIsBlank(mc)) continue;
                    // Celda de mod: se guarda por NOMBRE (los ids no son estables
                    // entre sesiones) para re-emparejarla al cargar.
                    if (mc.kind == CELL_MODCELL) {
                        if (mc.modCellId >= 0 && mc.modCellId < (int)g_modCells.size())
                            fprintf(f, "modcell %d %d %d %s\n", s, x, y, g_modCells[mc.modCellId].name.c_str());
                        continue;
                    }
                    // Los tres últimos campos (espera, volumen, velocidad) son
                    // de la v9; los lectores viejos los ignoran y el cargador
                    // de aquí acepta líneas de 8, 9 o 12 campos.
                    fprintf(f, "cell %d %d %d %d %d %d %d %.2f %d %.2f %.3f %.3f\n",
                            s, x, y, (int)mc.kind, mc.pitchIdx, mc.clip, mc.dir, mc.sust, mc.fxType,
                            mc.hold, mc.vol, mc.tmul);
                }
            }
            for (const BugSpawn& b : sc.bugs) {
                fprintf(f, "bug %d %d %d %d %d %d %.3f %d %.3f %d\n", s, b.x, b.y, b.dx, b.dy, b.clip, b.tempoMul, b.muted ? 1 : 0, b.volume, b.stopped ? 1 : 0);
            }
        }
        fclose(f);
        SetStatus("Project saved to %s", path);
    };

    auto loadProject = [&](const char* path) {
        FILE* f = fopen(path, "r");
        if (f == nullptr) {
            SetStatus("Could not open %s", path);
            return;
        }
        char line[1024];
        int fileVer = 0;
        if (fgets(line, sizeof(line), f) == nullptr ||
            sscanf(line, "SIMTUNES_PROJECT %d", &fileVer) != 1) {
            SetStatus("%s is not a Pinguus project", path);
            fclose(f);
            return;
        }

        // Proyectos v4 y anteriores: los samples vivían en los slots 16..31;
        // ahora esos índices son clips. Se remapean a 64+ al cargar.
        auto remapSlot = [&](int s) -> int {
            if (fileVer <= 4 && s >= 16 && s < 32) return s + 48;
            return s;
        };
        // Pre-v6 stored a palette index 0..7; map it to an absolute semitone.
        auto remapPitch = [&](int p) -> int {
            if (fileVer < 6 && p >= 0 && p < 8) return kOldPaletteSemis[p];
            return p;
        };

        BeginDrawing();
        ClearBackground(g_theme.bg);
        const char* loadingMsg = "Loading project... re-decoding clips to RAM";
        DrawText(loadingMsg, (screenWidth - MeasureText(loadingMsg, 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
        EndDrawing();

        ma_device_stop(&device);

        g_scenes.clear();
        g_scenes.emplace_back();
        g_curScene = 0;
        g_mirror = g_scenes[0].cells;
        for (int i = 0; i < MAX_SLOTS; i++) {
            g_engine.getInstrumentBank().Clear(i);
            g_engine.getPd().clearPatch(i);
            if (g_slotShaderOn[i]) { UnloadShader(g_slotShader[i]); g_slotShaderOn[i] = false; }
            g_slotPath[i].clear();
            g_slotVisualPath[i].clear();
            g_slotPdPath[i].clear();
            g_slotShaderPath[i].clear();
            g_slotTrimStart[i] = 0;
            g_slotTrimLen[i] = 0;
            g_clipFX[i] = ClipFX();
        }
        // El banco de melodías se vacía igual que todo lo demás: un proyecto
        // nuevo no debe traer las melodías del anterior.
        for (int i = 0; i < MELODY_BANK_SIZE; i++) g_melodies[i].clear();
        melIdx = 0;
        melAnalysed = false;
        melNote.clear();

        // El efecto de vídeo analógico también se vacía: un proyecto que no lo
        // lleve no debe heredar el VHS del anterior sin avisar.
        {
            bool keepShader = g_ntscShader.ok;
            g_ntsc = NtscFX();
            (void)keepShader;   // el shader compilado se reaprovecha tal cual
        }
        for (int i = 0; i < MAX_MODELS; i++) UnloadModelSlot(i);
        selectedModel = -1; selectedModelAnim = -1;
        for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
            for (int r = 0; r < TRACKER_ROWS; r++) g_tracker[ch][r] = TrkCell();
        }
        RequestClearTracker();
        for (int col = 0; col < LINEAR_COLS; col++)
            for (int row = 0; row < LINEAR_ROWS; row++) g_linear[col][row] = LinCell();
        g_linearLength = 32;
        g_linearLoop = true;
        g_linearRows = 4;
        RequestClearLinear();
        // Los pads y su patrón se vacían igual que el tracker y el lineal: un
        // proyecto cargado no debe heredar el kit del anterior.
        RequestPadStopAll();
        for (int i = 0; i < PAD_TOTAL; i++) {
            g_pads[i] = PadConfig();
            RequestSetPadConfig(i);
            padHeld[i] = 0;
        }
        RequestClearPadPattern(-1);
        padPatPlaying = false;
        padRecArm = false;
        padBank = 0;
        padSel = 0;
        padSteps = 16;
        padQuant = 1;
        mfxOn = false;
        RequestPadTransport(false, false, padQuant, padSteps);
        clearUndoHistory();
        songMode = false;
        int missingClips = 0;
        float loadedBpm = 120.0f;

        while (fgets(line, sizeof(line), f) != nullptr) {
            int slot, trimStart, trimLen, si, x, y, kind, pitch, clip, dir, dx, dy, gw, gh, vm, cellFx, tch, trow, tfx;
            float dur, sust, tempo;
            char filePath[512];

            if (sscanf(line, "bpm %f", &loadedBpm) == 1) continue;
            if (sscanf(line, "grid %d %d", &gw, &gh) == 2) {
                if (gw >= 8 && gw <= MAX_GW && gh >= 6 && gh <= MAX_GH) {
                    g_gridW = gw;
                    g_gridH = gh;
                }
                continue;
            }
            if (sscanf(line, "videomode %d", &vm) == 1) {
                int wantW = vm != 0 ? 720 : 1280;
                if (wantW != g_exportW) {
                    g_exportW = wantW;
                    g_exportH = vm != 0 ? 1280 : 720;
                    UnloadRenderTexture(collageRT);
                    collageRT = LoadRenderTexture(g_exportW, g_exportH);
                    UnloadRenderTexture(postRT);
                    postRT = LoadRenderTexture(g_exportW, g_exportH);
                }
                continue;
            }
            {
                int oct;
                if (sscanf(line, "octave %d", &oct) == 1) {
                    g_octave = oct < kMinOctave ? kMinOctave : (oct > kMaxOctave ? kMaxOctave : oct);
                    continue;
                }
                float mvol;
                if (sscanf(line, "master %f", &mvol) == 1) {
                    if (mvol < 0.0f) mvol = 0.0f;
                    if (mvol > 1.5f) mvol = 1.5f;
                    g_masterVol.store(mvol);
                    continue;
                }
            }
            if (sscanf(line, "clip %d trim %d %d path %511[^\n]", &slot, &trimStart, &trimLen, filePath) == 4) {
                slot = remapSlot(slot);
                if (slot < 0 || slot >= MAX_SLOTS) continue;
                bool isAudio = IsAudioFile(filePath);
                bool ok = isAudio
                              ? g_engine.getInstrumentBank().LoadAudioOnly(slot, filePath)
                              : IsImageFile(filePath)
                                  ? LoadImageIntoSlot(slot, filePath, g_transcodeMaxSide)
                                  : g_engine.getInstrumentBank().LoadVideo(slot, filePath, true);
                if (ok) {
                    g_slotPath[slot] = filePath;
                    if (trimLen > 0) { // non-destructive: just set the range
                        if (isAudio) g_engine.getInstrumentBank().SetAudioTrim(slot, (unsigned long long)trimStart, (unsigned long long)trimStart + trimLen);
                        else g_engine.getInstrumentBank().SetVideoTrim(slot, trimStart, trimStart + trimLen);
                        g_slotTrimStart[slot] = trimStart;
                        g_slotTrimLen[slot] = trimLen;
                    }
                    auto it = textureCache.find(slot);
                    if (it != textureCache.end()) {
                        UnloadTexture(it->second);
                        textureCache.erase(it);
                    }
                } else {
                    missingClips++;
                    printf("loadProject: no se pudo cargar '%s' (slot %d)\n", filePath, slot);
                }
                continue;
            }
            if (sscanf(line, "visual %d %511[^\n]", &slot, filePath) == 2) {
                slot = remapSlot(slot);
                if (slot >= 0 && slot < MAX_SLOTS && ReplaceVisualIntoSlot(slot, filePath, g_transcodeMaxSide)) {
                    g_slotVisualPath[slot] = filePath;
                    auto it = textureCache.find(slot);
                    if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
                }
                continue;
            }
            if (sscanf(line, "pd %d %511[^\n]", &slot, filePath) == 2) {
                slot = remapSlot(slot);
                if (slot >= 0 && slot < MAX_SLOTS && g_engine.getPd().loadPatch(slot, filePath)) {
                    g_slotPdPath[slot] = filePath;
                }
                continue;
            }
            if (sscanf(line, "shader %d %511[^\n]", &slot, filePath) == 2) {
                slot = remapSlot(slot);
                if (slot >= 0 && slot < MAX_SLOTS && !loadShaderFromFile(slot, filePath)) {
                    printf("loadProject: shader '%s' failed to compile (slot %d)\n", filePath, slot);
                }
                continue;
            }
            {
                // Nuevo: `model i px py pz rx ry rz scale toon path`. Compat:
                // sin toon (9 campos) y el viejo `model i path` (más abajo).
                int msl, mtoon = 0; float px, py, pz, rx, ry, rz, msc; char mpath[512];
                int n = sscanf(line, "model %d %f %f %f %f %f %f %f %d %511[^\n]", &msl, &px, &py, &pz, &rx, &ry, &rz, &msc, &mtoon, mpath);
                if (n != 10) { mtoon = 0; n = sscanf(line, "model %d %f %f %f %f %f %f %f %511[^\n]", &msl, &px, &py, &pz, &rx, &ry, &rz, &msc, mpath); }
                if (n >= 9 && msl >= 0 && msl < MAX_MODELS) {
                    if (LoadModelSlot(msl, mpath)) {
                        ModelSlot& ms = g_models[msl];
                        ms.pos = {px, py, pz}; ms.rot = {rx, ry, rz}; ms.userScale = msc;
                        ms.toon = (mtoon != 0); ApplyModelShader(ms);
                        printf("MODEL loaded: %s (%d anims, humanoid=%d)\n", mpath, ms.animCount, (int)ms.humanoid);
                    } else printf("loadProject: model '%s' failed (slot %d)\n", mpath, msl);
                    continue;
                }
            }
            if (sscanf(line, "model %d %511[^\n]", &slot, filePath) == 2) {
                if (slot >= 0 && slot < MAX_MODELS) {
                    if (LoadModelSlot(slot, filePath)) printf("MODEL loaded: %s (%d anims)\n", filePath, g_models[slot].animCount);
                    else printf("loadProject: model '%s' failed (slot %d)\n", filePath, slot);
                }
                continue;
            }
            {   // Animación humanoide importada. Formato actual: `anim <slot>
                // <flip> <ruta>`; se sigue aceptando el viejo `vrma <slot>
                // <ruta>` (proyectos anteriores a la importación de .bvh).
                int aslot = -1, aflip = 0;
                bool got = (sscanf(line, "anim %d %d %511[^\n]", &aslot, &aflip, filePath) == 3);
                if (!got) { aflip = 0; got = (sscanf(line, "vrma %d %511[^\n]", &aslot, filePath) == 2); }
                if (got) {
                    if (aslot >= 0 && aslot < MAX_MODELS && g_models[aslot].loaded && g_models[aslot].humanoid) {
                        VrmaClip vc;
                        if (loadHumanoidAnimFile(filePath, vc)) {
                            vc.flipY = (aflip != 0);
                            g_models[aslot].clips.push_back(vc);
                            g_models[aslot].animNames.push_back(vc.name);
                        } else printf("loadProject: animation '%s' failed (model %d)\n", filePath, aslot);
                    }
                    continue;
                }
            }
            {   // Trim/loop por animación de modelo (después de model+vrma).
                int mslot, manim, mst, men, mloop;
                if (sscanf(line, "modeltrim %d %d %d %d %d", &mslot, &manim, &mst, &men, &mloop) == 5) {
                    if (mslot >= 0 && mslot < MAX_MODELS && g_models[mslot].loaded &&
                        manim >= 0 && manim < MAX_MODEL_ANIMS) {
                        g_models[mslot].animTrimStart[manim] = mst;
                        g_models[mslot].animTrimEnd[manim]   = men;
                        g_models[mslot].animLoop[manim]      = (mloop != 0);
                    }
                    continue;
                }
            }
            {
                int flip, zoom, rot, cen, layer, move;
                float fscale, ax, ay, bx, by;
                int n = sscanf(line, "fx %d %d %d %d %d %f layer %d move %d %f %f %f %f",
                               &slot, &flip, &zoom, &rot, &cen, &fscale, &layer, &move, &ax, &ay, &bx, &by);
                if (n >= 6) {
                    slot = remapSlot(slot);
                    if (slot >= 0 && slot < MAX_SLOTS) {
                        ClipFX& fx = g_clipFX[slot];
                        fx.flipX = flip != 0;
                        fx.zoomPulse = zoom != 0;
                        fx.rotate = rot != 0;
                        fx.center = cen != 0;
                        fx.scale = fscale < 0.25f ? 0.25f : (fscale > 4.0f ? 4.0f : fscale);
                        if (n == 12) {
                            fx.layer = layer < 1 ? 1 : (layer > 8 ? 8 : layer);
                            fx.move = move != 0;
                            auto c01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                            fx.ax = c01(ax); fx.ay = c01(ay); fx.bx = c01(bx); fx.by = c01(by);
                        }
                    }
                    continue;
                }
            }
            {   // ntsc: el efecto de vídeo analógico de la mezcla final.
                int non;
                float nn[15];
                if (sscanf(line, "ntsc %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                           &non, &nn[0], &nn[1], &nn[2], &nn[3], &nn[4], &nn[5], &nn[6],
                           &nn[7], &nn[8], &nn[9], &nn[10], &nn[11], &nn[12], &nn[13]) == 15) {
                    auto c01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                    g_ntsc.noise = c01(nn[0]);      g_ntsc.lumaNoise = c01(nn[1]);
                    g_ntsc.chromaNoise = c01(nn[2]); g_ntsc.snow = c01(nn[3]);
                    g_ntsc.chromaDelay = c01(nn[4]); g_ntsc.phaseNoise = c01(nn[5]);
                    g_ntsc.ringing = c01(nn[6]);     g_ntsc.sharpen = c01(nn[7]);
                    g_ntsc.edgeWave = c01(nn[8]);    g_ntsc.edgeWaveSpeed = c01(nn[9]);
                    g_ntsc.headSwitch = c01(nn[10]); g_ntsc.chromaLoss = c01(nn[11]);
                    g_ntsc.scanlines = c01(nn[12]);  g_ntsc.tapeBlur = c01(nn[13]);
                    g_ntsc.enabled = non != 0;
                    // El shader se compila sólo si hace falta: un proyecto sin
                    // el efecto no paga por él al abrirse.
                    if (g_ntsc.enabled && !g_ntscShader.ok && !g_ntscShader.load()) g_ntsc.enabled = false;
                    continue;
                }
            }
            {   // fx2: colocación libre, giro fijo, transparencia y fusión.
                int fplace, fblend;
                float fposx, fposy, frot, fop;
                if (sscanf(line, "fx2 %d %d %f %f %f %f %d",
                           &slot, &fplace, &fposx, &fposy, &frot, &fop, &fblend) == 7) {
                    slot = remapSlot(slot);
                    if (slot >= 0 && slot < MAX_SLOTS) {
                        auto c01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                        ClipFX& fx = g_clipFX[slot];
                        fx.place = fplace != 0;
                        fx.posX = c01(fposx);
                        fx.posY = c01(fposy);
                        fx.rotDeg = fmodf(frot, 360.0f);
                        fx.opacity = c01(fop);
                        fx.blend = (fblend < 0 || fblend >= BLEND_FX_COUNT) ? BLEND_FX_NORMAL : fblend;
                        // PLACE manda sobre CENTER, igual que en la interfaz.
                        if (fx.place) { fx.center = false; fx.move = false; }
                    }
                    continue;
                }
            }
            if (sscanf(line, "trk %d %d %d %d %d", &tch, &trow, &slot, &pitch, &tfx) == 5) {
                slot = remapSlot(slot);
                pitch = remapPitch(pitch);
                if (tch < 0 || tch >= TRACKER_CHANNELS || trow < 0 || trow >= TRACKER_ROWS) continue;
                if (slot < 0 || slot >= MAX_SLOTS || pitch < -96 || pitch > 96 || tfx < 0 || tfx > 3) continue;
                g_tracker[tch][trow] = {slot, pitch, tfx};
                RequestTrackerCell(tch, trow, slot, SemitoneToColorId(pitch), tfx);
                continue;
            }
            {
                int llen, lloop, lrows = 4;
                int n = sscanf(line, "linparams %d %d %d", &llen, &lloop, &lrows);
                if (n >= 2) {
                    g_linearLength = llen < 1 ? 1 : (llen > LINEAR_COLS ? LINEAR_COLS : llen);
                    g_linearLoop = lloop != 0;
                    g_linearRows = lrows < 1 ? 1 : (lrows > LINEAR_ROWS ? LINEAR_ROWS : lrows);
                    RequestLinearParams(g_linearLength, g_linearLoop);
                    continue;
                }
            }
            {
                // v7 nuevo: `lin col row sample pitch fx`. Se acepta el formato
                // previo de 4 campos (sin pitch) derivando el tono de la fila.
                int lcol, lrow, lslot, lpitch, lfx;
                int n = sscanf(line, "lin %d %d %d %d %d", &lcol, &lrow, &lslot, &lpitch, &lfx);
                if (n == 4) { lfx = lpitch; lpitch = linearRowToSemitone(lrow); }
                if (n >= 4) {
                    lslot = remapSlot(lslot);
                    if (lcol < 0 || lcol >= LINEAR_COLS || lrow < 0 || lrow >= LINEAR_ROWS) continue;
                    if (lslot < 0 || lslot >= MAX_SLOTS || lpitch < -96 || lpitch > 96 || lfx < 0 || lfx > 4) continue;
                    g_linear[lcol][lrow] = {lslot, lpitch, (unsigned char)lfx};
                    RequestLinearCell(lcol, lrow, lslot, SemitoneToColorId(lpitch), lfx);
                    if (lrow >= g_linearRows) g_linearRows = lrow + 1; // no ocultar notas
                    continue;
                }
            }
            // --- Banco de melodías ---
            {
                int mi;
                char mname[128];
                if (sscanf(line, "melody %d %127[^\n]", &mi, mname) == 2) {
                    if (mi >= 0 && mi < MELODY_BANK_SIZE) {
                        g_melodies[mi].notes.clear();
                        g_melodies[mi].name = (strcmp(mname, "-") == 0) ? "" : mname;
                    }
                    continue;
                }
            }
            {
                int mi, mstep, msemi, mlen;
                float mpeak;
                if (sscanf(line, "melnote %d %d %d %d %f", &mi, &mstep, &msemi, &mlen, &mpeak) == 5) {
                    if (mi >= 0 && mi < MELODY_BANK_SIZE && mstep >= 0 && mstep < MAX_GW &&
                        msemi >= -96 && msemi <= 96) {
                        PitchToNotes::StepNote sn;
                        sn.step = mstep;
                        sn.semitone = msemi;
                        sn.lenSteps = mlen < 1 ? 1 : mlen;
                        sn.peak = mpeak < 0.0f ? 0.0f : (mpeak > 1.0f ? 1.0f : mpeak);
                        g_melodies[mi].notes.push_back(sn);
                    }
                    continue;
                }
            }

            // --- Modo BEATBOX ---
            {
                int pSteps, pQuant, pFx, pLatch;
                float px, py;
                if (sscanf(line, "padtransport %d %d %d %f %f %d",
                           &pSteps, &pQuant, &pFx, &px, &py, &pLatch) == 6) {
                    padSteps = pSteps < 4 ? 4 : (pSteps > PAD_PAT_MAX_STEPS ? PAD_PAT_MAX_STEPS : pSteps);
                    padQuant = pQuant < 0 ? 0 : (pQuant > 4 ? 0 : pQuant);
                    mfxType = (pFx < 0 || pFx >= MFX_COUNT) ? MFX_OFF : pFx;
                    mfxX = px < 0 ? 0 : (px > 1 ? 1 : px);
                    mfxY = py < 0 ? 0 : (py > 1 ? 1 : py);
                    mfxLatch = pLatch != 0;
                    mfxOn = false;   // un proyecto no se abre con el efecto metido
                    RequestPadTransport(padPatPlaying, padRecArm, padQuant, padSteps);
                    RequestSetMasterFx(mfxType, mfxX, mfxY, false);
                    continue;
                }
            }
            {
                int pi, pslot, ppitch, pmode, pchoke, pfx;
                float pvol;
                if (sscanf(line, "pad %d %d %d %f %d %d %d",
                           &pi, &pslot, &ppitch, &pvol, &pmode, &pchoke, &pfx) == 7) {
                    if (pi < 0 || pi >= PAD_TOTAL) continue;
                    pslot = remapSlot(pslot);
                    if (pslot < 0 || pslot >= MAX_SLOTS) continue;
                    PadConfig& p = g_pads[pi];
                    p.slot = pslot;
                    p.pitch = (signed char)(ppitch < -24 ? -24 : (ppitch > 24 ? 24 : ppitch));
                    p.vol = pvol < 0 ? 0 : (pvol > 1 ? 1 : pvol);
                    p.mode = (unsigned char)(pmode < 0 || pmode > 2 ? 0 : pmode);
                    p.choke = (unsigned char)(pchoke < 0 || pchoke > 4 ? 0 : pchoke);
                    p.fx = (unsigned char)(pfx < 0 || pfx > 4 ? 0 : pfx);
                    RequestSetPadConfig(pi);
                    continue;
                }
            }
            {
                int ptick;
                unsigned long long pmask;
                if (sscanf(line, "padstep %d %llu", &ptick, &pmask) == 2) {
                    if (ptick < 0 || ptick >= SNAP_PAD_TICKS) continue;
                    for (int b = 0; b < PAD_TOTAL; b++)
                        if (pmask & (1ULL << b)) RequestSetPadStep(ptick, b, true);
                    continue;
                }
            }

            if (sscanf(line, "scene %d duration %f", &si, &dur) == 2) {
                while ((int)g_scenes.size() <= si && (int)g_scenes.size() < MAX_SCENES) g_scenes.emplace_back();
                if (si >= 0 && si < (int)g_scenes.size()) g_scenes[si].duration = dur;
                continue;
            }
            {
                float cHold = 0.0f, cVol = 1.0f, cTime = 1.0f;
                int n = sscanf(line, "cell %d %d %d %d %d %d %d %f %d %f %f %f",
                               &si, &x, &y, &kind, &pitch, &clip, &dir, &sust, &cellFx,
                               &cHold, &cVol, &cTime);
                if (n >= 8) {
                    clip = remapSlot(clip);
                    pitch = remapPitch(pitch);
                    if (si < 0 || si >= (int)g_scenes.size()) continue;
                    if (x < 0 || x >= g_gridW || y < 0 || y >= g_gridH) continue;
                    if (pitch < -96 || pitch > 96 || clip < 0 || (clip >= MAX_SLOTS && !isModelSlot(clip)) || dir < 0 || dir > 7) continue;
                    MirrorCell& mc = g_scenes[si].cells[y * g_gridW + x];
                    mc.kind = (unsigned char)kind;
                    mc.pitchIdx = pitch;
                    mc.clip = clip;
                    mc.dir = dir;
                    mc.sust = sust;
                    // fxType is 0..3 for FX cells, 0/1 (A/B role) for teleporters.
                    mc.fxType = (n >= 9 && cellFx >= 0 && cellFx <= 3) ? cellFx : 0;
                    mc.color = g_palette[((pitch % 12) + 12) % 12].displayColor;
                    // v9: atributos superpuestos. Los proyectos anteriores no
                    // los traen, y entonces una celda SUSTAIN antigua se
                    // convierte en nota + espera, que es lo mismo que sonaba.
                    if (n >= 12) {
                        mc.hold = (cHold > 0.0f && cHold <= 30.0f) ? cHold : 0.0f;
                        mc.vol  = (cVol  > 0.0f && cVol  <= 1.5f)  ? cVol  : 1.0f;
                        mc.tmul = (cTime > 0.0f && cTime <= 8.0f)  ? cTime : 1.0f;
                    } else if (mc.kind == CELL_SUSTAIN) {
                        mc.hold = (sust > 0.0f && sust <= 30.0f) ? sust : 1.0f;
                        mc.kind = CELL_COLOR;
                    }
                    continue;
                }
            }
            {
                // Celda de mod: `modcell <s> <x> <y> <name>`. Se empareja el
                // nombre con un mod ya cargado; si no existe, se ignora.
                char mcName[128];
                int msi, mx, my;
                if (sscanf(line, "modcell %d %d %d %127s", &msi, &mx, &my, mcName) == 4) {
                    if (msi < 0 || msi >= (int)g_scenes.size()) continue;
                    if (mx < 0 || mx >= g_gridW || my < 0 || my >= g_gridH) continue;
                    int idx = -1;
                    for (int i = 0; i < (int)g_modCells.size(); i++) if (g_modCells[i].name == mcName) { idx = i; break; }
                    if (idx < 0) continue; // el mod no está cargado: se omite
                    MirrorCell& mc = g_scenes[msi].cells[my * g_gridW + mx];
                    mc = MirrorCell();
                    mc.kind = CELL_MODCELL;
                    mc.modCellId = idx;
                    mc.color = {g_modCells[idx].r, g_modCells[idx].g, g_modCells[idx].b, 255};
                    continue;
                }
            }
            {
                int bmuted = 0, bstopped = 0;
                float bvol = 1.0f;
                int n = sscanf(line, "bug %d %d %d %d %d %d %f %d %f %d", &si, &x, &y, &dx, &dy, &clip, &tempo, &bmuted, &bvol, &bstopped);
                if (n >= 7) { // 7 = oldest, 8 = +muted, 9 = +vol, 10 = +stopped
                    clip = remapSlot(clip);
                    if (si < 0 || si >= (int)g_scenes.size()) continue;
                    if (bvol < 0.0f) bvol = 0.0f;
                    if (bvol > 1.5f) bvol = 1.5f;
                    g_scenes[si].bugs.push_back({x, y, dx, dy, clip, tempo, bmuted != 0, bvol, bstopped != 0});
                    continue;
                }
            }
        }
        fclose(f);

        ma_device_start(&device);
        RequestSetGridSize(g_gridW, g_gridH);
        setTempo(loadedBpm);
        activateScene(0);
        if (missingClips > 0) SetStatus("Project loaded (%d clip(s) missing, see console)", missingClips);
        else SetStatus("Project loaded: %d scene(s)", (int)g_scenes.size());
    };

    // ---------------- Mods en Lua: puente app <-> scripts ----------------
    // Rellena el ScriptContext con lambdas que envuelven las mismas acciones de
    // la UI, y lo publica en g_scriptCtx para que la API de Lua lo use.
    ScriptContext scriptCtx;
    auto clampCell = [&](int& x, int& y) {
        x = x < 0 ? 0 : (x >= g_gridW ? g_gridW - 1 : x);
        y = y < 0 ? 0 : (y >= g_gridH ? g_gridH - 1 : y);
    };
    scriptCtx.registerCell = [&](const ModCellDef& d) {
        // Evita duplicados por nombre al recargar.
        for (auto& e : g_modCells) if (e.name == d.name) { e = d; return; }
        g_modCells.push_back(d);
    };
    scriptCtx.spawn = [&](int x, int y, int dir, int slot, float speed) {
        clampCell(x, y);
        dir = ((dir % 4) + 4) % 4;
        if (slot < 0 || slot >= MAX_SLOTS) slot = activeSlot();
        if (speed < 0.05f) speed = 0.05f;
        spawnBug(x, y, kDirDx[dir], kDirDy[dir], slot, speed);
    };
    scriptCtx.clearNoteys = [&]() { clearBugs(); };
    scriptCtx.paint = [&](int x, int y, int semi, int slot) {
        clampCell(x, y);
        if (slot < 0 || slot >= MAX_SLOTS) slot = activeSlot();
        MirrorCell& mc = g_mirror[y * g_gridW + x];
        mc = MirrorCell();
        mc.kind = CELL_COLOR;
        mc.pitchIdx = semi;
        mc.color = g_palette[((semi % 12) + 12) % 12].displayColor;
        mc.clip = slot;
        sendCellToEngine(x, y, mc);
    };
    scriptCtx.erase = [&](int x, int y) {
        clampCell(x, y);
        g_mirror[y * g_gridW + x] = MirrorCell();
        RequestPaintCell(x, y, 0, 0, ModifierType::None, 0, 0, 0);
    };
    scriptCtx.playLive = [&](int slot, int semi) {
        if (slot < 0 || slot >= MAX_SLOTS) slot = activeSlot();
        RequestTriggerLive(slot, SemitoneToColorId(semi), 0);
    };
    scriptCtx.linearSet = [&](int col, int row, int slot, int semi) {
        if (slot < 0 || slot >= MAX_SLOTS) slot = activeSlot();
        linearPlace(col, row, slot, semi, 0);
    };
    scriptCtx.linearClear = [&]() { linearClearAll(); };
    scriptCtx.setBpm = [&](float v) { setTempo(v); };
    scriptCtx.status = [&](const char* msg) { SetStatus("mod: %s", msg); };
    scriptCtx.gridW = [&]() { return g_gridW; };
    scriptCtx.gridH = [&]() { return g_gridH; };
    scriptCtx.bpm = [&]() { return bpmDisplay; };
    scriptCtx.playing = [&]() { return !paused; };
    scriptCtx.playheadLinear = [&]() { return g_snapshotPublisher.read().linearCol; };
    scriptCtx.playheadTracker = [&]() { return g_snapshotPublisher.read().trackerRow; };
    scriptCtx.noteyCount = [&]() { return g_snapshotPublisher.read().count; };
    scriptCtx.notey = [&](int i, int& x, int& y, int& dx, int& dy, int& slot, bool& playing) -> bool {
        const SnapshotBuffer& s = g_snapshotPublisher.read();
        if (i < 0 || i >= s.count || !s.bichos[i].isActive) return false;
        x = s.bichos[i].x; y = s.bichos[i].y; dx = 0; dy = 0;
        slot = s.bichos[i].sampleId; playing = s.bichos[i].isPlaying;
        return true;
    };
    g_scriptCtx = &scriptCtx;
    g_scripts.init();
    // Auto-carga mods de la carpeta mods/ al arrancar (si existe).
    if (DirectoryExists("mods")) {
        FilePathList mf = LoadDirectoryFiles("mods");
        for (unsigned int i = 0; i < mf.count; i++) {
            if (IsFileExtension(mf.paths[i], ".lua")) {
                std::string e;
                if (g_scripts.loadFile(mf.paths[i], e)) printf("MOD loaded: %s\n", mf.paths[i]);
                else printf("MOD failed %s: %s\n", mf.paths[i], e.c_str());
            }
        }
        UnloadDirectoryFiles(mf);
        if (!g_scripts.list().empty()) SetStatus("Loaded %d mod(s)", (int)g_scripts.list().size());
    }

    // Auto-carga modelos 3D (.glb/.vrm) de assets/models/ al arrancar.
    if (DirectoryExists("assets/models")) {
        FilePathList mf = LoadDirectoryFiles("assets/models");
        int mi = 0;
        for (unsigned int i = 0; i < mf.count && mi < MAX_MODELS; i++) {
            if (IsFileExtension(mf.paths[i], ".glb") || IsFileExtension(mf.paths[i], ".gltf") || IsFileExtension(mf.paths[i], ".vrm")) {
                if (LoadModelSlot(mi, mf.paths[i])) { printf("MODEL loaded: %s (%d anims)\n", mf.paths[i], g_models[mi].animCount); mi++; }
            }
        }
        UnloadDirectoryFiles(mf);
        if (mi > 0) SetStatus("Loaded %d 3D model(s)", mi);
    }

    // Hay algo que valga la pena preguntar al cerrar? Un lienzo en blanco y
    // cero grabaciones no merecen un modal: se cierra y ya.
    auto quitWorthAsking = [&]() {
        if (!g_sessionClips.empty()) return true;
        for (const Scene& sc : g_scenes) {
            if (!sc.bugs.empty()) return true;
            for (int i = 0; i < g_gridW * g_gridH; i++) if (!cellIsBlank(sc.cells[i])) return true;
        }
        return false;
    };

    // Borra SÓLO lo grabado en esta sesión (ver RegisterSessionClip): temp/
    // puede tener material de días anteriores y no es nuestro para tirarlo.
    auto discardSessionClips = [&]() {
        int n = 0;
        for (const std::string& p : g_sessionClips) if (remove(p.c_str()) == 0) n++;
        g_sessionClips.clear();
        return n;
    };

    // El bucle ya NO termina por WindowShouldClose(): esa señal abre el modal
    // de cierre y es el modal el que decide. Ojo, WindowShouldClose() se
    // rearma sola cada fotograma en raylib, así que hay que capturarla aquí.
    while (!quitNow) {
        if (WindowShouldClose() && !quitDialogOpen) {
            if (quitWorthAsking()) quitDialogOpen = true;
            else quitNow = true;
        }
        float dt = GetFrameTime();
        if (g_statusTimer > 0.0f) g_statusTimer -= dt;

        // ---------- LOS MÓVILES: estado de slots y peticiones de grabación ----------
        // Pinguus Cam enseña la misma fila de slots que el PC, así que hay que
        // decirle cuáles están ocupados; y cuando alguien pulsa uno libre, la
        // grabación se hace AQUÍ. El hilo de red no puede hacerla: no le está
        // permitido tocar el banco de slots ni dibujar, y grabar implica ambas.
        //
        // Las peticiones se atienden UNA A UNA, en el orden en que llegaron. No
        // hace falta un candado para conseguirlo: recordFromPhoneDev() no
        // devuelve el control hasta que termina, así que dos móviles que pulsen
        // a la vez se sirven en fila y al segundo se le avisa de que espere.
        {
            static float slotPushTimer = 0.0f;
            slotPushTimer -= dt;
            if (slotPushTimer <= 0.0f && g_phone.isRunning() && g_phone.deviceCount() > 0) {
                slotPushTimer = 0.7f;
                uint8_t clipUsed[PhoneLink::kClipSlots], smpUsed[PhoneLink::kSampleSlots];
                for (int i = 0; i < PhoneLink::kClipSlots; i++)
                    clipUsed[i] = (uint8_t)(clipState(i) != 0 ? 1 : 0);
                for (int i = 0; i < PhoneLink::kSampleSlots; i++)
                    smpUsed[i] = (uint8_t)(clipState(SAMPLE_BASE + i) != 0 ? 1 : 0);
                g_phone.broadcastSlots(clipUsed, smpUsed, allowPhoneRec, false, 0);
            }

            std::vector<PhoneLink::RecordRequest> reqs = g_phone.takeRequests();
            // Al que no va primero se le dice YA que está en cola, antes de que
            // el de delante bloquee la ventana: si no, se quedaría mirando un
            // botón que no responde sin saber por qué.
            for (size_t i = 1; i < reqs.size(); i++)
                g_phone.sendRecStatus(reqs[i].device, PhoneLink::kStatQueued, 0, (int)i,
                                      reqs[i].slot, "Another phone is recording - you are next");

            for (size_t i = 0; i < reqs.size(); i++) {
                const PhoneLink::RecordRequest& r = reqs[i];
                const char* refuse = nullptr;
                if (!allowPhoneRec)       refuse = "The PC has phone recording switched off";
                else if (recording)       refuse = "The PC is exporting right now - try again in a moment";
                else if (r.slot < 0 || r.slot >= MAX_SLOTS) refuse = "That slot does not exist";
                else if (clipState(r.slot) != 0)            refuse = "That slot is already taken";
                if (refuse) {
                    g_phone.sendRecStatus(r.device, PhoneLink::kStatRefused, 0, 0, r.slot, refuse);
                    SetStatus("%s asked to record into %s - refused (%s)",
                              r.deviceName.c_str(), slotLabel(r.slot), refuse);
                    continue;
                }
                SetStatus("%s is recording %ds into %s", r.deviceName.c_str(), r.seconds, slotLabel(r.slot));
                recordFromPhoneDev(r.slot, r.withVideo, r.device, r.seconds);
            }
        }

        int cellSize = leftPanelWidth / g_gridW;
        if (viewH / g_gridH < cellSize) cellSize = viewH / g_gridH;
        int gridOffX = (leftPanelWidth - cellSize * g_gridW) / 2;
        int gridOffY = viewY0 + (viewH - cellSize * g_gridH) / 2;

#ifdef UI_SMOKE_TEST
        {
            static int frameNo = 0;
            frameNo++;
            if (frameNo == 30) {
                if (clipState(1) == 2) selectedClipSlot = 1;
                for (int x = 4; x < 28; x += 2) {
                    int semi = ((x / 2) % 8);
                    MirrorCell& mc = g_mirror[10 * g_gridW + x];
                    mc.kind = CELL_COLOR; mc.color = g_palette[((semi % 12) + 12) % 12].displayColor;
                    mc.clip = selectedClipSlot; mc.pitchIdx = semi;
                    sendCellToEngine(x, 10, mc);
                }
                int ax[4] = {29, 29, 2, 2}, ay[4] = {10, 14, 14, 10}, ad[4] = {1, 2, 3, 0};
                for (int i = 0; i < 4; i++) {
                    MirrorCell& mc = g_mirror[ay[i] * g_gridW + ax[i]];
                    mc.kind = CELL_ARROW; mc.dir = ad[i];
                    sendCellToEngine(ax[i], ay[i], mc);
                }
                MirrorCell& mf = g_mirror[14 * g_gridW + 10];
                mf.kind = CELL_FX; mf.fxType = 1; mf.color = g_palette[5].displayColor;
                mf.clip = selectedClipSlot; mf.pitchIdx = 5;
                sendCellToEngine(10, 14, mf);
                // Atributos superpuestos, en la fila que ya recorre el notey:
                // volumen sobre una nota, cámara lenta sobre otra, espera sobre
                // una tercera y un SILENCIO en una celda que estaba vacía.
                g_mirror[10 * g_gridW + 8].vol = 0.4f;   sendCellToEngine(8, 10, g_mirror[10 * g_gridW + 8]);
                g_mirror[10 * g_gridW + 12].tmul = 0.5f; sendCellToEngine(12, 10, g_mirror[10 * g_gridW + 12]);
                g_mirror[10 * g_gridW + 14].hold = 1.0f; sendCellToEngine(14, 10, g_mirror[10 * g_gridW + 14]);
                g_mirror[10 * g_gridW + 15].hold = 0.5f; sendCellToEngine(15, 10, g_mirror[10 * g_gridW + 15]); // silencio
                // Portal 1: entrance A at (18,10), exit B at (18,14).
                placeTeleport(18, 10, 0);
                placeTeleport(18, 14, 0);
                if (clipState(SAMPLE_BASE) == 1) {
                    MirrorCell& mk = g_mirror[10 * g_gridW + 7];
                    mk.kind = CELL_COLOR; mk.color = g_palette[6].displayColor;
                    mk.clip = SAMPLE_BASE; mk.pitchIdx = 6;
                    sendCellToEngine(7, 10, mk);
                    for (int r = 0; r < TRACKER_ROWS; r += 4) {
                        trackerPlace(0, r, SAMPLE_BASE, (r / 4) % 8, r % 8 == 0 ? 2 : 0);
                    }
                }
                spawnBug(4, 10, 1, 0, selectedClipSlot, 1.0f);
                spawnBug(4, 12, 1, 0, selectedClipSlot, 0.5f);
                SetStatus("Smoke test: painted");
            }
            if (frameNo == 40) {
                g_clipFX[1].flipX = true;
                g_clipFX[1].move = true;
                g_clipFX[1].layer = 7;
                g_clipFX[0].center = true;
                g_clipFX[0].layer = 2;
                if (FileExists("assets/Mentorius.gif")) loadFileIntoSlot(5, "assets/Mentorius.gif");
                // Replace clip 0's (video) visual with the GIF, keeping audio.
                if (FileExists("assets/Mentorius.gif")) {
                    ReplaceVisualIntoSlot(0, "assets/Mentorius.gif", g_transcodeMaxSide);
                    g_slotVisualPath[0] = "assets/Mentorius.gif";
                    auto it = textureCache.find(0);
                    if (it != textureCache.end()) { UnloadTexture(it->second); textureCache.erase(it); }
                }
                { MirrorCell& mg = g_mirror[8 * g_gridW + 12]; mg.kind = CELL_COLOR; mg.color = g_palette[4].displayColor; mg.clip = 5; mg.pitchIdx = 4; sendCellToEngine(12, 8, mg); }
                spawnBug(10, 8, 1, 0, 5, 1.0f);
                // Fase 2: coloca una celda de mod (si algún mod registró alguna).
                if (!g_modCells.empty()) {
                    MirrorCell& mm = g_mirror[9 * g_gridW + 16];
                    mm = MirrorCell(); mm.kind = CELL_MODCELL; mm.modCellId = 0;
                    mm.color = {g_modCells[0].r, g_modCells[0].g, g_modCells[0].b, 255};
                    sendCellToEngine(16, 9, mm);
                }
                // GLSL video shader on a clip (compiles + renders + save/load).
                if (FileExists("assets/glsl/wave.fs")) {
                    if (loadShaderFromFile(1, "assets/glsl/wave.fs")) SetStatus("Smoke: GLSL shader loaded");
                    else SetStatus("Smoke: GLSL shader FAILED");
                }
                // Modo lineal (estilo canvas): notas en varios carriles con
                // tonos crecientes, para verificar colocación, FX-sobre-nota,
                // save/load y el playhead.
                {
                    int lsample = clipState(SAMPLE_BASE) == 1 ? SAMPLE_BASE : selectedClipSlot;
                    linearSetRows(4);
                    for (int col = 0; col < 12; col++) {
                        int row = col % 4;
                        linearPlace(col * 2, row, lsample, col, 0);
                    }
                    linearApplyFx(8, 0, 2);  // eco sobre una nota ya colocada (col8,row0)
                    linearApplyFx(9, 0, 1);  // FX sobre celda vacía: debe no hacer nada
                    g_linearLength = 24;
                    RequestLinearParams(g_linearLength, g_linearLoop);
                }
            }
            if (frameNo == 60) { pushUndo(); MirrorCell& mc = g_mirror[5 * g_gridW + 5]; mc.kind = CELL_COLOR; mc.color = g_palette[0].displayColor; mc.clip = selectedClipSlot; mc.pitchIdx = 0; sendCellToEngine(5, 5, mc); }
            if (frameNo == 70) doUndo();
            if (frameNo == 90 && FileExists("smoke_test.mid")) importMidi("smoke_test.mid");
            if (frameNo == 120) startRecording(true);
            // Live voices (what a MIDI key / gamepad Y button triggers): play a
            // clip slot at a couple of pitches — exercises audio + video path.
            if (frameNo == 130) RequestTriggerLive(1, SemitoneToColorId(0), 0);
            if (frameNo == 150) RequestTriggerLive(1, SemitoneToColorId(5), 0);
            // Modelo 3D: coloca una celda de modelo en el loop de noteys (ruta real
            // de disparo) y, para el screenshot, fuerza una voz de modelo activa.
            if (frameNo == 45 && g_models[0].loaded) {
                MirrorCell& mmc = g_mirror[10 * g_gridW + 20];
                mmc = MirrorCell(); mmc.kind = CELL_COLOR; mmc.color = g_palette[0].displayColor;
                mmc.clip = modelAnimId(0, 0); mmc.pitchIdx = 0;
                sendCellToEngine(20, 10, mmc);
            }
            if (frameNo == 46) {
                // Diagnóstico de los modelos auto-cargados (incluye el .vrm real).
                for (int mi = 0; mi < MAX_MODELS; mi++) if (g_models[mi].loaded)
                    printf("SMOKE: model %d '%s' meshes=%d skeletalAnims=%d humanoid=%d\n", mi,
                           GetFileName(g_models[mi].path.c_str()), g_models[mi].model.meshCount,
                           g_models[mi].animCount, (int)g_models[mi].humanoid);
            }
            if (frameNo == 250 && g_models[0].loaded) { selectedModel = 0; activeBar = 2; selectedModelAnim = modelAnimId(0, 3); }
            if (frameNo == 255 && g_models[0].loaded) {
                g_modelVoices[0].active = true; g_modelVoices[0].id = 90001;
                g_modelVoices[0].model = 0; g_modelVoices[0].anim = 0; g_modelVoices[0].frame = 8.0f;
                g_models[0].rot = {0, 30, 0}; // probar transform
            }
            if (frameNo == 258) {
                // Editor sobre un modelo HUMANOIDE VRM 0.x (baseYaw=180, antes de
                // espaldas) con toon + un .vrma: verifica que ahora mira de frente.
                int hs = -1;
                for (int mi = 0; mi < MAX_MODELS; mi++) if (g_models[mi].loaded && g_models[mi].humanoid && g_models[mi].baseYaw > 0) { hs = mi; break; }
                if (hs < 0) for (int mi = 0; mi < MAX_MODELS; mi++) if (g_models[mi].loaded && g_models[mi].humanoid) { hs = mi; break; }
                if (hs < 0) hs = 0;
                printf("SMOKE: editor model %d '%s' baseYaw=%.0f\n", hs, GetFileName(g_models[hs].path.c_str()), g_models[hs].baseYaw);
                modelEditorSlot = hs; modelEditorOpen = true;
                VrmaClip clip;
                if (FileExists("assets/models/vrma/VRMA_01.vrma") && loadVrmaClip("assets/models/vrma/VRMA_01.vrma", clip)) {
                    int have = 0; for (int r = 0; r < HB_COUNT; r++) if (clip.track[r].has) have++;
                    printf("SMOKE: VRMA imported '%s' dur=%.1fs tracks=%d\n", clip.name.c_str(), clip.duration, have);
                    g_models[hs].clips.push_back(clip);
                    g_models[hs].animNames.push_back(clip.name);
                    modelEditorAnim = kHumanoidAnimCount; modelEditorFrame = clip.duration * 60.0f * 0.35f;
                } else { modelEditorAnim = 2; printf("SMOKE: VRMA import FAILED\n"); }
                // Mocap .bvh: la MISMA tubería de retargeting, distinto lector.
                // Se importa por la ruta real (loadHumanoidAnimFile) y se deja
                // seleccionada, así el screenshot del editor la enseña animando.
                {
                    VrmaClip bclip;
                    if (FileExists("assets/models/anims/wave.bvh") &&
                        loadHumanoidAnimFile("assets/models/anims/wave.bvh", bclip)) {
                        int have = 0; for (int r = 0; r < HB_COUNT; r++) if (bclip.track[r].has) have++;
                        printf("SMOKE: BVH imported '%s' dur=%.1fs tracks=%d isBvh=%d\n",
                               bclip.name.c_str(), bclip.duration, have, (int)bclip.isBvh);
                        g_models[hs].clips.push_back(bclip);
                        g_models[hs].animNames.push_back(bclip.name);
                        modelEditorAnim = kHumanoidAnimCount + (int)g_models[hs].clips.size() - 1;
                        modelEditorFrame = bclip.duration * 60.0f * 0.30f;
                    } else printf("SMOKE: BVH import FAILED\n");
                }
                g_models[hs].toon = true; ApplyModelShader(g_models[hs]);
            }
            // El retargeting del .bvh se comprueba con NÚMEROS, no a ojo: en el
            // saludo el antebrazo derecho tiene que acabar POR ENCIMA del hombro.
            // (Así se fijó el sentido por defecto de flipY: al revés queda debajo.)
            if (frameNo == 262 && modelEditorSlot >= 0 && !g_models[modelEditorSlot].clips.empty()) {
                ModelSlot& ds = g_models[modelEditorSlot];
                int bvhAnim = kHumanoidAnimCount + (int)ds.clips.size() - 1;
                computeHumanoidPose(ds, bvhAnim, 0.30f);
                int bs = ds.hbone[HB_R_UPPERARM], be = ds.hbone[HB_R_LOWERARM];
                if (bs >= 0 && be >= 0) {
                    float shoulderY = ds.procAnim.framePoses[0][bs].translation.y;
                    float elbowY = ds.procAnim.framePoses[0][be].translation.y;
                    printf("SMOKE: BVH retarget shoulderY=%.2f elbowY=%.2f -> %s\n",
                           shoulderY, elbowY, elbowY > shoulderY ? "arm RAISED (ok)" : "arm DOWN (mirrored!)");
                }
            }
            if (frameNo == 266) TakeScreenshot("smoke_modeleditor.png");
            if (frameNo == 268) modelEditorOpen = false;
            if (frameNo == 272) TakeScreenshot("smoke_model.png");
            if (frameNo == 280) TakeScreenshot("smoke_shot.png");
            if (frameNo == 300) stopRecording();
            if (frameNo == 100) { // probar el TEMA PROPIO editable
                g_themeIdx = kThemeCount;
                g_customTheme.bg = {18, 10, 26, 255}; g_customTheme.accent = {255, 130, 210, 255};
                g_customTheme.button = {60, 40, 70, 255};
                ApplyTheme();
            }
            if (frameNo == 320) openEditor(clipState(1) == 2 ? 1 : 0); // editor de CLIP (verifica botones)
            if (frameNo == 380) TakeScreenshot("smoke_editor.png");
            if (frameNo == 400) editorOpen = false;
            // Per-notey STOP: freeze one notey (respawns stopped via activateScene;
            // also exercises save/load of the flag).
            if (frameNo == 402 && !g_scenes[g_curScene].bugs.empty()) {
                g_scenes[g_curScene].bugs[0].stopped = true;
                activateScene(g_curScene);
            }
            if (frameNo == 405) saveProject("smoke_project.smt");
            if (frameNo == 415) loadProject("smoke_project.smt");
            // Compatibilidad hacia atrás: un proyecto del formato ANTIGUO, donde
            // la espera era una celda entera (kind 3 = CELL_SUSTAIN) y la línea
            // tenía 9 campos, no 12. Tiene que quedar como nota + espera.
            if (frameNo == 420) {
                FILE* lf = fopen("smoke_legacy.smt", "w");
                if (lf) {
                    fprintf(lf, "SIMTUNES_PROJECT 8\ngrid 32 24\nscene 0 duration 0.00\n");
                    fprintf(lf, "cell 0 3 3 3 5 0 0 2.00 0\n");   // SUSTAIN de 2s con nota
                    fprintf(lf, "cell 0 4 3 1 7 0 0 1.00 0\n");   // nota normal
                    fclose(lf);
                    loadProject("smoke_legacy.smt");
                    const MirrorCell& a = g_mirror[3 * g_gridW + 3];
                    const MirrorCell& b = g_mirror[3 * g_gridW + 4];
                    printf("SMOKE legacy: sustain-> kind=%d hold=%.2f pitch=%d | plain-> kind=%d hold=%.2f\n",
                           (int)a.kind, a.hold, a.pitchIdx, (int)b.kind, b.hold);
                    fflush(stdout);
                    loadProject("smoke_project.smt");   // vuelve al proyecto de la prueba
                }
            }
            // Interoperabilidad con el TELÉFONO: un .smt escrito por la
            // aplicación de Android tiene que abrirse aquí tal cual. Es la
            // razón de que el puerto use este formato y no uno propio.
            if (frameNo == 425) {
                // El fichero se escribe AQUÍ, literal, tal y como lo emite
                // AndroidProject::Save() en el teléfono: así la comprobación
                // corre siempre y no depende de que alguien haya dejado un
                // .smt suelto. Que el teléfono emita de verdad esto lo prueba
                // la otra parte, la ida y vuelta de androidapp.
                FILE* phf = fopen("smoke_phone.smt", "w");
                if (phf) {
                    fprintf(phf,
                        "SIMTUNES_PROJECT 9\noctave 2\nmaster 0.800\nbpm 140.00\ngrid 24 18\n"
                        "clip 3 trim 12 48 path /storage/emulated/0/Movies/x.mp4\n"
                        "atrim 3 4410 88200\n"
                        "fx 3 0 0 1 1 1.50 layer 7 move 0 0.200 0.500 0.800 0.500\n"
                        "scene 0 duration 0.00\n"
                        "cell 0 2 3 1 7 3 0 1.00 0 0.00 1.000 1.000\n"
                        "cell 0 4 3 5 -5 1 0 1.00 2 2.00 0.400 0.500\n"
                        "cell 0 5 3 0 0 0 0 1.00 0 0.50 1.000 1.000\n"
                        "cell 0 1 5 2 0 0 2 1.00 0 0.00 1.000 1.000\n"
                        "cell 0 1 7 6 0 0 2 1.00 0 0.00 1.000 1.000\n"
                        "cell 0 9 7 6 0 0 2 1.00 1 0.00 1.000 1.000\n"
                        "bug 0 1 3 1 0 3 0.500 1 0.750 0\n");
                    fclose(phf);
                }
                loadProject("smoke_phone.smt");
                const MirrorCell& pn = g_mirror[3 * g_gridW + 2];   // nota normal
                const MirrorCell& pf = g_mirror[3 * g_gridW + 4];   // nota + FX + los 3 atributos
                const MirrorCell& pr = g_mirror[3 * g_gridW + 5];   // silencio
                const MirrorCell& pp = g_mirror[7 * g_gridW + 1];   // portal A
                printf("SMOKE phone: grid=%dx%d oct=%d bpm=%.0f noteys=%d\n",
                       g_gridW, g_gridH, g_octave, bpmDisplay, (int)g_scenes[0].bugs.size());
                printf("SMOKE phone: nota kind=%d pitch=%d | fx kind=%d fx=%d hold=%.2f vol=%.2f time=%.2f\n",
                       (int)pn.kind, pn.pitchIdx, (int)pf.kind, pf.fxType, pf.hold, pf.vol, pf.tmul);
                printf("SMOKE phone: silencio kind=%d hold=%.2f | portal kind=%d id=%d rol=%d\n",
                       (int)pr.kind, pr.hold, (int)pp.kind, pp.dir, pp.fxType);
                printf("SMOKE phone: clipfx rotate=%d center=%d scale=%.2f layer=%d\n",
                       g_clipFX[3].rotate ? 1 : 0, g_clipFX[3].center ? 1 : 0,
                       g_clipFX[3].scale, g_clipFX[3].layer);
                fflush(stdout);
                loadProject("smoke_project.smt");
            }
            if (frameNo == 450) activeView = 1;
            if (frameNo == 470) TakeScreenshot("smoke_loaded.png");
            if (frameNo == 480) activeView = 2; // LINEAR view (piano-roll)
            if (frameNo == 495) TakeScreenshot("smoke_linear.png");
            if (frameNo == 500) { activeView = 0; showNoteyList = true; } // notey list w/ play/stop
            if (frameNo == 508) TakeScreenshot("smoke_noteys.png");
            if (frameNo == 512) clearSlot(5); // empty a slot back to blank
            if (frameNo == 516) { showDevices = true; devTab = 0; } // DEVICES: mandos
            if (frameNo == 524) TakeScreenshot("smoke_devices.png");
            // Pestaña del TELÉFONO: comparte el APK y dibuja las tres rutas.
            if (frameNo == 528) { devTab = 1; g_apkServer.start(kApkPort, kApkPath); }
            if (frameNo == 538) TakeScreenshot("smoke_phone.png");
            if (frameNo == 542) { g_apkServer.stop(); showDevices = false; }
            if (frameNo == 546) showMods = true;    // MODS panel (Lua scripts)
            if (frameNo == 552) TakeScreenshot("smoke_mods.png");
            if (frameNo == 556) showMods = false;

            // ---- Capturas extra SOLO para el manual (docs/) ----
            // El recorrido de arriba existe para VERIFICAR que las funciones
            // andan; estas de aquí abajo son para ILUSTRARLAS, así que abren
            // pantallas que la prueba no necesitaba abrir.
            if (frameNo == 560) { activeView = 1; }              // TRACKER
            if (frameNo == 568) TakeScreenshot("smoke_tracker.png");
            if (frameNo == 572) { activeView = 0; openEditor(SAMPLE_BASE); }  // editor de SAMPLE (onda)
            if (frameNo == 590) TakeScreenshot("smoke_sampleeditor.png");
            if (frameNo == 594) editorOpen = false;
            if (frameNo == 598) { slotChoiceOpen = true; slotChoiceSlot = 6; slotChoiceVideo = true; }
            if (frameNo == 604) TakeScreenshot("smoke_slotchooser.png");
            if (frameNo == 608) slotChoiceOpen = false;
            if (frameNo == 612) startupOpen = true;              // diálogo de inicio
            if (frameNo == 618) TakeScreenshot("smoke_startup.png");
            if (frameNo == 622) startupOpen = false;
            // Modal de cierre. Se abre a mano (no llega el evento de cerrar
            // ventana en modo automático) sólo para dibujarlo y fotografiarlo.
            if (frameNo == 626) quitDialogOpen = true;
            if (frameNo == 632) TakeScreenshot("smoke_quit.png");
            if (frameNo == 636) quitDialogOpen = false;

            // ---- Modo BEATBOX ----
            // Monta un kit de cuatro pads (uno por modo de disparo), golpea un
            // par, mete el filtro y graba un patrón, que es todo lo que hace la
            // sampleadora. Lo importante es lo que se IMPRIME al final: que el
            // motor recibió los golpes y que el patrón quedó escrito.
            if (frameNo == 646) {
                activeView = 3;
                g_pads[0].slot = 0;                                    // CLIP 1, one-shot
                g_pads[1].slot = 1;  g_pads[1].mode = PAD_GATE;
                g_pads[2].slot = SAMPLE_BASE; g_pads[2].mode = PAD_LOOP;
                g_pads[3].slot = 2;  g_pads[3].choke = 1; g_pads[3].pitch = 5;
                g_pads[7].slot = 3;  g_pads[7].choke = 1; g_pads[7].fx = 2;
                for (int i = 0; i < 8; i++) RequestSetPadConfig(i);
                padSel = 3;
            }
            if (frameNo == 650) { padPatPlaying = true; padRecArm = true;
                                  RequestPadTransport(true, true, padQuant, padSteps); }
            if (frameNo == 652) RequestPadTrigger(0, 1.0f);
            if (frameNo == 656) RequestPadTrigger(3, 0.8f);
            if (frameNo == 660) RequestPadTrigger(7, 1.0f);   // corta al 3 (mismo grupo)
            if (frameNo == 662) { mfxType = MFX_FILTER; mfxX = 0.35f; mfxY = 0.7f; mfxOn = true;
                                  RequestSetMasterFx(mfxType, mfxX, mfxY, true); }
            if (frameNo == 666) RequestPadTrigger(2, 1.0f);   // bucle
            if (frameNo == 672) TakeScreenshot("smoke_beatbox.png");
            if (frameNo == 676) {
                const SnapshotBuffer& bs = g_snapshotPublisher.read();
                int hits = 0;
                for (int t = 0; t < SNAP_PAD_TICKS; t++) if (bs.padPattern[t]) hits++;
                printf("SMOKE beatbox: sonando=0x%llx bucle=0x%llx tick=%d pasos=%d rec=%d ticks_con_golpe=%d\n",
                       (unsigned long long)bs.padPlaying, (unsigned long long)bs.padLooping,
                       bs.padTick, bs.padSteps, bs.padRecording ? 1 : 0, hits);
                fflush(stdout);
            }
            if (frameNo == 680) { padRecArm = false; padPatPlaying = false;
                                  RequestPadTransport(false, false, padQuant, padSteps);
                                  RequestPadStopAll();
                                  mfxOn = false; RequestSetMasterFx(mfxType, mfxX, mfxY, false); }
            // Ida y vuelta por disco: el kit y el patrón tienen que sobrevivir a
            // guardar y volver a abrir, que es donde se rompen estas cosas.
            if (frameNo == 684) saveProject("smoke_beatbox.smt");
            if (frameNo == 690) loadProject("smoke_beatbox.smt");
            if (frameNo == 706) {
                const SnapshotBuffer& bs = g_snapshotPublisher.read();
                int hits = 0, filled = 0;
                for (int t = 0; t < SNAP_PAD_TICKS; t++) if (bs.padPattern[t]) hits++;
                for (int i = 0; i < PAD_TOTAL; i++) if (!g_pads[i].empty()) filled++;
                printf("SMOKE beatbox reload: pads_con_sonido=%d ticks_con_golpe=%d "
                       "pad3(slot=%d tono=%d choke=%d) pad2(modo=%d)\n",
                       filled, hits, g_pads[3].slot, (int)g_pads[3].pitch, (int)g_pads[3].choke,
                       (int)g_pads[2].mode);
                fflush(stdout);
            }

            // ---- Capas: colocación libre, transparencia y modos de fusión ----
            if (frameNo == 712) {
                activeView = 0;
                if (paused) togglePause();          // que haya clips sonando y por tanto visibles
                g_clipFX[0].place = true;  g_clipFX[0].center = false; g_clipFX[0].move = false;
                g_clipFX[0].posX = 0.28f;  g_clipFX[0].posY = 0.34f;
                g_clipFX[0].rotDeg = 15.0f; g_clipFX[0].scale = 1.5f;
                g_clipFX[0].opacity = 0.65f; g_clipFX[0].blend = BLEND_FX_SCREEN;
                g_clipFX[1].place = true;  g_clipFX[1].posX = 0.72f; g_clipFX[1].posY = 0.66f;
                g_clipFX[1].opacity = 0.80f; g_clipFX[1].blend = BLEND_FX_MULTIPLY;
                g_clipFX[1].move = false;  g_clipFX[1].center = false;
            }
            // ---- Vídeo analógico (NTSC/VHS) sobre la mezcla final ----
            if (frameNo == 718) {
                g_ntsc.presetVHS();
                g_ntsc.enabled = g_ntscShader.load();
                printf("SMOKE ntsc: shader compilado=%d\n", g_ntscShader.ok ? 1 : 0);
                fflush(stdout);
            }
            if (frameNo == 726) TakeScreenshot("smoke_layers.png");
            // El lector de presets de ntsc-rs, con un archivo de su formato
            // real: objeto PLANO, "version": 1, y grupos como un booleano
            // suelto junto a sus hijos.
            if (frameNo == 730) {
                FILE* jf = fopen("smoke_ntsc_preset.json", "w");
                if (jf) {
                    fprintf(jf, "{\n");
                    fprintf(jf, "  \"composite_noise\": true,\n");
                    fprintf(jf, "  \"composite_noise_intensity\": 0.25,\n");
                    fprintf(jf, "  \"luma_noise\": false,\n");
                    fprintf(jf, "  \"luma_noise_intensity\": 0.9,\n");
                    fprintf(jf, "  \"chroma_noise\": true,\n");
                    fprintf(jf, "  \"chroma_noise_intensity\": 0.4,\n");
                    fprintf(jf, "  \"snow_intensity\": 0.12,\n");
                    fprintf(jf, "  \"chroma_delay_horizontal\": 2.0,\n");
                    fprintf(jf, "  \"chroma_phase_noise_intensity\": 0.33,\n");
                    fprintf(jf, "  \"ringing\": true,\n");
                    fprintf(jf, "  \"ringing_power\": 2.5,\n");
                    fprintf(jf, "  \"ringing_frequency\": 0.4,\n");
                    fprintf(jf, "  \"composite_preemphasis\": 1.6,\n");
                    fprintf(jf, "  \"vhs_settings\": true,\n");
                    fprintf(jf, "  \"vhs_edge_wave\": 1.5,\n");
                    fprintf(jf, "  \"vhs_chroma_loss\": 0.2,\n");
                    fprintf(jf, "  \"vhs_tape_speed\": 2,\n");
                    fprintf(jf, "  \"head_switching\": true,\n");
                    fprintf(jf, "  \"head_switching_height\": 8,\n");
                    fprintf(jf, "  \"random_seed\": 918273645,\n");
                    fprintf(jf, "  \"use_field\": 1,\n");
                    fprintf(jf, "  \"filter_type\": 0,\n");
                    fprintf(jf, "  \"version\": 1\n}\n");
                    fclose(jf);
                }
                NtscPresetResult pr = LoadNtscPreset("smoke_ntsc_preset.json", g_ntsc);
                printf("SMOKE ntsc preset: ok=%d traducidos=%d ignorados=%d | ruido=%.2f croma=%.2f "
                       "lumaruido=%.2f(grupo off) cabezal=%.2f cinta=%.2f\n",
                       pr.ok ? 1 : 0, pr.applied, pr.ignored, g_ntsc.noise, g_ntsc.chromaNoise,
                       g_ntsc.lumaNoise, g_ntsc.headSwitch, g_ntsc.tapeBlur);
                fflush(stdout);
            }
            if (frameNo == 734) showNtsc = true;
            if (frameNo == 742) TakeScreenshot("smoke_vhs.png");
            if (frameNo == 746) showNtsc = false;
            // El editor de clip, con la fila nueva y el escenario de colocación.
            if (frameNo == 750) openEditor(0);
            if (frameNo == 762) TakeScreenshot("smoke_place.png");
            if (frameNo == 766) editorOpen = false;
            // Ida y vuelta por disco de todo lo visual nuevo.
            if (frameNo == 770) saveProject("smoke_layers.smt");
            if (frameNo == 776) loadProject("smoke_layers.smt");
            if (frameNo == 792) {
                printf("SMOKE layers reload: clip0(place=%d x=%.2f y=%.2f rot=%.0f op=%.2f blend=%d) "
                       "clip1(op=%.2f blend=%d) ntsc(on=%d ruido=%.2f)\n",
                       g_clipFX[0].place ? 1 : 0, g_clipFX[0].posX, g_clipFX[0].posY,
                       g_clipFX[0].rotDeg, g_clipFX[0].opacity, g_clipFX[0].blend,
                       g_clipFX[1].opacity, g_clipFX[1].blend,
                       g_ntsc.enabled ? 1 : 0, g_ntsc.noise);
                fflush(stdout);
            }

            // ---- MELODY: de un sonido a notas ----
            // Se fabrica una escala de sierra de tono EXACTO, se mete en un
            // slot como si viniera del micrófono, se analiza y se escribe en el
            // lienzo. Así se prueba el camino entero — detector, cuantización y
            // escritura — sin depender de que haya un micrófono conectado.
            if (frameNo == 800) {
                // Un tarareo LARGO y con microcortes entre notas, que es lo que
                // sale de verdad de un micrófono — no notas separadas por
                // silencios cómodos.
                const int sr = 44100;
                const int scale[12] = {0, 2, 4, 5, 7, 9, 11, 12, 11, 7, 4, 0};
                std::vector<float> pcm;
                float phase = 0.0f;
                for (int i = 0; i < (int)(0.60f * sr); i++) pcm.push_back(0.0f);  // silencio al empezar
                for (int k = 0; k < 12; k++) {
                    float hz = 440.0f * powf(2.0f, (scale[k] - 9) / 12.0f);
                    for (int i = 0; i < (int)(0.45f * sr); i++) {
                        phase += hz / sr;
                        if (phase >= 1.0f) phase -= 1.0f;
                        pcm.push_back((2.0f * phase - 1.0f) * 0.5f);
                    }
                    for (int i = 0; i < (int)(0.03f * sr); i++) pcm.push_back(0.0f);
                }
                melSrcSlot = SAMPLE_BASE + 15;      // un slot de sample libre
                melIdx = 1;                         // se guardará en la ranura 2
                ma_device_stop(&device);
                g_engine.getInstrumentBank().LoadAudioFromPCM(melSrcSlot, pcm.data(), pcm.size(), false);
                ma_device_start(&device);
                showMelody = true;
            }
            if (frameNo == 806) {
                const InstrumentSource& ms = g_engine.getInstrumentBank().at(melSrcSlot);
                melResult = PitchToNotes::Analyse(ms.audio.pPCM, (size_t)ms.audio.totalFrames, 44100, melOpt);
                melAnalysed = true;
                std::string got;
                for (const auto& n : melResult.notes) got += TextFormat("%d ", n.semitone);
                printf("SMOKE melody: %.1f s de audio -> notas=%d tonos=[%s] con_tono=%d%%\n",
                       (double)ms.audio.totalFrames / 44100.0,
                       (int)melResult.notes.size(), got.c_str(),
                       melResult.framesAnalysed ? melResult.framesVoiced * 100 / melResult.framesAnalysed : 0);
                fflush(stdout);
            }
            if (frameNo == 812) TakeScreenshot("smoke_melody.png");
            // Guardar en la ranura 2 del banco, como hace el botón SAVE.
            if (frameNo == 816) {
                setTempo(120.0f);
                melIdx = 1;
                // FIT: la resolución más fina que TODAVÍA cabe en la rejilla.
                // A 1/16 este tarareo pediría más casillas de las que hay.
                static const int kDivVal[3] = {4, 2, 1};
                melDiv = 1;
                for (int d = 0; d < 3; d++) {
                    auto probe = PitchToNotes::ToSteps(melResult, 120.0f, kDivVal[d], true, 0);
                    if (!probe.empty() && probe.back().step + 1 <= g_gridW) { melDiv = kDivVal[d]; break; }
                }
                auto st = PitchToNotes::ToSteps(melResult, 120.0f, melDiv, true, MAX_GW);
                g_melodies[melIdx].notes = st;
                g_melodies[melIdx].name = DefaultMelodyName(melIdx, (int)st.size());
                showMelody = false;
                selectedTool = TOOL_MELODY;
                printf("SMOKE melody bank: ranura=%d notas=%d ancho=%d celdas (rejilla %d, cell=1/%d)\n",
                       melIdx + 1, (int)g_melodies[melIdx].notes.size(),
                       g_melodies[melIdx].widthSteps(), g_gridW, melDiv * 4);
                fflush(stdout);
            }
            // Y estamparla en el lienzo, igual que un clic con la celda MELODY.
            if (frameNo == 820) {
                const int row = g_gridH / 2;
                pushUndo();
                for (const auto& n : g_melodies[melIdx].notes) {
                    int cx = n.step;
                    if (cx >= g_gridW) break;
                    MirrorCell& mc = g_mirror[row * g_gridW + cx];
                    mc = MirrorCell();
                    mc.kind = CELL_COLOR;
                    int semi = n.semitone + g_octave * 12;
                    mc.pitchIdx = semi;
                    mc.color = g_palette[((semi % 12) + 12) % 12].displayColor;
                    mc.clip = 0;
                    mc.vol = 0.35f + 0.65f * n.peak;
                    sendCellToEngine(cx, row, mc);
                }
                spawnBug(0, row, 1, 0, 0, 1.0f);
                int painted = 0;
                for (int x = 0; x < g_gridW; x++)
                    if (g_mirror[row * g_gridW + x].kind == CELL_COLOR) painted++;
                printf("SMOKE melody stamp: celdas_pintadas=%d fila=%d\n", painted, row);
                fflush(stdout);
            }
            if (frameNo == 826) TakeScreenshot("smoke_melody_canvas.png");
            // Ida y vuelta por disco del banco.
            if (frameNo == 830) saveProject("smoke_melody.smt");
            if (frameNo == 836) loadProject("smoke_melody.smt");
            if (frameNo == 852) {
                std::string got;
                for (const auto& n : g_melodies[1].notes) got += TextFormat("%d ", n.semitone);
                printf("SMOKE melody reload: ranura2_notas=%d tonos=[%s] nombre='%s'\n",
                       (int)g_melodies[1].notes.size(), got.c_str(), g_melodies[1].name.c_str());
                fflush(stdout);
            }
            if (frameNo == 856) {
                printf("SMOKE subidas de textura: pedidas=%lld hechas=%lld (%.0f%% ahorradas)\n",
                       g_uploadsAsked, g_uploadsDone,
                       g_uploadsAsked ? 100.0 * (g_uploadsAsked - g_uploadsDone) / g_uploadsAsked : 0.0);
                printf("SMOKE tiempos, MEDIANA de %d fotogramas: fotograma %.2f ms | "
                       "collage %.2f ms | captura %.2f ms\n",
                       g_frames, Median(g_sFrame) * 1000.0,
                       Median(g_sCollage) * 1000.0, Median(g_sCapture) * 1000.0);
                printf("SMOKE collage dibujado %lld de %lld fotogramas (%.0f%% ahorrados)\n",
                       g_collageDrawn, g_collageAsked,
                       g_collageAsked ? 100.0 * (g_collageAsked - g_collageDrawn) / g_collageAsked : 0.0);
                printf("SMOKE dentro del collage: clips 2D %.2f ms | modelos 3D %.2f ms\n",
                       Median(g_s2D) * 1000.0, Median(g_s3D) * 1000.0);
                // Y el peor, para ver si hay tirones aunque la mediana esté bien.
                if (!g_sFrame.empty())
                    printf("SMOKE peor fotograma: %.0f ms (los tirones son cargar/decodificar, no dibujar)\n",
                           g_sFrame.back() * 1000.0);
                fflush(stdout);
                break;
            }
        }
#endif

        // ---------------- Modo SONG ----------------
        if (songMode && !paused && g_scenes[g_curScene].duration > 0.0f) {
            sceneTimeLeft -= dt;
            if (sceneTimeLeft <= 0.0f) {
                int n = (int)g_scenes.size();
                for (int k = 1; k <= n; k++) {
                    int idx = (g_curScene + k) % n;
                    if (g_scenes[idx].duration > 0.0f) {
                        clearUndoHistory();
                        activateScene(idx);
                        break;
                    }
                }
            }
        }

        bool uiLocked = editorOpen || startupOpen || recChoiceOpen || slotChoiceOpen || showDevices || showMods || showNtsc || showMelody || modelEditorOpen || quitDialogOpen;

        // ---------------- Archivos arrastrados ----------------
        // Se aceptan SIEMPRE, también con un panel abierto. Antes se exigía
        // !uiLocked, así que soltar archivos con el editor, los mods o el panel
        // DEV delante no hacía absolutamente nada visible — y como el aviso de
        // por qué se imprimía por consola (que en Windows no existe), parecía
        // que el arrastrar y soltar estuviera roto.
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();

            if (dropped.count == 1 && IsFileExtension(dropped.paths[0], ".smt")) {
                loadProject(dropped.paths[0]);
                UnloadDroppedFiles(dropped);
            } else if (dropped.count == 1 &&
                       (IsFileExtension(dropped.paths[0], ".mid") || IsFileExtension(dropped.paths[0], ".midi"))) {
                importMidi(dropped.paths[0]);
                UnloadDroppedFiles(dropped);
            } else {
                int clipTarget = selectedClipSlot;
                int sampleTarget = selectedSampleSlot;
                {
                    Vector2 m = GetMousePosition();
                    for (int i = 0; i < PAGE_SIZE; i++) {
                        Rectangle r = {(float)(46 + i * 27), (float)row2Y + 4, 26, 24};
                        if (CheckCollisionPointRec(m, r)) { clipTarget = clipPage * PAGE_SIZE + i; break; }
                    }
                    for (int i = 0; i < PAGE_SIZE; i++) {
                        Rectangle r = {(float)(46 + i * 27), (float)row3Y + 4, 26, 24};
                        if (CheckCollisionPointRec(m, r)) { sampleTarget = SAMPLE_BASE + samplePage * PAGE_SIZE + i; break; }
                    }
                }

                BeginDrawing();
                ClearBackground(g_theme.bg);
                const char* loadingMsg = "Loading file(s)... decoding to RAM, this can take a moment";
                DrawText(loadingMsg, (screenWidth - MeasureText(loadingMsg, 22)) / 2, screenHeight / 2 - 11, 22, RAYWHITE);
                EndDrawing();

                ma_device_stop(&device);

                int loadedCount = 0;
                // Cada motivo de rechazo se cuenta por separado: "no ha pasado
                // nada" es exactamente lo que el usuario ve si esto se calla, y
                // los tres motivos se arreglan de formas distintas.
                int skippedType = 0, skippedFull = 0;
                char lastSkippedName[128] = "";
                char lastError[256] = "";
                for (unsigned int fidx = 0; fidx < dropped.count; fidx++) {
                    const char* path = dropped.paths[fidx];
                    bool isAudio = IsAudioFile(path);
                    if (!isAudio && !IsVisualFile(path)) {
                        skippedType++;
                        snprintf(lastSkippedName, sizeof(lastSkippedName), "%s", GetFileName(path));
                        continue;
                    }
                    int* target = isAudio ? &sampleTarget : &clipTarget;
                    int limit = isAudio ? MAX_SLOTS : NUM_CLIPS;
                    if (*target >= limit) { skippedFull++; continue; }

                    bool ok = isAudio
                                  ? g_engine.getInstrumentBank().LoadAudioOnly(*target, path)
                                  : IsImageFile(path)
                                      ? LoadImageIntoSlot(*target, path, g_transcodeMaxSide)
                                      : g_engine.getInstrumentBank().LoadVideo(*target, path, true);
                    if (ok) {
                        g_slotPath[*target] = path;
                        g_slotTrimStart[*target] = 0;
                        g_slotTrimLen[*target] = 0;
                        auto it = textureCache.find(*target);
                        if (it != textureCache.end()) {
                            UnloadTexture(it->second);
                            textureCache.erase(it);
                        }
                        loadedCount++;
                        (*target)++;
                    } else {
                        snprintf(lastError, sizeof(lastError), "%s", GetFileName(path));
                    }
                }

                ma_device_start(&device);

                // Siempre se dice algo. Da igual lo que haya pasado: soltar
                // archivos y no obtener ni una línea de respuesta es
                // indistinguible de que la función no exista.
                if (loadedCount > 0 && lastError[0] == '\0' && skippedType == 0 && skippedFull == 0) {
                    SetStatus("Loaded %d file(s)", loadedCount);
                } else if (loadedCount > 0 && lastError[0] != '\0') {
                    SetStatus("Loaded %d file(s); %s failed%s", loadedCount, lastError,
                              g_ffmpegState == 0 ? " - ffmpeg is missing" : "");
                } else if (loadedCount > 0) {
                    SetStatus("Loaded %d file(s); skipped %d (unsupported) and %d (slots full)",
                              loadedCount, skippedType, skippedFull);
                } else if (lastError[0] != '\0') {
                    SetStatus(g_ffmpegState == 0
                                  ? "Could not load %s - ffmpeg is missing, see the warning at the bottom"
                                  : "Could not load %s - unsupported or damaged file",
                              lastError);
                } else if (skippedFull > 0) {
                    SetStatus("No free slot left for %d dropped file(s) - free some or change page", skippedFull);
                } else if (skippedType > 0) {
                    SetStatus("Cannot use \"%s\": drop video, image, GIF, .wav/.mp3/.ogg/.flac, .mid or .smt",
                              lastSkippedName);
                } else {
                    SetStatus("Nothing was dropped that Pinguus can read");
                }

                UnloadDroppedFiles(dropped);
            }
        }

        // ---------------- Teclado ----------------
        if (!uiLocked) {
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (ctrl && IsKeyPressed(KEY_Z)) {
                if (shift) doRedo();
                else doUndo();
            }

            if (!ctrl) {
                if (IsKeyPressed(KEY_SPACE)) togglePause();
                if (IsKeyPressed(KEY_UP))   setTempo(bpmDisplay + 5.0f);
                if (IsKeyPressed(KEY_DOWN)) setTempo(bpmDisplay - 5.0f);
                // Number keys pick notes: 1-9 -> C..G#, 0 -> A.
                for (int k = 0; k < 9; k++) {
                    if (IsKeyPressed(KEY_ONE + k)) { selectedTool = k; lastColorIndex = k; }
                }
                if (IsKeyPressed(KEY_ZERO)) { selectedTool = 9; lastColorIndex = 9; }
                // Octave down / up.
                if (IsKeyPressed(KEY_LEFT_BRACKET)  && g_octave > kMinOctave) { g_octave--; SetStatus("Octave %d", g_octave); }
                if (IsKeyPressed(KEY_RIGHT_BRACKET) && g_octave < kMaxOctave) { g_octave++; SetStatus("Octave %d", g_octave); }
                if (IsKeyPressed(KEY_E)) selectedTool = TOOL_ERASE;
                if (IsKeyPressed(KEY_M)) selectedTool = TOOL_MUTE;
                if (IsKeyPressed(KEY_A)) {
                    if (selectedTool == TOOL_ARROW) arrowDir = (arrowDir + 1) % 4;
                    selectedTool = TOOL_ARROW;
                }
                if (IsKeyPressed(KEY_R)) arrowDir = (arrowDir + 1) % 4;
                if (IsKeyPressed(KEY_S)) {
                    if (selectedTool == TOOL_SUSTAIN) sustainIdx = (sustainIdx + 1) % kSustainCount;
                    selectedTool = TOOL_SUSTAIN;
                }
                if (IsKeyPressed(KEY_T)) sustainIdx = (sustainIdx + 1) % kSustainCount;
                if (IsKeyPressed(KEY_F)) {
                    if (selectedTool == TOOL_FX) fxTypeIdx = (fxTypeIdx + 1) % kFxCount;
                    selectedTool = TOOL_FX;
                }
                if (IsKeyPressed(KEY_V)) {
                    if (selectedTool == TOOL_VOL) volIdx = (volIdx + 1) % kVolCount;
                    selectedTool = TOOL_VOL;
                }
                if (IsKeyPressed(KEY_W)) {
                    if (selectedTool == TOOL_TIME) timeIdx = (timeIdx + 1) % kTimeCount;
                    selectedTool = TOOL_TIME;
                }
                if (IsKeyPressed(KEY_G)) {
                    if (selectedTool == TOOL_ARP) arpIdx = (arpIdx + 1) % kArpCount;
                    selectedTool = TOOL_ARP;
                }
                if (IsKeyPressed(KEY_N)) {
                    if (selectedTool == TOOL_MELODY) melIdx = (melIdx + 1) % MELODY_BANK_SIZE;
                    selectedTool = TOOL_MELODY;
                    if (g_melodies[melIdx].empty()) showMelody = true;
                }
                if (IsKeyPressed(KEY_P)) {
                    if (selectedTool == TOOL_TELEPORT) teleId = (teleId + 1) % NUM_TELE_IDS;
                    selectedTool = TOOL_TELEPORT;
                }
                if (IsKeyPressed(KEY_TAB)) {
                    if (activeBar == 1) selectedSampleSlot = SAMPLE_BASE + ((selectedSampleSlot - SAMPLE_BASE + 1) % NUM_SAMPLES);
                    else selectedClipSlot = (selectedClipSlot + 1) % NUM_CLIPS;
                }
                if (IsKeyPressed(KEY_C)) clearPaint();
                if (IsKeyPressed(KEY_B)) clearBugs();
            }
        }

        // ---------------- Mouse ----------------
        int mouseX = GetMouseX();
        int mouseY = GetMouseY();
        bool mouseOverGrid = activeView == 0 &&
                             mouseX >= gridOffX && mouseX < gridOffX + cellSize * g_gridW &&
                             mouseY >= gridOffY && mouseY < gridOffY + cellSize * g_gridH;
        bool mouseOverPalette = mouseY >= paletteY && mouseY < screenHeight && mouseX < leftPanelWidth;
        // The notey list panel sits over the left of the grid; don't paint under it.
        const int kNoteyPanelW = 300;
        if (showNoteyList && activeView == 0 && mouseX < kNoteyPanelW && mouseY >= viewY0 && mouseY < viewY0 + viewH)
            mouseOverGrid = false;
        if (uiLocked) { mouseOverGrid = false; mouseOverPalette = false; }

        const SnapshotBuffer& snapshot = g_snapshotPublisher.read();

        // Clic DERECHO en la celda MELODY: abre su editor. Es donde se genera,
        // se ve, se renombra y se borra cada una de las ocho.
        if (mouseOverPalette && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            int idx = mouseX * PALETTE_CELLS / leftPanelWidth;
            if (idx == TOOL_MELODY) { selectedTool = TOOL_MELODY; showMelody = true; }
        }
        if (mouseOverPalette && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int idx = mouseX * PALETTE_CELLS / leftPanelWidth;
            if (idx >= 0 && idx < PALETTE_CELLS) {
                if (idx < NOTE_COUNT) {
                    selectedTool = idx;
                    lastColorIndex = idx;
                } else if (idx == TOOL_ARROW) {
                    if (selectedTool == TOOL_ARROW) arrowDir = (arrowDir + 1) % 4;
                    selectedTool = TOOL_ARROW;
                } else if (idx == TOOL_SUSTAIN) {
                    if (selectedTool == TOOL_SUSTAIN) sustainIdx = (sustainIdx + 1) % kSustainCount;
                    selectedTool = TOOL_SUSTAIN;
                } else if (idx == TOOL_FX) {
                    if (selectedTool == TOOL_FX) fxTypeIdx = (fxTypeIdx + 1) % kFxCount;
                    selectedTool = TOOL_FX;
                } else if (idx == TOOL_VOL) {
                    if (selectedTool == TOOL_VOL) volIdx = (volIdx + 1) % kVolCount;
                    selectedTool = TOOL_VOL;
                } else if (idx == TOOL_TIME) {
                    if (selectedTool == TOOL_TIME) timeIdx = (timeIdx + 1) % kTimeCount;
                    selectedTool = TOOL_TIME;
                } else if (idx == TOOL_ARP) {
                    if (selectedTool == TOOL_ARP) arpIdx = (arpIdx + 1) % kArpCount;
                    selectedTool = TOOL_ARP;
                } else if (idx == TOOL_MELODY) {
                    // Igual que el resto de la paleta: volver a pincharla cicla
                    // su valor, aquí cuál de las ocho melodías. Y si la que toca
                    // está VACÍA se abre el editor solo — es lo que hace falta
                    // en ese momento y ahorra descubrir dónde estaba.
                    if (selectedTool == TOOL_MELODY) melIdx = (melIdx + 1) % MELODY_BANK_SIZE;
                    selectedTool = TOOL_MELODY;
                    if (g_melodies[melIdx].empty()) showMelody = true;
                } else if (idx == TOOL_TELEPORT) {
                    if (selectedTool == TOOL_TELEPORT) teleId = (teleId + 1) % NUM_TELE_IDS;
                    selectedTool = TOOL_TELEPORT;
                } else {
                    selectedTool = idx;
                }
            }
        }

        int hoverCellX = -1, hoverCellY = -1;
        if (mouseOverGrid) {
            hoverCellX = (mouseX - gridOffX) / cellSize;
            hoverCellY = (mouseY - gridOffY) / cellSize;
            if (hoverCellX >= g_gridW) hoverCellX = g_gridW - 1;
            if (hoverCellY >= g_gridH) hoverCellY = g_gridH - 1;

            if (selectedTool == TOOL_ARP) {
                // Single click stamps the arpeggio to the right: each cell a
                // note of the chord, so a notey walking through plays it.
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    pushUndo();
                    const ArpPat& ap = kArpPatterns[arpIdx];
                    int baseSemi = lastColorIndex + g_octave * 12;
                    for (int k = 0; k < ap.n; k++) {
                        int cx = hoverCellX + k;
                        if (cx >= g_gridW) break;
                        MirrorCell& mc = g_mirror[hoverCellY * g_gridW + cx];
                        mc = MirrorCell();
                        mc.kind = CELL_COLOR;
                        int semi = baseSemi + ap.steps[k];
                        mc.pitchIdx = semi;
                        mc.color = g_palette[((semi % 12) + 12) % 12].displayColor;
                        mc.clip = activeSlot();
                        sendCellToEngine(cx, hoverCellY, mc);
                    }
                    SetStatus("Arpeggio '%s' stamped", ap.name);
                }
            } else if (selectedTool == TOOL_MELODY) {
                // Igual que el arpegio, pero las notas vienen del banco: un
                // clic estampa la melodía entera hacia la derecha, una casilla
                // por semicorchea, y un notey que pase por encima la toca.
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    const MelodyClip& mel = g_melodies[melIdx];
                    if (mel.empty()) {
                        showMelody = true;      // no hay nada que estampar: al editor
                    } else {
                        pushUndo();
                        int placed = 0;
                        for (const auto& n : mel.notes) {
                            int cx = hoverCellX + n.step;
                            if (cx >= g_gridW) break;
                            MirrorCell& mc = g_mirror[hoverCellY * g_gridW + cx];
                            mc = MirrorCell();
                            mc.kind = CELL_COLOR;
                            int semi = n.semitone + g_octave * 12;
                            mc.pitchIdx = semi;
                            mc.color = g_palette[((semi % 12) + 12) % 12].displayColor;
                            mc.clip = activeSlot();
                            mc.vol = 0.35f + 0.65f * n.peak;   // el relieve de lo cantado
                            sendCellToEngine(cx, hoverCellY, mc);
                            placed++;
                        }
                        const int missed = (int)mel.notes.size() - placed;
                        SetStatus(missed > 0
                            ? TextFormat("Melody %d stamped (%d notes; %d ran off the edge)",
                                         melIdx + 1, placed, missed)
                            : TextFormat("Melody %d stamped (%d notes) with %s",
                                         melIdx + 1, placed, slotLabel(activeSlot())));
                    }
                }
            } else if (selectedTool == TOOL_TELEPORT) {
                // Portals are placed by single click (not drag): first click of
                // an id drops entrance A, second drops exit B (A -> B one way).
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    pushUndo();
                    placeTeleport(hoverCellX, hoverCellY, teleId);
                }
            } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pushUndo();
                if (hoverCellX != lastPaintX || hoverCellY != lastPaintY || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    lastPaintX = hoverCellX;
                    lastPaintY = hoverCellY;
                    MirrorCell& mc = g_mirror[hoverCellY * g_gridW + hoverCellX];
                    int erasedTeleId = (mc.kind == CELL_TELEPORT) ? mc.dir : -1;

                    if (selectedTool == TOOL_ERASE) {
                        mc = MirrorCell();
                    } else if (selectedTool == TOOL_ARROW) {
                        mc = MirrorCell();
                        mc.kind = CELL_ARROW;
                        mc.dir = arrowDir;
                    } else if (selectedTool == TOOL_MUTE) {
                        mc = MirrorCell();
                        mc.kind = CELL_MUTE;
                    } else if (selectedTool == TOOL_SUSTAIN) {
                        // HOLD se SUPERPONE, como el FX: sobre una nota la
                        // alarga sin tocarla, y sobre una celda vacía deja una
                        // espera sin nota — o sea, un SILENCIO.
                        mc.hold = kSustainChoices[sustainIdx];
                        if (mc.kind == CELL_SUSTAIN) mc.kind = CELL_COLOR; // formato antiguo
                    } else if (selectedTool == TOOL_FX) {
                        // FX solo se pone ENCIMA de una nota existente; en celdas
                        // vacías (u otros tipos) no hace nada. Conserva clip/tono.
                        if (cellHasNote(mc)) {
                            mc.kind = CELL_FX;
                            mc.fxType = fxTypeIdx;
                        }
                    } else if (selectedTool == TOOL_VOL) {
                        // Volumen y velocidad sólo tienen sentido sobre una nota.
                        if (cellHasNote(mc)) mc.vol = kVolChoices[volIdx];
                    } else if (selectedTool == TOOL_TIME) {
                        if (cellHasNote(mc)) mc.tmul = kTimeChoices[timeIdx];
                    } else if (selectedTool == TOOL_MODCELL) {
                        // Fase 2: coloca la celda personalizada elegida (del mod).
                        if (selectedModCell >= 0 && selectedModCell < (int)g_modCells.size()) {
                            const ModCellDef& d = g_modCells[selectedModCell];
                            mc = MirrorCell();
                            mc.kind = CELL_MODCELL;
                            mc.modCellId = selectedModCell;
                            mc.color = {d.r, d.g, d.b, 255};
                        }
                    } else {
                        const PaletteEntry& e = g_palette[selectedTool];
                        mc.kind = CELL_COLOR;
                        mc.color = e.displayColor;
                        mc.clip = activeSlot();
                        mc.pitchIdx = selectedTool + g_octave * 12; // absolute semitone
                    }
                    sendCellToEngine(hoverCellX, hoverCellY, mc);
                    if (erasedTeleId >= 0) resendTeleGroup(erasedTeleId); // portal was overwritten
                }
            } else {
                lastPaintX = lastPaintY = -1;
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                if (selectedTool == TOOL_ERASE) {
                    deleteBugAt(hoverCellX, hoverCellY, snapshot);
                } else {
                    pushUndo();
                    int dx = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? -1 : 1;
                    spawnBug(hoverCellX, hoverCellY, dx, 0, activeSlot(), kSpeedChoices[selectedSpeedIdx]);
                }
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
                deleteBugAt(hoverCellX, hoverCellY, snapshot);
            }
        }

        // ---------------- Pads por TECLADO NUMÉRICO ----------------
        // A propósito funciona en CUALQUIER vista, no solo en BEATBOX: poder
        // meter el ritmo con la mano derecha mientras se pinta el lienzo con
        // el ratón es media gracia de tener una sampleadora dentro. El numpad
        // no lo usa nada más en todo el programa, así que no pisa a nadie.
        // Golpear sí depende de que no haya un modal delante; SOLTAR no: si se
        // abre un diálogo con un pad pulsado y no se atendiera el soltar, una
        // nota en modo GATE se quedaría sonando para siempre.
        for (int i = 0; i < PAD_PER_BANK; i++) {
            int g = padBank * PAD_PER_BANK + i;
            if (!uiLocked && IsKeyPressed(kPadKeys[i])) {
                padPress(g, PADSRC_KEY, 1.0f);
                if (activeView == 3) padSel = i;
            }
            if (IsKeyReleased(kPadKeys[i])) padLift(g, PADSRC_KEY);
        }
        // El ratón puede soltarse LEJOS del pad que golpeó, así que el soltar
        // se atiende aquí y no sobre el rectángulo del pad.
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            for (int g = 0; g < PAD_TOTAL; g++) padLift(g, PADSRC_MOUSE);

        // ---------------- MIDI de entrada (teclado/controlador) ----------------
        // Polling en el hilo principal: cada Note On toca el slot activo al tono
        // de la tecla (voz en vivo, con su video); Start/Stop (realtime) hacen
        // play/stop; la rueda de modulación (CC1) controla el volumen maestro.
        if (midiHudTimer > 0.0f) midiHudTimer -= dt;
        if (g_midiIn && midiInPort >= 0) {
            std::vector<unsigned char> msg;
            for (;;) {
                msg.clear();
                g_midiIn->getMessage(&msg);
                if (msg.empty()) break;
                // Guarda el último mensaje para el diagnóstico del panel DEVICES.
                midiLastLen = (int)(msg.size() < 3 ? msg.size() : 3);
                for (int b = 0; b < midiLastLen; b++) midiLastBytes[b] = msg[b];
                midiHudTimer = 1.5f;
                unsigned char st = msg[0], hi = st & 0xF0;
                // En BEATBOX las notas 36..51 son los 16 pads del banco visible
                // (36 = C1 es el pad 1 en casi todo lo que trae pads: MPC,
                // Maschine, Launchpad en modo drum), así que un controlador de
                // pads funciona sin configurar nada. Fuera de esa vista, y para
                // las notas de fuera de ese rango, sigue tocando el slot activo.
                bool padNote = activeView == 3 && msg.size() >= 2 &&
                               msg[1] >= PAD_MIDI_BASE && msg[1] < PAD_MIDI_BASE + PAD_PER_BANK;
                if (padNote && (hi == 0x90 || hi == 0x80)) {
                    int i = (int)msg[1] - PAD_MIDI_BASE;
                    int g = padBank * PAD_PER_BANK + i;
                    bool on = (hi == 0x90 && msg.size() >= 3 && msg[2] > 0);
                    if (on) {
                        // La velocidad del golpe entra tal cual en el volumen:
                        // es lo que separa tocar un ritmo de dispararlo.
                        padPress(g, PADSRC_MIDI, msg[2] / 127.0f);
                        padSel = i;
                    } else {
                        padLift(g, PADSRC_MIDI);
                    }
                    midiLastNote = msg[1];
                    midiHudTimer = 1.5f;
                } else if (hi == 0x90 && msg.size() >= 3 && msg[2] > 0) {    // Note On
                    int semi = (int)msg[1] - 60;
                    int fx = (selectedTool == TOOL_FX) ? fxTypeIdx + 1 : 0;
                    RequestTriggerLive(activeSlot(), SemitoneToColorId(semi), fx);
                    midiLastNote = msg[1];
                    midiHudTimer = 1.5f;
                } else if (hi == 0xB0 && msg.size() >= 3 &&
                           (msg[1] == 74 || msg[1] == 71 || msg[1] == 64)) {
                    // CC74 y CC71 son "cutoff" y "resonance" en prácticamente
                    // todos los controladores: se conectan a los dos ejes del
                    // pad XY. CC64 (el pedal de resonancia) enciende el efecto.
                    if (msg[1] == 74) mfxX = msg[2] / 127.0f;
                    else if (msg[1] == 71) mfxY = msg[2] / 127.0f;
                    else mfxOn = msg[2] >= 64;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                } else if (st == 0xFA) {                                     // Start
                    if (paused) togglePause();
                } else if (st == 0xFC) {                                     // Stop
                    if (!paused) togglePause();
                } else if (hi == 0xB0 && msg.size() >= 3 && msg[1] == 1) {   // CC1 mod wheel
                    g_masterVol.store(msg[2] / 127.0f * 1.5f, std::memory_order_relaxed);
                }
            }
        }

        // ---------------- Mando de juego (gamepad) ----------------
        // Escanea los índices 0..3 y usa el primero conectado (el gamepad no
        // siempre es el 0, sobre todo por Bluetooth). Se calcula siempre para
        // que el panel DEVICES pueda mostrar su estado aunque haya un modal.
        gpIndex = -1;
        for (int gi = 0; gi < 4; gi++) if (IsGamepadAvailable(gi)) { gpIndex = gi; break; }

        // Captura de remapeo: corre aunque el panel esté abierto. El próximo
        // botón que pulses queda asignado a la acción elegida.
        if (gpIndex >= 0 && gpRemapAction >= 0) {
            int b = GetGamepadButtonPressed(); // último botón pulsado (global)
            if (b > GAMEPAD_BUTTON_UNKNOWN) {
                gpMap[gpRemapAction] = b;
                gpRemapAction = -1;
                saveControls();
                SetStatus("Button mapped");
            }
        }

        if (gpIndex >= 0 && !uiLocked && gpRemapAction < 0) {
            const int gp = gpIndex;
            auto pressed = [&](GpAction a) { return IsGamepadButtonPressed(gp, gpMap[a]); };
            auto cycleSlot = [&](int delta) {
                if (activeBar == 1) {
                    int idx = (selectedSampleSlot - SAMPLE_BASE + delta + NUM_SAMPLES) % NUM_SAMPLES;
                    selectedSampleSlot = SAMPLE_BASE + idx; samplePage = idx / PAGE_SIZE;
                } else {
                    int idx = (selectedClipSlot + delta + NUM_CLIPS) % NUM_CLIPS;
                    selectedClipSlot = idx; clipPage = idx / PAGE_SIZE;
                }
            };

            // En BEATBOX el mando ES la botonera, así que ni alterna modos ni
            // mueve cursores: cada botón es un pad. Por eso se salta el
            // alternador de modo, que allí no llevaría a ningún sitio.
            if (activeView != 3 && pressed(GA_TOGGLE_MODE)) gpMode = 1 - gpMode;

            if (activeView == 3) {
                // ---- Modo BEATBOX: doce botones, doce pads ----
                for (int i = 0; i < 12; i++) {
                    int g = padBank * PAD_PER_BANK + i;
                    if (IsGamepadButtonPressed(gp, kPadGamepadBtns[i])) {
                        padPress(g, PADSRC_GP, 1.0f);
                        padSel = i;
                    }
                    if (IsGamepadButtonReleased(gp, kPadGamepadBtns[i])) padLift(g, PADSRC_GP);
                }
                // Stick derecho = el pad XY de la sección de FX. Se enciende
                // solo con moverlo (si no hay latch) y se apaga al soltarlo:
                // es el gesto de "meter" el filtro un par de compases.
                float rx = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_RIGHT_X);
                float ry = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_RIGHT_Y);
                bool stickOut = (fabsf(rx) > 0.2f || fabsf(ry) > 0.2f);
                if (stickOut) {
                    mfxX = (rx + 1.0f) * 0.5f;
                    mfxY = (-ry + 1.0f) * 0.5f;
                    if (!mfxLatch) mfxOn = true;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                } else if (mfxOn && !mfxLatch) {
                    mfxOn = false;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_THUMB)) {
                    mfxLatch = !mfxLatch;
                    if (!mfxLatch) mfxOn = false;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_THUMB))
                    padBank = (padBank + 1) % PAD_BANKS;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_RIGHT)) {   // Start
                    padPatPlaying = !padPatPlaying;
                    if (!padPatPlaying) padRecArm = false;
                    RequestPadTransport(padPatPlaying, padRecArm, padQuant, padSteps);
                }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_LEFT)) {    // Select
                    padRecArm = !padRecArm;
                    if (padRecArm) padPatPlaying = true;
                    RequestPadTransport(padPatPlaying, padRecArm, padQuant, padSteps);
                }
            } else if (gpMode == 0) {
                // ---- Modo NOTEYS ----
                if (pressed(GA_CONTROLLABLE)) {
                    gpControllable = !gpControllable;
                    gpPrevCellX = gpPrevCellY = -1;
                    SetStatus(gpControllable ? "Controllable notey: move to trigger cells" : "Controllable notey off");
                }
                if (gpCurX < 0) { gpCurX = g_gridW / 2.0f; gpCurY = g_gridH / 2.0f; }
                // Stick izquierdo + D-pad mueven el cursor.
                float ax = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_LEFT_X);
                float ay = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_LEFT_Y);
                const float dz = 0.25f;
                if (fabsf(ax) > dz) gpCurX += ax * 14.0f * dt;
                if (fabsf(ay) > dz) gpCurY += ay * 14.0f * dt;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) { gpCurX += 1; gpSpawnDir = 0; }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_DOWN))  { gpCurY += 1; gpSpawnDir = 1; }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  { gpCurX -= 1; gpSpawnDir = 2; }
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_UP))    { gpCurY -= 1; gpSpawnDir = 3; }
                gpCurX = gpCurX < 0 ? 0 : (gpCurX > g_gridW - 1 ? (float)(g_gridW - 1) : gpCurX);
                gpCurY = gpCurY < 0 ? 0 : (gpCurY > g_gridH - 1 ? (float)(g_gridH - 1) : gpCurY);
                int cx = (int)gpCurX, cy = (int)gpCurY;

                // Stick DERECHO: X elige slot (clip/sample), Y cambia de barra.
                float rx = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_RIGHT_X);
                float ry = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_RIGHT_Y);
                if (rx > 0.6f && !gpRStickLatchX) { cycleSlot(+1); gpRStickLatchX = true; }
                else if (rx < -0.6f && !gpRStickLatchX) { cycleSlot(-1); gpRStickLatchX = true; }
                else if (fabsf(rx) < 0.3f) gpRStickLatchX = false;
                if (ry < -0.6f && !gpRStickLatchY) { activeBar = 0; gpRStickLatchY = true; }   // arriba = CLIP
                else if (ry > 0.6f && !gpRStickLatchY) { activeBar = 1; gpRStickLatchY = true; } // abajo = SMP
                else if (fabsf(ry) < 0.3f) gpRStickLatchY = false;

                // LB/RB: cambian la NOTA a colocar (tono).
                if (pressed(GA_NOTE_PREV)) { lastColorIndex = (lastColorIndex + NOTE_COUNT - 1) % NOTE_COUNT; selectedTool = lastColorIndex; }
                if (pressed(GA_NOTE_NEXT)) { lastColorIndex = (lastColorIndex + 1) % NOTE_COUNT; selectedTool = lastColorIndex; }

                // A = poner nota | B = poner notey | X = borrar | Y = play/stop.
                if (pressed(GA_PLACE_NOTE)) {
                    pushUndo();
                    MirrorCell& mc = g_mirror[cy * g_gridW + cx];
                    mc = MirrorCell();
                    mc.kind = CELL_COLOR;
                    mc.color = g_palette[lastColorIndex].displayColor;
                    mc.clip = activeSlot();
                    mc.pitchIdx = lastColorIndex + g_octave * 12;
                    sendCellToEngine(cx, cy, mc);
                }
                if (pressed(GA_PLACE_NOTEY))
                    spawnBug(cx, cy, kDirDx[gpSpawnDir], kDirDy[gpSpawnDir], activeSlot(), kSpeedChoices[selectedSpeedIdx]);
                if (pressed(GA_ERASE)) {
                    int erasedTeleId = (g_mirror[cy * g_gridW + cx].kind == CELL_TELEPORT) ? g_mirror[cy * g_gridW + cx].dir : -1;
                    g_mirror[cy * g_gridW + cx] = MirrorCell();
                    RequestPaintCell(cx, cy, 0, 0, ModifierType::None, 0, 0, 0);
                    if (erasedTeleId >= 0) resendTeleGroup(erasedTeleId);
                    deleteBugAt(cx, cy, snapshot);
                }
                if (pressed(GA_PLAYSTOP)) togglePause();

                // Notey Controlable: al ENTRAR en una celda nueva, dispara lo que
                // haya pintado ahí (como un notey que la pisa).
                if (gpControllable && (cx != gpPrevCellX || cy != gpPrevCellY)) {
                    const MirrorCell& mc = g_mirror[cy * g_gridW + cx];
                    if (mc.kind == CELL_COLOR || mc.kind == CELL_SUSTAIN || mc.kind == CELL_FX) {
                        int fx = (mc.kind == CELL_FX) ? mc.fxType + 1 : 0; // 1..4 estilo tracker
                        // Voz dedicada: su video RE-DISPARA en vez de multiplicarse.
                        RequestTriggerControllable(mc.clip, SemitoneToColorId(mc.pitchIdx), fx);
                    }
                    gpPrevCellX = cx; gpPrevCellY = cy;
                }
            } else {
                // ---- Modo UI: transporte, tempo, vista, octava ----
                if (pressed(GA_PLAYSTOP) || pressed(GA_PLACE_NOTE)) togglePause();
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_UP))    setTempo(bpmDisplay + 5.0f);
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_DOWN))  setTempo(bpmDisplay - 5.0f);
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  activeView = (activeView + 3) % 4;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) activeView = (activeView + 1) % 4;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_TRIGGER_2) && g_octave > kMinOctave) g_octave--;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_TRIGGER_2) && g_octave < kMaxOctave) g_octave++;
            }
        }

        // Mods en Lua: corren cada frame (reaccionan/manejan el mundo). El
        // snapshot ya está leído, así que sus lecturas de noteys/playhead ven
        // el estado actual. No corren durante el diálogo inicial.
        if (!startupOpen) g_scripts.update(dt);

        // Drena la cola de eventos de video: los de MODELO 3D (id >= MODEL_SLOT_BASE)
        // van al pool de modelos; el resto, al pool de video 2D.
        {
            VideoEvent ev;
            while (g_videoEventQueue.pop(ev)) {
                if (isModelSlot(ev.sampleId)) {
                    int model = modelOfId(ev.sampleId), anim = animOfId(ev.sampleId);
                    if (ev.type == VideoEventType::VoiceStarted && model >= 0 && model < MAX_MODELS && g_models[model].loaded) {
                        if (anim < 0 || anim >= modelAnimTotal(g_models[model])) anim = 0;
                        ModelVoice* slot = nullptr;
                        for (ModelVoice& v : g_modelVoices) if (v.active && v.id == ev.bichoIndex) { slot = &v; break; }
                        if (!slot) for (ModelVoice& v : g_modelVoices) if (!v.active) { slot = &v; break; }
                        if (slot) { slot->active = true; slot->id = ev.bichoIndex; slot->model = model; slot->anim = anim; slot->frame = (float)animTrimStartF(g_models[model], anim); }
                    } else if (ev.type == VideoEventType::VoiceStopped) {
                        for (ModelVoice& v : g_modelVoices) if (v.active && v.id == ev.bichoIndex) v.active = false;
                    }
                } else {
                    g_videoVoicePool.handleEvent(ev, g_engine.getInstrumentBank());
                }
            }
        }
        if (!paused) g_videoVoicePool.advanceAll(g_engine.getInstrumentBank(), dt);
        // Avanza las voces de modelo: reproduce la animación UNA vez (~60 fps) y
        // al terminar se apaga (efecto "trigger"). Un re-disparo la reinicia.
        if (!paused) {
            for (ModelVoice& v : g_modelVoices) {
                if (!v.active) continue;
                ModelSlot& s = g_models[v.model];
                if (!s.loaded || v.anim < 0 || v.anim >= modelAnimTotal(s)) { v.active = false; continue; }
                int st = animTrimStartF(s, v.anim), en = animTrimEndF(s, v.anim);
                if (v.frame < (float)st) v.frame = (float)st;
                v.frame += 60.0f * dt;
                if (v.frame >= (float)en) {
                    if (s.animLoop[v.anim] && en > st) { // loopea el rango recortado
                        v.frame = (float)st + fmodf(v.frame - (float)st, (float)(en - st));
                    } else v.active = false; // reprodujo el rango una vez
                }
            }
        }

        // ============================ DIBUJO ============================
        BeginDrawing();
        ClearBackground(g_theme.bg);

        int previewSlot = -1; // slot hovered in a CLIP/SMP bar, for the popup

        // --- Pestañas ---
        {
            DrawRectangle(0, 0, leftPanelWidth, tabsH, g_theme.panel);
            Vector2 m = GetMousePosition();
            const char* tabLabels[4] = {"CANVAS", "TRACKER", "LINEAR", "BEATBOX"};
            for (int t = 0; t < 4; t++) {
                Rectangle r = {(float)(8 + t * 62), 2, 58, 20};
                bool act = activeView == t;
                bool hover = CheckCollisionPointRec(m, r);
                DrawRectangleRec(r, act ? (Color){64, 68, 96, 255} : (hover ? (Color){50, 52, 70, 255} : (Color){34, 36, 50, 255}));
                DrawRectangleLinesEx(r, act ? 2.0f : 1.0f, act ? RAYWHITE : (Color){80, 82, 105, 255});
                const char* lbl = tabLabels[t];
                DrawText(lbl, (int)(r.x + (r.width - MeasureText(lbl, 12)) / 2), (int)r.y + 4, 12,
                         act ? RAYWHITE : (Color){150, 150, 170, 255});
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) activeView = t;
            }
            // NOTEYS list toggle (mute/enable individual noteys).
            Rectangle nb = {262, 2, 78, 20};
            bool nhover = CheckCollisionPointRec(m, nb);
            DrawRectangleRec(nb, showNoteyList ? (Color){64, 68, 96, 255} : (nhover ? (Color){50, 52, 70, 255} : (Color){34, 36, 50, 255}));
            DrawRectangleLinesEx(nb, showNoteyList ? 2.0f : 1.0f, showNoteyList ? RAYWHITE : (Color){80, 82, 105, 255});
            DrawText("NOTEYS", (int)(nb.x + (nb.width - MeasureText("NOTEYS", 12)) / 2), (int)nb.y + 4, 12,
                     showNoteyList ? RAYWHITE : (Color){150, 150, 170, 255});
            if (!uiLocked && nhover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) showNoteyList = !showNoteyList;

            // HUD compacto de mando / MIDI (donde antes iba "SPACE stops all").
            {
                int hx = 344;
                if (gpIndex >= 0) {
                    const char* gm = gpMode != 0 ? "UI" : (gpControllable ? "NOTEY*" : "NOTEYS");
                    DrawText(TextFormat("GP:%s", gm), hx, 7, 10, gpControllable ? (Color){255, 190, 90, 255} : (Color){150, 210, 150, 255});
                    hx += 62;
                }
                bool mact = midiInPort >= 0 && midiHudTimer > 0.0f && midiLastNote >= 0;
                DrawText(midiInPort >= 0 ? (mact ? TextFormat("MIDI %s", SemitoneName(midiLastNote - 60)) : "MIDI") : "MIDI:off",
                         hx, 7, 10, mact ? (Color){235, 200, 120, 255} : (Color){110, 110, 135, 255});
            }

            // Master volume slider.
            DrawText("MASTER", 470, 7, 11, (Color){160, 160, 180, 255});
            Rectangle vs = {524, 4, 120, 16};
            float mv = g_masterVol.load();
            DrawRectangleRec(vs, (Color){36, 38, 52, 255});
            DrawRectangle((int)vs.x, (int)vs.y, (int)(vs.width * (mv / 1.5f)), (int)vs.height, (Color){70, 150, 90, 255});
            DrawRectangleLinesEx(vs, 1, (Color){90, 92, 118, 255});
            DrawText(TextFormat("%.0f%%", mv * 100.0f), (int)vs.x + 46, 6, 11, (Color){225, 240, 225, 255});
            if (!uiLocked && CheckCollisionPointRec(m, vs) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                float t = (m.x - vs.x) / vs.width;
                t = t < 0 ? 0 : (t > 1 ? 1 : t);
                g_masterVol.store(t * 1.5f, std::memory_order_relaxed);
            }

            // Botones DEVICES (mando/MIDI) y MODS (scripts Lua).
            {
                Rectangle db = {652, 3, 40, 18};
                bool dh = CheckCollisionPointRec(m, db);
                DrawRectangleRec(db, showDevices ? (Color){64, 68, 96, 255} : (dh ? (Color){50, 52, 70, 255} : (Color){34, 36, 50, 255}));
                DrawRectangleLinesEx(db, 1, (Color){90, 92, 118, 255});
                DrawText("DEV", (int)db.x + 7, 7, 11, RAYWHITE);
                if (dh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) showDevices = !showDevices;

                Rectangle mb = {696, 3, 48, 18};
                bool mh = CheckCollisionPointRec(m, mb);
                DrawRectangleRec(mb, showMods ? (Color){64, 68, 96, 255} : (mh ? (Color){50, 52, 70, 255} : (Color){34, 36, 50, 255}));
                DrawRectangleLinesEx(mb, 1, (Color){90, 92, 118, 255});
                DrawText("MODS", (int)mb.x + 6, 7, 11, g_scripts.list().empty() ? RAYWHITE : (Color){150, 230, 160, 255});
                if (mh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) showMods = !showMods;

                // VHS: el efecto de vídeo analógico sobre la mezcla final. Se
                // pone en verde cuando está actuando, porque es un efecto que
                // cambia TODO lo que se graba y olvidárselo encendido se paga.
                Rectangle vb = {748, 3, 44, 18};
                bool vh = CheckCollisionPointRec(m, vb);
                DrawRectangleRec(vb, showNtsc ? (Color){64, 68, 96, 255} : (vh ? (Color){50, 52, 70, 255} : (Color){34, 36, 50, 255}));
                DrawRectangleLinesEx(vb, g_ntsc.enabled ? 2.0f : 1.0f,
                                     g_ntsc.enabled ? (Color){235, 120, 90, 255} : (Color){90, 92, 118, 255});
                DrawText("VHS", (int)vb.x + 11, 7, 11, g_ntsc.enabled ? (Color){255, 170, 120, 255} : RAYWHITE);
                if (vh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) showNtsc = !showNtsc;

            }
        }

        if (activeView == 0) {
            // ===================== VISTA CANVAS =====================
            for (int y = 0; y < g_gridH; y++) {
                for (int x = 0; x < g_gridW; x++) {
                    const MirrorCell& mc = g_mirror[y * g_gridW + x];
                    if (cellIsBlank(mc)) continue;
                    int px = gridOffX + x * cellSize, py = gridOffY + y * cellSize;

                    if (mc.kind == CELL_COLOR || mc.kind == CELL_SUSTAIN || mc.kind == CELL_FX) {
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, mc.color);
                        const char* lbl = slotLabel(mc.clip);
                        DrawText(lbl, px + cellSize - MeasureText(lbl, 10) - 3, py + cellSize - 11, 10, (Color){0, 0, 0, 170});
                        if (mc.kind == CELL_SUSTAIN) {
                            DrawRectangle(px + 4, py + cellSize / 2 - 2, cellSize - 8, 4, (Color){255, 255, 255, 220});
                        } else if (mc.kind == CELL_FX) {
                            DrawText(kFxShort[mc.fxType], px + 3, py + 2, 10, (Color){255, 255, 255, 230});
                        }
                    } else if (mc.kind == CELL_EMPTY) {
                        // Sólo puede llegar aquí una celda con atributos y sin
                        // tipo: un SILENCIO (espera sin nota). Se dibuja hueca
                        // para que se distinga de una nota de un vistazo.
                        DrawRectangleLinesEx({(float)px + 2, (float)py + 2, (float)cellSize - 4, (float)cellSize - 4},
                                             1, (Color){120, 170, 210, 200});
                        DrawRectangle(px + 5, py + cellSize / 2 - 2, cellSize - 10, 4, (Color){120, 170, 210, 220});
                    } else if (mc.kind == CELL_ARROW) {
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, (Color){52, 54, 74, 255});
                        DrawArrowGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.32f, mc.dir, RAYWHITE);
                    } else if (mc.kind == CELL_MUTE) {
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, (Color){60, 40, 44, 255});
                        DrawMuteGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.28f, (Color){235, 140, 140, 255});
                    } else if (mc.kind == CELL_TELEPORT) {
                        Color tc = kTeleColors[mc.dir % 8];
                        // Entrance A = filled disc; exit B = hollow ring.
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, (Color){30, 30, 44, 255});
                        if (mc.fxType == 0) DrawCircle(px + cellSize / 2, py + cellSize / 2, cellSize * 0.3f, tc);
                        DrawTeleGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.32f, tc);
                        const char* lbl = TextFormat("%s%d", mc.fxType == 0 ? "A" : "B", mc.dir + 1);
                        DrawText(lbl, px + cellSize / 2 - MeasureText(lbl, 10) / 2, py + cellSize / 2 - 5, 10,
                                 mc.fxType == 0 ? BLACK : tc);
                    } else if (mc.kind == CELL_MODCELL) {
                        // Celda personalizada de un mod: color propio + glifo.
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, (Color){28, 28, 40, 255});
                        DrawRectangleLinesEx({(float)px + 1, (float)py + 1, (float)cellSize - 2, (float)cellSize - 2}, 2, mc.color);
                        if (mc.modCellId >= 0 && mc.modCellId < (int)g_modCells.size()) {
                            const char* g = g_modCells[mc.modCellId].glyph.c_str();
                            DrawText(g, px + cellSize / 2 - MeasureText(g, 12) / 2, py + cellSize / 2 - 6, 12, mc.color);
                        }
                    }

                    // Distintivos de los atributos superpuestos. Son GLIFOS, no
                    // texto: una celda de 21px ya lleva el código del FX arriba
                    // a la izquierda y la etiqueta del slot abajo a la derecha,
                    // y dos números más ahí dentro se pisan entre sí.
                    if (mc.hold > 0.0f && mc.kind != CELL_EMPTY) {
                        // Barra horizontal centrada: "aquí el notey espera".
                        DrawRectangle(px + 4, py + cellSize / 2 - 2, cellSize - 8, 4, (Color){120, 200, 255, 230});
                    }
                    if (mc.vol < 1.0f) {
                        // Columna en el borde izquierdo con la altura del nivel.
                        int h = (int)((cellSize - 6) * mc.vol);
                        if (h < 2) h = 2;
                        DrawRectangle(px + 2, py + cellSize - 3 - h, 3, h, (Color){150, 235, 170, 240});
                    }
                    if (mc.tmul != 1.0f) {
                        // Triángulo abajo a la izquierda: apunta a la izquierda
                        // si va lenta, a la derecha si va rápida. DrawPoly con
                        // rotación 0 deja un vértice en +X, o sea a la derecha.
                        DrawPoly({px + 9.0f, py + cellSize - 7.0f}, 3, 5.0f,
                                 mc.tmul < 1.0f ? 180.0f : 0.0f, (Color){255, 190, 110, 245});
                    }
                }
            }

            for (int x = 0; x <= g_gridW; x++) {
                DrawLine(gridOffX + x * cellSize, gridOffY, gridOffX + x * cellSize, gridOffY + g_gridH * cellSize, (Color){38, 38, 52, 255});
            }
            for (int y = 0; y <= g_gridH; y++) {
                DrawLine(gridOffX, gridOffY + y * cellSize, gridOffX + g_gridW * cellSize, gridOffY + y * cellSize, (Color){38, 38, 52, 255});
            }

            if (hoverCellX >= 0) {
                int px = gridOffX + hoverCellX * cellSize, py = gridOffY + hoverCellY * cellSize;
                Color outline = RAYWHITE;
                if (selectedTool == TOOL_ERASE) {
                    outline = (Color){230, 90, 90, 255};
                } else if (selectedTool == TOOL_ARROW) {
                    DrawArrowGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.32f, arrowDir, (Color){200, 200, 220, 120});
                } else if (selectedTool == TOOL_MUTE) {
                    DrawMuteGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.28f, (Color){235, 140, 140, 140});
                } else if (selectedTool == TOOL_TELEPORT) {
                    Color tc = kTeleColors[teleId % 8];
                    tc.a = 150;
                    DrawTeleGlyph(px + cellSize / 2.0f, py + cellSize / 2.0f, cellSize * 0.32f, tc);
                    DrawText(TextFormat("%d", teleId + 1), px + cellSize / 2 - 3, py + cellSize / 2 - 5, 10, tc);
                } else if (selectedTool == TOOL_ARP) {
                    // Ghost of the arpeggio run to the right.
                    const ArpPat& ap = kArpPatterns[arpIdx];
                    for (int k = 0; k < ap.n; k++) {
                        int cx = hoverCellX + k;
                        if (cx >= g_gridW) break;
                        int semi = lastColorIndex + g_octave * 12 + ap.steps[k];
                        Color gc = g_palette[((semi % 12) + 12) % 12].displayColor;
                        gc.a = 120;
                        DrawRectangle(gridOffX + cx * cellSize + 1, py + 1, cellSize - 2, cellSize - 2, gc);
                    }
                    outline = (Color){255, 210, 100, 255};
                } else if (selectedTool == TOOL_MELODY) {
                    // Fantasma de la melodía entera, cada nota a su altura
                    // relativa: se ve DÓNDE va a caer y si se sale del borde
                    // antes de soltar el clic.
                    const MelodyClip& mel = g_melodies[melIdx];
                    for (const auto& n : mel.notes) {
                        int cx = hoverCellX + n.step;
                        if (cx >= g_gridW) break;
                        int semi = n.semitone + g_octave * 12;
                        Color gc = g_palette[((semi % 12) + 12) % 12].displayColor;
                        gc.a = 120;
                        DrawRectangle(gridOffX + cx * cellSize + 1, py + 1, cellSize - 2, cellSize - 2, gc);
                    }
                    if (mel.empty()) {
                        DrawText("empty", px + 2, py + 2, 10, (Color){255, 120, 120, 220});
                        outline = (Color){255, 120, 120, 255};
                    } else {
                        outline = (Color){150, 220, 255, 255};
                    }
                } else if (selectedTool == TOOL_FX || selectedTool == TOOL_VOL || selectedTool == TOOL_TIME) {
                    // Los tres se ponen ENCIMA de una nota existente. El ghost
                    // no pinta color: solo muestra lo que se va a añadir, y
                    // avisa en rojo si la celda está vacía (no haría nada).
                    const MirrorCell& hc = g_mirror[hoverCellY * g_gridW + hoverCellX];
                    bool hasNote = cellHasNote(hc);
                    const char* tag = selectedTool == TOOL_FX  ? kFxShort[fxTypeIdx]
                                    : selectedTool == TOOL_VOL ? TextFormat("%d", (int)(kVolChoices[volIdx] * 100.0f + 0.5f))
                                                               : kTimeLabels[timeIdx];
                    DrawText(tag, px + 3, py + 2, 10,
                             hasNote ? (Color){255, 255, 255, 220} : (Color){255, 120, 120, 200});
                    outline = hasNote ? (Color){255, 210, 100, 255} : (Color){150, 90, 90, 255};
                } else if (selectedTool == TOOL_SUSTAIN) {
                    // HOLD vale en cualquier celda: sobre una nota la alarga y
                    // sobre una vacía deja un silencio. Nunca es "no hará nada",
                    // así que no hay aviso rojo.
                    DrawRectangle(px + 4, py + cellSize / 2 - 2, cellSize - 8, 4, (Color){120, 200, 255, 170});
                    DrawText(kSustainLabels[sustainIdx], px + 3, py + 2, 10, (Color){200, 230, 255, 220});
                    outline = (Color){120, 200, 255, 255};
                } else if (selectedTool == TOOL_MODCELL) {
                    if (selectedModCell >= 0 && selectedModCell < (int)g_modCells.size()) {
                        const ModCellDef& d = g_modCells[selectedModCell];
                        Color gc = {d.r, d.g, d.b, 120};
                        DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, gc);
                        DrawText(d.glyph.c_str(), px + cellSize / 2 - MeasureText(d.glyph.c_str(), 12) / 2, py + cellSize / 2 - 6, 12, RAYWHITE);
                        outline = (Color){d.r, d.g, d.b, 255};
                    }
                } else if (selectedTool >= 0 && selectedTool < NOTE_COUNT) {
                    Color ghost = g_palette[selectedTool].displayColor;
                    ghost.a = 110;
                    DrawRectangle(px + 1, py + 1, cellSize - 2, cellSize - 2, ghost);
                }
                DrawRectangleLinesEx({(float)px, (float)py, (float)cellSize, (float)cellSize}, 2, outline);
            }

            for (int i = 0; i < snapshot.count; i++) {
                const BichoVisual& v = snapshot.bichos[i];
                if (!v.isActive) continue;
                int cx = gridOffX + v.x * cellSize + cellSize / 2;
                int cy = gridOffY + v.y * cellSize + cellSize / 2;
                if (v.stopped) {
                    // Stopped notey: frozen in place — a blue square (pause look).
                    float s = cellSize * 0.28f;
                    DrawRectangle((int)(cx - s), (int)(cy - s), (int)(s * 2), (int)(s * 2), (Color){70, 110, 150, 255});
                    DrawRectangleLines((int)(cx - s), (int)(cy - s), (int)(s * 2), (int)(s * 2), (Color){150, 200, 235, 255});
                } else if (v.muted) {
                    // Muted notey: hollow gray ring with a slash — it moves but stays silent.
                    float radius = cellSize * 0.34f;
                    DrawCircleLines(cx, cy, radius, (Color){120, 120, 135, 255});
                    DrawLineEx({cx - radius * 0.7f, cy + radius * 0.7f}, {cx + radius * 0.7f, cy - radius * 0.7f}, 2, (Color){120, 120, 135, 255});
                } else {
                    float radius = v.isPlaying ? cellSize * 0.42f : cellSize * 0.32f;
                    DrawCircle(cx, cy, radius, v.isPlaying ? (Color){255, 190, 60, 255} : (Color){150, 150, 165, 255});
                    DrawCircleLines(cx, cy, radius, v.isPlaying ? RAYWHITE : (Color){90, 90, 105, 255});
                }
            }

            // Cursor del gamepad (modo NOTEYS). En "Notey Controlable" se dibuja
            // como un notey (círculo) que dispara al pasar; si no, como celda
            // resaltada con la flecha de dirección de disparo.
            if (gpIndex >= 0 && gpMode == 0 && gpCurX >= 0) {
                int gx = gridOffX + (int)gpCurX * cellSize, gy = gridOffY + (int)gpCurY * cellSize;
                if (gpControllable) {
                    float rcx = gx + cellSize / 2.0f, rcy = gy + cellSize / 2.0f;
                    DrawCircle((int)rcx, (int)rcy, cellSize * 0.40f, (Color){255, 170, 60, 235});
                    DrawCircleLines((int)rcx, (int)rcy, cellSize * 0.40f, RAYWHITE);
                    DrawRectangleLinesEx({(float)gx, (float)gy, (float)cellSize, (float)cellSize}, 2, (Color){255, 200, 90, 200});
                } else {
                    DrawRectangleLinesEx({(float)gx, (float)gy, (float)cellSize, (float)cellSize}, 3, (Color){120, 230, 140, 255});
                    DrawArrowGlyph(gx + cellSize / 2.0f, gy + cellSize / 2.0f, cellSize * 0.28f, gpSpawnDir, (Color){120, 230, 140, 200});
                }
            }
        } else if (activeView == 1) {
            // ===================== VISTA TRACKER =====================
            const int gutter = 34;
            const int chanW = (leftPanelWidth - gutter) / TRACKER_CHANNELS;
            const int headY = viewY0 + 4;
            const int rowsY0 = viewY0 + 20;
            const int rowH = 15;
            Vector2 m = GetMousePosition();

            for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
                DrawText(TextFormat("CH%d", ch + 1), gutter + ch * chanW + 6, headY, 12, (Color){160, 160, 180, 255});
            }
            if (UIButton({(float)(leftPanelWidth - 60), (float)headY - 2, 52, 16}, "Clear", 10) && !uiLocked) {
                trackerClearAll();
            }
            if (UIButton({(float)(leftPanelWidth - 120), (float)headY - 2, 52, 16}, "MIDI", 10) && !uiLocked) {
                char path[512] = "";
                int r = NativeOpenDialog("Import MIDI file...", "*.mid *.midi", path, sizeof(path));
                if (r == 1) importMidi(path);
            }

            for (int r = 0; r < TRACKER_ROWS; r++) {
                int ry = rowsY0 + r * rowH;
                Color rowBg = (r % 4 == 0) ? (Color){28, 29, 42, 255} : (Color){22, 23, 33, 255};
                if (r == snapshot.trackerRow) rowBg = (Color){70, 66, 40, 255};
                DrawRectangle(0, ry, leftPanelWidth, rowH - 1, rowBg);
                DrawText(TextFormat("%02d", r), 6, ry + 2, 10, (Color){120, 120, 145, 255});

                for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
                    Rectangle cellRec = {(float)(gutter + ch * chanW), (float)ry, (float)(chanW - 2), (float)(rowH - 1)};
                    const TrkCell& c = g_tracker[ch][r];
                    bool hover = !uiLocked && CheckCollisionPointRec(m, cellRec);

                    if (c.sample >= 0) {
                        Color noteBg = g_palette[((c.pitchIdx % 12) + 12) % 12].displayColor;
                        noteBg.a = 120;
                        DrawRectangleRec(cellRec, noteBg);
                        const char* fxTxt = c.fx > 0 ? kFxShort[c.fx - 1] : "";
                        DrawText(TextFormat("%s %s %s", slotLabel(c.sample), SemitoneName(c.pitchIdx), fxTxt),
                                 (int)cellRec.x + 5, ry + 2, 10, RAYWHITE);
                    }
                    if (hover) {
                        DrawRectangleLinesEx(cellRec, 1, RAYWHITE);
                        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            int fx = (selectedTool == TOOL_FX) ? fxTypeIdx + 1 : 0;
                            trackerPlace(ch, r, selectedSampleSlot, lastColorIndex + g_octave * 12, fx);
                        }
                        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                            trackerPlace(ch, r, -1, 3, 0);
                        }
                    }
                }
            }
            DrawText("L-click: note (SMP+color; FX tool adds effect)  R-click: erase  MIDI: import .mid",
                     6, rowsY0 + TRACKER_ROWS * rowH + 2, 11, (Color){130, 130, 155, 255});
        } else if (activeView == 2) {
            // ===================== VISTA LINEAR (estilo canvas, lineal) =====================
            // Como el canvas pero se reproduce en línea: columnas de tiempo x
            // FILAS/carriles (que puedes añadir o quitar). Cada casilla guarda
            // su clip/sample + tono (del color elegido) igual que el canvas;
            // un playhead vertical barre de izquierda a derecha disparándolas.
            Vector2 m = GetMousePosition();
            const int transportH = 22;
            const int gutter = 30;                 // regleta de números de carril
            const int rollX0 = gutter;
            const int rollY0 = viewY0 + transportH;
            const int rollW = leftPanelWidth - gutter;
            const int rollH = viewH - transportH - 16;
            // Only the loop range is shown, stretched to fill the width — so
            // cells stay wide enough to read the clip/sample label on them.
            const int ncols = g_linearLength < 1 ? 1 : g_linearLength;
            const float colW = (float)rollW / ncols;
            const int nrows = g_linearRows;
            const float rowH = (float)rollH / nrows;
            const int rollBottom = rollY0 + (int)(nrows * rowH);

            // --- Barra de transporte ---
            int tx = 6;
            if (UIButton({(float)tx, (float)viewY0 + 2, 46, 18}, "Clear", 10) && !uiLocked) {
                linearClearAll();
            }
            tx += 50;
            // LOOP toggle.
            {
                Rectangle lb = {(float)tx, (float)viewY0 + 2, 44, 18};
                bool hover = !uiLocked && CheckCollisionPointRec(m, lb);
                DrawRectangleRec(lb, g_linearLoop ? (Color){64, 96, 68, 255} : (hover ? (Color){50, 52, 70, 255} : (Color){40, 42, 56, 255}));
                DrawRectangleLinesEx(lb, 1, (Color){110, 112, 140, 255});
                DrawText("LOOP", (int)(lb.x + (lb.width - MeasureText("LOOP", 10)) / 2), (int)lb.y + 5, 10, RAYWHITE);
                if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    g_linearLoop = !g_linearLoop;
                    RequestLinearParams(g_linearLength, g_linearLoop);
                }
            }
            tx += 50;
            // LENGTH -/+ (columnas del loop).
            DrawText("LEN", tx, viewY0 + 6, 10, (Color){160, 160, 180, 255});
            tx += 24;
            if (UIButton({(float)tx, (float)viewY0 + 2, 16, 18}, "-", 12) && !uiLocked) {
                g_linearLength = g_linearLength > 1 ? g_linearLength - 1 : 1;
                RequestLinearParams(g_linearLength, g_linearLoop);
            }
            DrawText(TextFormat("%d", g_linearLength), tx + 20, viewY0 + 6, 11, RAYWHITE);
            if (UIButton({(float)tx + 38, (float)viewY0 + 2, 16, 18}, "+", 12) && !uiLocked) {
                g_linearLength = g_linearLength < LINEAR_COLS ? g_linearLength + 1 : LINEAR_COLS;
                RequestLinearParams(g_linearLength, g_linearLoop);
            }
            tx += 58;
            // ROWS -/+ (carriles).
            DrawText("ROWS", tx, viewY0 + 6, 10, (Color){160, 160, 180, 255});
            tx += 34;
            if (UIButton({(float)tx, (float)viewY0 + 2, 16, 18}, "-", 12) && !uiLocked) {
                linearSetRows(g_linearRows - 1);
            }
            DrawText(TextFormat("%d", g_linearRows), tx + 20, viewY0 + 6, 11, RAYWHITE);
            if (UIButton({(float)tx + 38, (float)viewY0 + 2, 16, 18}, "+", 12) && !uiLocked) {
                linearSetRows(g_linearRows + 1);
            }
            tx += 58;
            DrawText(TextFormat("CLIP/SMP: %s  %s  %s", slotLabel(activeSlot()),
                                SemitoneName((selectedTool < NOTE_COUNT ? selectedTool : lastColorIndex) + g_octave * 12),
                                paused ? "stopped" : "playing"),
                     tx, viewY0 + 6, 10, (Color){130, 130, 155, 255});

            // --- Fondo de carriles + regleta de números ---
            for (int row = 0; row < nrows; row++) {
                int ry = rollY0 + (int)(row * rowH);
                Color rowBg = (row % 2) ? (Color){22, 23, 33, 255} : (Color){26, 27, 39, 255};
                DrawRectangle(0, ry, leftPanelWidth, (int)rowH + 1, rowBg);
                DrawText(TextFormat("%d", row + 1), 6, ry + (int)(rowH / 2) - 5, 11, (Color){120, 120, 145, 255});
            }

            // --- Rejilla (líneas de columna cada 4 = compás) ---
            for (int col = 0; col <= ncols; col++) {
                int cx = rollX0 + (int)(col * colW);
                Color lc = (col % 4 == 0) ? (Color){52, 54, 72, 255} : (Color){36, 37, 50, 255};
                DrawLine(cx, rollY0, cx, rollBottom, lc);
            }
            for (int row = 0; row <= nrows; row++) {
                int ry = rollY0 + (int)(row * rowH);
                DrawLine(rollX0, ry, rollX0 + (int)(ncols * colW), ry, (Color){34, 35, 48, 255});
            }

            // --- Notas colocadas (muestran su clip/sample como el canvas) ---
            for (int col = 0; col < ncols; col++) {
                for (int row = 0; row < nrows; row++) {
                    const LinCell& c = g_linear[col][row];
                    if (c.sample < 0) continue;
                    int px = rollX0 + (int)(col * colW);
                    int py = rollY0 + (int)(row * rowH);
                    Color nc = g_palette[((c.pitchIdx % 12) + 12) % 12].displayColor;
                    DrawRectangle(px + 1, py + 1, (int)colW - 1, (int)rowH - 1, nc);
                    // Etiqueta del slot + nota (como en el canvas), si cabe.
                    if (colW >= 22) {
                        DrawText(TextFormat("%s %s", slotLabel(c.sample), SemitoneName(c.pitchIdx)),
                                 px + 3, py + 2, 10, (Color){0, 0, 0, 200});
                    } else if (colW >= 13) {
                        DrawText(slotLabel(c.sample), px + 2, py + 2, 10, (Color){0, 0, 0, 200});
                    }
                    if (c.fx > 0 && rowH >= 22) {
                        DrawText(kFxShort[c.fx - 1], px + 3, py + (int)rowH - 12, 9, (Color){0, 0, 0, 220});
                    }
                }
            }

            // --- Playhead ---
            if (snapshot.linearCol >= 0 && snapshot.linearCol < ncols) {
                int px = rollX0 + (int)((snapshot.linearCol + 0.5f) * colW);
                DrawLine(px, rollY0, px, rollBottom, (Color){255, 230, 120, 220});
            }

            // --- Hover + entrada del ratón ---
            bool overRoll = !uiLocked && m.x >= rollX0 && m.x < rollX0 + (int)(ncols * colW) &&
                            m.y >= rollY0 && m.y < rollBottom;
            if (overRoll) {
                int hc = (int)((m.x - rollX0) / colW);
                int hr = (int)((m.y - rollY0) / rowH);
                if (hc >= 0 && hc < ncols && hr >= 0 && hr < nrows) {
                    Rectangle cellRec = {(float)(rollX0 + hc * colW), (float)(rollY0 + hr * rowH), colW, rowH};
                    bool hasNote = g_linear[hc][hr].sample >= 0;
                    // La herramienta FX solo resalta si hay nota debajo.
                    Color hl = (selectedTool == TOOL_FX && !hasNote) ? (Color){120, 120, 130, 255} : RAYWHITE;
                    DrawRectangleLinesEx(cellRec, 1, hl);
                    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        if (selectedTool == TOOL_FX) {
                            linearApplyFx(hc, hr, fxTypeIdx + 1); // solo sobre notas existentes
                        } else if (selectedTool == TOOL_ERASE) {
                            linearPlace(hc, hr, -1, 0, 0);
                        } else {
                            int pitch = (selectedTool < NOTE_COUNT ? selectedTool : lastColorIndex) + g_octave * 12;
                            linearPlace(hc, hr, activeSlot(), pitch, 0);
                        }
                    }
                    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                        linearPlace(hc, hr, -1, 0, 0);
                    }
                }
            }
            DrawText("L-click: place selected CLIP/SMP at the chosen note color | FX tool: drop effect onto an existing note | R-click: erase",
                     6, rollBottom + 2, 10, (Color){130, 130, 155, 255});

        } else if (activeView == 3) {
            // ===================== VISTA BEATBOX (sampleadora) =====================
            //
            // Cuatro zonas: los 16 pads (arriba a la izquierda), el inspector del
            // pad elegido (en medio), la sección de FX en vivo (a la derecha) y
            // el patrón con su transporte (abajo, a todo lo ancho).
            //
            // El disparo de los pads NO se hace aquí: vive en el bloque de
            // entrada, más arriba, para que el teclado numérico y el mando
            // funcionen también mirando el lienzo. Aquí solo se dibuja y se
            // atiende el ratón.
            Vector2 m = GetMousePosition();
            const int gpad = padBank * PAD_PER_BANK;

            // ---------------- Cabecera: bancos y parada de emergencia ----------------
            {
                DrawText("BANK", 8, viewY0 + 8, 11, (Color){160, 160, 180, 255});
                for (int b = 0; b < PAD_BANKS; b++) {
                    Rectangle r = {(float)(46 + b * 34), (float)viewY0 + 4, 30, 18};
                    bool act = padBank == b;
                    DrawRectangleRec(r, act ? (Color){80, 110, 150, 255} : (Color){40, 42, 58, 255});
                    DrawRectangleLinesEx(r, act ? 2.0f : 1.0f, act ? RAYWHITE : (Color){90, 92, 118, 255});
                    DrawText(TextFormat("%c", 'A' + b), (int)r.x + 11, (int)r.y + 3, 12,
                             act ? RAYWHITE : (Color){150, 150, 170, 255});
                    if (!uiLocked && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) padBank = b;
                }
                if (UIButton({188, (float)viewY0 + 4, 74, 18}, "STOP ALL", 10) && !uiLocked) {
                    RequestPadStopAll();
                    for (int i = 0; i < PAD_TOTAL; i++) padHeld[i] = 0;
                }
                DrawText("Numpad = pads | R-click a pad to assign the selected CLIP/SMP",
                         272, viewY0 + 8, 10, (Color){130, 130, 155, 255});
            }

            // ---------------- Los 16 pads ----------------
            const int padX0 = 8, padY0 = viewY0 + 30;
            const int padSize = 92, padGap = 6;
            for (int i = 0; i < PAD_PER_BANK; i++) {
                int col = i % PAD_COLS, row = i / PAD_COLS;
                Rectangle r = {(float)(padX0 + col * (padSize + padGap)),
                               (float)(padY0 + row * (padSize + padGap)),
                               (float)padSize, (float)padSize};
                int g = gpad + i;
                const PadConfig& pc = g_pads[g];
                bool playing = (snapshot.padPlaying >> g) & 1ULL;
                bool looping = (snapshot.padLooping >> g) & 1ULL;
                bool hover = !uiLocked && CheckCollisionPointRec(m, r);

                // Fondo: vacío en gris; con contenido, azulado si es CLIP y
                // verdoso si es SAMPLE; encendido mientras suena.
                Color bg;
                if (pc.empty())        bg = (Color){32, 34, 46, 255};
                else if (playing)      bg = (Color){235, 200, 110, 255};
                else if (pc.slot >= SAMPLE_BASE) bg = (Color){48, 78, 62, 255};
                else                   bg = (Color){48, 60, 92, 255};
                if (hover && !playing) bg = (Color){(unsigned char)(bg.r + 18), (unsigned char)(bg.g + 18),
                                                    (unsigned char)(bg.b + 18), 255};
                DrawRectangleRec(r, bg);

                // Miniatura del vídeo, si el slot lleva imagen: un pad con la
                // cara del clip se reconoce mucho antes que uno con un número.
                if (!pc.empty() && clipState(pc.slot) == 2) {
                    const InstrumentSource& src = g_engine.getInstrumentBank().at(pc.slot);
                    if (!src.videoFrames.empty()) {
                        Texture2D& tex = getOrCreateTexture(pc.slot, src.videoWidth, src.videoHeight, src.videoChannels);
                        uploadSlotFrame(tex, pc.slot, 0, src);
                        float s = (float)(padSize - 8) / tex.width;
                        if (tex.height * s > padSize - 24) s = (float)(padSize - 24) / tex.height;
                        float dw = tex.width * s, dh = tex.height * s;
                        DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height},
                                       {r.x + (padSize - dw) / 2, r.y + 12, dw, dh}, {0, 0}, 0.0f,
                                       playing ? WHITE : (Color){255, 255, 255, 170});
                    }
                }

                bool sel = (padSel == i);
                Color border = looping ? (Color){255, 150, 60, 255}
                                       : (sel ? RAYWHITE : (Color){90, 92, 118, 255});
                DrawRectangleLinesEx(r, (looping || sel) ? 3.0f : 1.0f, border);

                Color fg = playing ? (Color){30, 25, 10, 255} : RAYWHITE;
                DrawText(kPadKeyNames[i], (int)r.x + 5, (int)r.y + 3, 12, fg);
                if (pc.empty()) {
                    DrawText("- empty -", (int)r.x + 5, (int)r.y + padSize / 2 - 4, 10, (Color){110, 110, 135, 255});
                } else {
                    DrawText(slotLabel(pc.slot), (int)r.x + 5, (int)r.y + padSize - 26, 13, fg);
                    DrawText(kPadModeShort[pc.mode], (int)r.x + 5, (int)r.y + padSize - 12, 9,
                             playing ? (Color){60, 50, 20, 255} : (Color){170, 172, 200, 255});
                    if (pc.pitch != 0)
                        DrawText(SemitoneName(pc.pitch), (int)r.x + padSize - 26, (int)r.y + 3, 10, fg);
                    if (pc.choke != 0)
                        DrawText(TextFormat("C%d", pc.choke), (int)r.x + padSize - 22, (int)r.y + padSize - 12, 9,
                                 (Color){235, 140, 140, 255});
                    if (pc.fx != 0)
                        DrawText(kFxShort[pc.fx - 1], (int)r.x + padSize - 22, (int)r.y + padSize - 24, 9,
                                 (Color){150, 200, 255, 255});
                }

                if (hover) {
                    // Izquierdo: seleccionar y GOLPEAR (se mantiene mientras el
                    // botón siga abajo, para que el modo GATE funcione con ratón).
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        padSel = i;
                        padPress(g, PADSRC_MOUSE, 1.0f);
                    }
                    // Derecho: asignar el CLIP/SMP que esté elegido en las barras
                    // de abajo. Es el camino corto para montar un kit: eliges el
                    // sonido una vez y vas repartiéndolo por los pads.
                    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                        int s = activeSlot();
                        if (isModelSlot(s)) {
                            SetStatus("A 3D model has no audio - pick a CLIP or a SMP slot");
                        } else {
                            g_pads[g].slot = s;
                            RequestSetPadConfig(g);
                            padSel = i;
                            SetStatus("Pad %d%c = %s", i + 1, 'A' + padBank, slotLabel(s));
                        }
                    }
                }
            }

            // ---------------- Inspector del pad elegido ----------------
            {
                int gsel = gpad + padSel;
                PadConfig& pc = g_pads[gsel];
                const int ix = padX0 + PAD_COLS * (padSize + padGap) + 6;
                const int iw = 186;
                int iy = padY0;
                DrawRectangle(ix, iy, iw, PAD_ROWS * (padSize + padGap) - padGap, (Color){26, 27, 40, 255});
                DrawRectangleLines(ix, iy, iw, PAD_ROWS * (padSize + padGap) - padGap, (Color){70, 72, 96, 255});
                iy += 6;
                DrawText(TextFormat("PAD %d%c   [%s]", padSel + 1, 'A' + padBank, kPadKeyNames[padSel]),
                         ix + 8, iy, 13, RAYWHITE);
                iy += 20;
                DrawText(pc.empty() ? "no sound assigned" : TextFormat("slot %s", slotLabel(pc.slot)),
                         ix + 8, iy, 11, pc.empty() ? (Color){130, 130, 155, 255} : (Color){235, 200, 120, 255});
                iy += 18;

                if (UIButton({(float)ix + 8, (float)iy, 100, 20}, "Assign selected", 10) && !uiLocked) {
                    int s = activeSlot();
                    if (isModelSlot(s)) SetStatus("A 3D model has no audio - pick a CLIP or a SMP slot");
                    else { pc.slot = s; RequestSetPadConfig(gsel); }
                }
                if (UIButton({(float)ix + 114, (float)iy, 64, 20}, "Clear", 10) && !uiLocked) {
                    pc = PadConfig();
                    RequestSetPadConfig(gsel);
                }
                iy += 26;
                if (UIButton({(float)ix + 8, (float)iy, 100, 20}, "Edit sample", 10) && !uiLocked) {
                    if (pc.empty()) SetStatus("Assign a CLIP or SMP to this pad first");
                    else openEditor(pc.slot);
                }
                if (UIButton({(float)ix + 114, (float)iy, 64, 20}, "Play", 10) && !uiLocked) {
                    if (!pc.empty()) RequestPadTrigger(gsel, 1.0f);
                }
                iy += 30;

                // Tono
                DrawText("PITCH", ix + 8, iy + 4, 10, (Color){160, 160, 180, 255});
                if (UIButton({(float)ix + 52, (float)iy, 20, 20}, "-", 12) && !uiLocked) {
                    if (pc.pitch > -24) { pc.pitch--; RequestSetPadConfig(gsel); }
                }
                DrawText(SemitoneName(pc.pitch), ix + 78, iy + 5, 12, RAYWHITE);
                if (UIButton({(float)ix + 122, (float)iy, 20, 20}, "+", 12) && !uiLocked) {
                    if (pc.pitch < 24) { pc.pitch++; RequestSetPadConfig(gsel); }
                }
                if (UIButton({(float)ix + 148, (float)iy, 30, 20}, "0", 10) && !uiLocked) {
                    pc.pitch = 0; RequestSetPadConfig(gsel);
                }
                iy += 26;

                // Volumen (deslizador)
                DrawText("VOL", ix + 8, iy + 4, 10, (Color){160, 160, 180, 255});
                Rectangle vs = {(float)ix + 42, (float)iy + 2, 100, 16};
                DrawRectangleRec(vs, (Color){36, 38, 52, 255});
                DrawRectangle((int)vs.x, (int)vs.y, (int)(vs.width * pc.vol), (int)vs.height, (Color){70, 150, 90, 255});
                DrawRectangleLinesEx(vs, 1, (Color){90, 92, 118, 255});
                DrawText(TextFormat("%d%%", (int)(pc.vol * 100.0f + 0.5f)), ix + 148, iy + 5, 11, RAYWHITE);
                if (!uiLocked && CheckCollisionPointRec(m, vs) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float t = (m.x - vs.x) / vs.width;
                    pc.vol = t < 0 ? 0 : (t > 1 ? 1 : t);
                    RequestSetPadConfig(gsel);
                }
                iy += 26;

                // Modo de disparo
                DrawText("MODE", ix + 8, iy + 4, 10, (Color){160, 160, 180, 255});
                if (UIButton({(float)ix + 52, (float)iy, 126, 20}, kPadModeNames[pc.mode], 11) && !uiLocked) {
                    pc.mode = (unsigned char)((pc.mode + 1) % 3);
                    RequestSetPadConfig(gsel);
                }
                iy += 26;

                // Grupo de corte
                DrawText("CHOKE", ix + 8, iy + 4, 10, (Color){160, 160, 180, 255});
                if (UIButton({(float)ix + 52, (float)iy, 126, 20},
                             pc.choke == 0 ? "none" : TextFormat("group %d", pc.choke), 11) && !uiLocked) {
                    pc.choke = (unsigned char)((pc.choke + 1) % 5);
                    RequestSetPadConfig(gsel);
                }
                iy += 26;

                // Efecto fijo del pad
                DrawText("FX", ix + 8, iy + 4, 10, (Color){160, 160, 180, 255});
                if (UIButton({(float)ix + 52, (float)iy, 126, 20},
                             pc.fx == 0 ? "dry" : kFxNames[pc.fx - 1], 11) && !uiLocked) {
                    pc.fx = (unsigned char)((pc.fx + 1) % 5);
                    RequestSetPadConfig(gsel);
                }
                iy += 28;

                DrawText("ONE-SHOT: plays to the end", ix + 8, iy, 9, (Color){120, 120, 145, 255});
                DrawText("GATE: only while held down", ix + 8, iy + 12, 9, (Color){120, 120, 145, 255});
                DrawText("LOOP: press again to stop", ix + 8, iy + 24, 9, (Color){120, 120, 145, 255});
                DrawText("CHOKE: same group cuts itself", ix + 8, iy + 36, 9, (Color){120, 120, 145, 255});
            }

            // ---------------- Sección de FX en vivo (pad XY) ----------------
            {
                const int fx0 = padX0 + PAD_COLS * (padSize + padGap) + 198;
                const int fw = leftPanelWidth - fx0 - 8;
                int fy = padY0;
                DrawRectangle(fx0, fy, fw, PAD_ROWS * (padSize + padGap) - padGap, (Color){26, 27, 40, 255});
                DrawRectangleLines(fx0, fy, fw, PAD_ROWS * (padSize + padGap) - padGap, (Color){70, 72, 96, 255});

                DrawText("LIVE FX", fx0 + 8, fy + 6, 13, RAYWHITE);
                // ON es momentáneo salvo que LATCH esté puesto: así se puede
                // "meter" el filtro solo en el compás que interesa.
                Rectangle ob = {(float)(fx0 + fw - 108), (float)fy + 4, 44, 18};
                DrawRectangleRec(ob, mfxOn ? (Color){200, 90, 70, 255} : (Color){40, 42, 58, 255});
                DrawRectangleLinesEx(ob, 1, (Color){90, 92, 118, 255});
                DrawText("ON", (int)ob.x + 14, (int)ob.y + 4, 11, RAYWHITE);
                Rectangle lb = {(float)(fx0 + fw - 60), (float)fy + 4, 52, 18};
                DrawRectangleRec(lb, mfxLatch ? (Color){80, 110, 150, 255} : (Color){40, 42, 58, 255});
                DrawRectangleLinesEx(lb, 1, (Color){90, 92, 118, 255});
                DrawText("LATCH", (int)lb.x + 8, (int)lb.y + 4, 10, RAYWHITE);
                if (!uiLocked && CheckCollisionPointRec(m, lb) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    mfxLatch = !mfxLatch;
                    if (!mfxLatch) mfxOn = false;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                }
                if (!uiLocked && CheckCollisionPointRec(m, ob) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    mfxOn = !mfxOn;
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                }

                // El pad XY: el eje X es el primer mando y el Y el segundo, con
                // el 0 abajo (arrastrar hacia arriba SUBE el valor, que es lo
                // que espera cualquiera que haya tocado un mando físico).
                Rectangle xy = {(float)fx0 + 8, (float)fy + 28, (float)fw - 16, (float)fw - 16};
                DrawRectangleRec(xy, (Color){18, 19, 28, 255});
                for (int gl = 1; gl < 4; gl++) {
                    DrawLine((int)(xy.x + xy.width * gl / 4), (int)xy.y,
                             (int)(xy.x + xy.width * gl / 4), (int)(xy.y + xy.height), (Color){40, 42, 58, 255});
                    DrawLine((int)xy.x, (int)(xy.y + xy.height * gl / 4),
                             (int)(xy.x + xy.width), (int)(xy.y + xy.height * gl / 4), (Color){40, 42, 58, 255});
                }
                DrawRectangleLinesEx(xy, mfxOn ? 2.0f : 1.0f, mfxOn ? (Color){235, 120, 90, 255} : (Color){90, 92, 118, 255});
                float knobX = xy.x + mfxX * xy.width;
                float knobY = xy.y + (1.0f - mfxY) * xy.height;
                DrawLine((int)knobX, (int)xy.y, (int)knobX, (int)(xy.y + xy.height), (Color){70, 90, 120, 255});
                DrawLine((int)xy.x, (int)knobY, (int)(xy.x + xy.width), (int)knobY, (Color){70, 90, 120, 255});
                DrawCircle((int)knobX, (int)knobY, 9, mfxOn ? (Color){255, 170, 90, 255} : (Color){130, 140, 170, 255});
                DrawCircleLines((int)knobX, (int)knobY, 9, RAYWHITE);

                if (!uiLocked && CheckCollisionPointRec(m, xy) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    padXYDragging = true;
                    if (!mfxLatch) mfxOn = true;   // momentáneo: suena mientras se arrastra
                }
                if (padXYDragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float tx = (m.x - xy.x) / xy.width;
                    float ty = 1.0f - (m.y - xy.y) / xy.height;
                    mfxX = tx < 0 ? 0 : (tx > 1 ? 1 : tx);
                    mfxY = ty < 0 ? 0 : (ty > 1 ? 1 : ty);
                    RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                }
                if (padXYDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    padXYDragging = false;
                    if (!mfxLatch) { mfxOn = false; RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn); }
                }

                int ly = (int)(xy.y + xy.height + 4);
                DrawText(TextFormat("X %s  %.0f%%", kMasterFxAxisX[mfxType], mfxX * 100.0f),
                         fx0 + 8, ly, 10, (Color){160, 160, 180, 255});
                DrawText(TextFormat("Y %s  %.0f%%", kMasterFxAxisY[mfxType], mfxY * 100.0f),
                         fx0 + 8, ly + 12, 10, (Color){160, 160, 180, 255});
                ly += 28;

                // Lista de efectos
                for (int t = 0; t < MFX_COUNT; t++) {
                    Rectangle r = {(float)fx0 + 8, (float)(ly + t * 20), (float)fw - 16, 18};
                    bool act = mfxType == t;
                    DrawRectangleRec(r, act ? (Color){80, 110, 150, 255} : (Color){36, 38, 52, 255});
                    DrawRectangleLinesEx(r, act ? 2.0f : 1.0f, act ? RAYWHITE : (Color){70, 72, 96, 255});
                    DrawText(kMasterFxNames[t], (int)r.x + 8, (int)r.y + 3, 11,
                             act ? RAYWHITE : (Color){150, 150, 170, 255});
                    if (!uiLocked && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        mfxType = t;
                        RequestSetMasterFx(mfxType, mfxX, mfxY, mfxOn);
                    }
                }
            }

            // ---------------- Patrón + transporte ----------------
            {
                const int patY = padY0 + PAD_ROWS * (padSize + padGap) + 2;
                int gsel = gpad + padSel;
                DrawText(TextFormat("PATTERN  -  editing pad %d%c  (%d steps)", padSel + 1, 'A' + padBank, padSteps),
                         8, patY, 11, (Color){180, 180, 205, 255});

                const float laneX = 8.0f, laneW = (float)leftPanelWidth - 16.0f;
                const float laneY = (float)patY + 14, laneH = 34.0f;
                float colW = laneW / padSteps;
                DrawRectangle((int)laneX, (int)laneY, (int)laneW, (int)laneH, (Color){22, 23, 34, 255});

                for (int s = 0; s < padSteps; s++) {
                    Rectangle c = {laneX + s * colW, laneY, colW - 1, laneH};
                    // Un paso de 1/16 abarca PAD_PAT_RES ticks: se pinta si
                    // CUALQUIERA de ellos lleva un golpe de este pad (al grabar
                    // sin cuantizar el golpe cae entre líneas, y aun así se ve).
                    bool onSel = false, onAny = false;
                    for (int k = 0; k < PAD_PAT_RES; k++) {
                        unsigned long long mk = snapshot.padPattern[s * PAD_PAT_RES + k];
                        if (mk & (1ULL << gsel)) onSel = true;
                        if (mk != 0ULL) onAny = true;
                    }
                    Color bg = (s % 4 == 0) ? (Color){38, 40, 56, 255} : (Color){28, 30, 42, 255};
                    if (onSel) bg = (Color){235, 200, 110, 255};
                    else if (onAny) bg = (Color){60, 70, 95, 255};   // hay algo de otro pad
                    DrawRectangleRec(c, bg);
                    DrawRectangleLinesEx(c, 1, (Color){50, 52, 70, 255});

                    if (!uiLocked && CheckCollisionPointRec(m, c)) {
                        DrawRectangleLinesEx(c, 2, RAYWHITE);
                        // Escribir a mano cae siempre en la línea del paso.
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                            RequestSetPadStep(s * PAD_PAT_RES, gsel, !onSel);
                        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                            for (int k = 0; k < PAD_PAT_RES; k++)
                                RequestSetPadStep(s * PAD_PAT_RES + k, gsel, false);
                        }
                    }
                }
                // Playhead
                if (snapshot.padTick >= 0) {
                    float px = laneX + (snapshot.padTick / (float)PAD_PAT_RES) * colW;
                    DrawLine((int)px, (int)laneY, (int)px, (int)(laneY + laneH), (Color){255, 90, 90, 255});
                }
                DrawRectangleLines((int)laneX, (int)laneY, (int)laneW, (int)laneH, (Color){90, 92, 118, 255});

                // --- Botonera del transporte del patrón ---
                int tx = 8;
                const int ty = (int)(laneY + laneH + 4);
                auto padTransportChanged = [&]() {
                    RequestPadTransport(padPatPlaying, padRecArm, padQuant, padSteps);
                };

                {
                    Rectangle r = {(float)tx, (float)ty, 58, 20};
                    DrawRectangleRec(r, padPatPlaying ? (Color){70, 150, 90, 255} : (Color){40, 42, 58, 255});
                    DrawRectangleLinesEx(r, 1, (Color){90, 92, 118, 255});
                    DrawText(padPatPlaying ? "STOP" : "PLAY", (int)r.x + 12, (int)r.y + 4, 12, RAYWHITE);
                    if (!uiLocked && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        padPatPlaying = !padPatPlaying;
                        if (!padPatPlaying) padRecArm = false;
                        padTransportChanged();
                    }
                    tx += 62;
                }
                {
                    Rectangle r = {(float)tx, (float)ty, 58, 20};
                    bool blink = padRecArm && fmodf((float)GetTime(), 1.0f) < 0.5f;
                    DrawRectangleRec(r, padRecArm ? (blink ? (Color){235, 70, 70, 255} : (Color){140, 45, 45, 255})
                                                  : (Color){40, 42, 58, 255});
                    DrawRectangleLinesEx(r, 1, (Color){90, 92, 118, 255});
                    DrawText("REC", (int)r.x + 16, (int)r.y + 4, 12, RAYWHITE);
                    if (!uiLocked && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        padRecArm = !padRecArm;
                        if (padRecArm) padPatPlaying = true;   // grabar arranca el bucle
                        padTransportChanged();
                    }
                    tx += 62;
                }
                if (UIButton({(float)tx, (float)ty, 56, 20}, "CLEAR", 10) && !uiLocked) {
                    RequestClearPadPattern(-1);
                    SetStatus("Pattern cleared");
                }
                tx += 60;
                if (UIButton({(float)tx, (float)ty, 66, 20}, "CLR PAD", 10) && !uiLocked) {
                    RequestClearPadPattern(gsel);
                    SetStatus("Pad %d%c erased from the pattern", padSel + 1, 'A' + padBank);
                }
                tx += 74;

                DrawText("LEN", tx, ty + 5, 10, (Color){160, 160, 180, 255});
                tx += 26;
                if (UIButton({(float)tx, (float)ty, 18, 20}, "-", 12) && !uiLocked) {
                    padSteps -= 4; if (padSteps < 4) padSteps = 4;
                    padTransportChanged();
                }
                DrawText(TextFormat("%d", padSteps), tx + 24, ty + 5, 11, RAYWHITE);
                if (UIButton({(float)tx + 44, (float)ty, 18, 20}, "+", 12) && !uiLocked) {
                    padSteps += 4; if (padSteps > PAD_PAT_MAX_STEPS) padSteps = PAD_PAT_MAX_STEPS;
                    padTransportChanged();
                }
                tx += 70;

                DrawText("QUANT", tx, ty + 5, 10, (Color){160, 160, 180, 255});
                tx += 40;
                if (UIButton({(float)tx, (float)ty, 50, 20}, kPadQuantNames[padQuant], 10) && !uiLocked) {
                    padQuant = (padQuant + 1) % 5;
                    padTransportChanged();
                }
                tx += 58;

                // Grabar la SALIDA (vídeo+audio o solo audio): los mismos
                // botones del resto del programa, aquí a mano porque tocar y
                // grabar a la vez es justo lo que se hace con una sampleadora.
                {
                    Rectangle r = {(float)tx, (float)ty, 74, 20};
                    DrawRectangleRec(r, recording ? (Color){200, 60, 60, 255} : (Color){40, 42, 58, 255});
                    DrawRectangleLinesEx(r, 1, (Color){90, 92, 118, 255});
                    DrawText(recording ? "STOP REC" : "REC OUT", (int)r.x + 8, (int)r.y + 4, 11, RAYWHITE);
                    if (!uiLocked && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        if (recording) stopRecording();
                        else recChoiceOpen = true;   // pregunta vídeo+audio o solo audio
                    }
                    tx += 78;
                }

                DrawText("L-click a step to toggle it | R-click clears that step | REC records what you play, quantized as chosen",
                         8, ty + 24, 10, (Color){130, 130, 155, 255});
            }
        }

        // --- Notey list panel (left overlay, canvas view) ---
        if (showNoteyList && activeView == 0 && !uiLocked) {
            std::vector<BugSpawn>& bugs = g_scenes[g_curScene].bugs;
            Rectangle panel = {0, (float)viewY0, (float)kNoteyPanelW, (float)viewH};
            DrawRectangleRec(panel, (Color){22, 23, 34, 245});
            DrawRectangleLinesEx(panel, 1, (Color){70, 72, 96, 255});
            DrawText(TextFormat("NOTEYS (%d)", (int)bugs.size()), 8, viewY0 + 6, 14, RAYWHITE);

            Vector2 m = GetMousePosition();
            // Mute all / enable all.
            if (UIButton({8, (float)viewY0 + 26, 96, 20}, "Mute all", 11)) {
                for (int i = 0; i < (int)bugs.size(); i++) {
                    bugs[i].muted = true;
                    if (i < snapshot.count && snapshot.bichos[i].isActive)
                        RequestSetBichoMuted(snapshot.bichos[i].poolIndex, true);
                }
            }
            if (UIButton({108, (float)viewY0 + 26, 104, 20}, "Enable all", 11)) {
                for (int i = 0; i < (int)bugs.size(); i++) {
                    bugs[i].muted = false;
                    if (i < snapshot.count && snapshot.bichos[i].isActive)
                        RequestSetBichoMuted(snapshot.bichos[i].poolIndex, false);
                }
            }

            const int rowH = 22;
            const int listTop = viewY0 + 52;
            int visibleRows = (viewH - 60) / rowH;
            int nBugs = (int)bugs.size();
            if (noteyScroll > nBugs - visibleRows) noteyScroll = nBugs - visibleRows;
            if (noteyScroll < 0) noteyScroll = 0;
            // Scroll with the wheel while hovering the panel.
            if (CheckCollisionPointRec(m, panel)) noteyScroll -= (int)GetMouseWheelMove();
            if (noteyScroll < 0) noteyScroll = 0;
            if (noteyScroll > (nBugs > visibleRows ? nBugs - visibleRows : 0)) noteyScroll = (nBugs > visibleRows ? nBugs - visibleRows : 0);

            for (int r = 0; r < visibleRows; r++) {
                int i = noteyScroll + r;
                if (i >= nBugs) break;
                const BugSpawn& b = bugs[i];
                float ry = (float)(listTop + r * rowH);
                Rectangle row = {4, ry, (float)kNoteyPanelW - 8, (float)rowH - 2};
                DrawRectangleRec(row, (Color){30, 31, 44, 255});
                int liveIdx = (i < snapshot.count && snapshot.bichos[i].isActive) ? snapshot.bichos[i].poolIndex : -1;

                // Info: number, slot, position.
                DrawText(TextFormat("#%d %s(%d,%d)", i + 1, slotLabel(b.clip), b.x, b.y),
                         8, (int)ry + 4, 11, (b.muted || b.stopped) ? (Color){130, 130, 145, 255} : RAYWHITE);

                // Volume slider (0..1.5).
                Rectangle vsl = {110, ry + 4, 56, (float)rowH - 9};
                DrawRectangleRec(vsl, (Color){22, 23, 34, 255});
                DrawRectangle((int)vsl.x, (int)vsl.y, (int)(vsl.width * (b.volume / 1.5f)), (int)vsl.height, (Color){70, 130, 170, 255});
                DrawRectangleLinesEx(vsl, 1, (Color){70, 72, 96, 255});
                if (CheckCollisionPointRec(m, vsl) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float t = (m.x - vsl.x) / vsl.width;
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    bugs[i].volume = t * 1.5f;
                    if (liveIdx >= 0) RequestSetBichoVolume(liveIdx, bugs[i].volume);
                }

                // Play / Stop toggle (freezes this notey in place).
                Rectangle pl = {172.0f, ry + 1, 58, (float)rowH - 4};
                DrawRectangleRec(pl, b.stopped ? (Color){80, 66, 46, 255} : (Color){46, 66, 80, 255});
                DrawRectangleLinesEx(pl, 1, b.stopped ? (Color){210, 170, 110, 255} : (Color){110, 170, 210, 255});
                const char* pll = b.stopped ? "PLAY" : "STOP";
                Color plc = b.stopped ? (Color){235, 200, 140, 255} : (Color){150, 200, 235, 255};
                DrawText(pll, (int)(pl.x + (pl.width - MeasureText(pll, 11)) / 2), (int)ry + 4, 11, plc);
                if (CheckCollisionPointRec(m, pl) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    bugs[i].stopped = !bugs[i].stopped;
                    if (liveIdx >= 0) RequestSetBichoStopped(liveIdx, bugs[i].stopped);
                }

                // Mute toggle (only its own rectangle).
                Rectangle tg = {(float)kNoteyPanelW - 52, ry + 1, 48, (float)rowH - 4};
                DrawRectangleRec(tg, b.muted ? (Color){80, 50, 54, 255} : (Color){50, 80, 56, 255});
                DrawRectangleLinesEx(tg, 1, b.muted ? (Color){200, 110, 110, 255} : (Color){120, 200, 130, 255});
                const char* tgl = b.muted ? "MUTE" : "ON";
                Color tgc = b.muted ? (Color){230, 150, 150, 255} : (Color){150, 230, 160, 255};
                DrawText(tgl, (int)(tg.x + (tg.width - MeasureText(tgl, 11)) / 2), (int)ry + 4, 11, tgc);
                if (CheckCollisionPointRec(m, tg) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    bugs[i].muted = !bugs[i].muted;
                    if (liveIdx >= 0) RequestSetBichoMuted(liveIdx, bugs[i].muted);
                }
            }
            if (nBugs == 0) {
                DrawText("No noteys yet.", 10, listTop + 4, 12, (Color){140, 140, 160, 255});
                DrawText("Right-click the grid", 10, listTop + 22, 11, (Color){110, 110, 130, 255});
                DrawText("to add one.", 10, listTop + 36, 11, (Color){110, 110, 130, 255});
            }
        }

        // --- Toolbar fila 1 ---
        DrawRectangle(0, row1Y, leftPanelWidth, paletteY - row1Y, g_theme.panel);
        {
            float by = (float)row1Y + 4.0f;
            float bh = 24.0f;

            if (UIButton({10, by, 30, bh}, "-5") && !uiLocked)  setTempo(bpmDisplay - 5.0f);
            DrawText(TextFormat("BPM %.0f", bpmDisplay), 46, (int)by + 4, 17, g_theme.accent);
            if (UIButton({128, by, 30, bh}, "+5") && !uiLocked) setTempo(bpmDisplay + 5.0f);

            if (UIButton({170, by, 90, bh}, "Clear paint") && !uiLocked) clearPaint();
            if (UIButton({266, by, 90, bh}, "Clear noteys", 12) && !uiLocked) clearBugs();

            // PLAY parpadea suave cuando está en pausa (sin cartel encima
            // del lienzo: puedes seguir trabajando mientras tanto).
            Rectangle playR = {362, by, 56, bh};
            if (paused) {
                float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 5.0f);
                Color bg = {(unsigned char)(40 + 30 * pulse), (unsigned char)(90 + 60 * pulse), (unsigned char)(50 + 30 * pulse), 255};
                DrawRectangleRec(playR, bg);
                DrawRectangleLinesEx(playR, 2, (Color){120, 230, 140, (unsigned char)(140 + 115 * pulse)});
                DrawText("PLAY", (int)playR.x + 11, (int)by + 5, 14, RAYWHITE);
                if (!uiLocked && CheckCollisionPointRec(GetMousePosition(), playR) &&
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) togglePause();
            } else {
                if (UIButton(playR, "STOP") && !uiLocked) togglePause();
            }
            DrawText("[SPC]", 424, (int)by + 7, 11, (Color){130, 130, 150, 255});

            if (UIButton({470, by, 60, bh}, "Save") && !uiLocked) {
                char path[512] = "project.smt";
                int r = NativeSaveDialog("Save project as...", "project.smt", path, sizeof(path));
                if (r == 0) {
                    SetStatus("Save cancelled");
                } else {
                    if (r == 1) EnsureExtension(path, sizeof(path), ".smt");
                    saveProject(path);
                }
            }
            if (UIButton({536, by, 60, bh}, "Load") && !uiLocked) {
                char path[512] = "project.smt";
                int r = NativeOpenDialog("Load project...", "*.smt", path, sizeof(path));
                if (r == 0) {
                    SetStatus("Load cancelled");
                } else {
                    loadProject(path);
                }
            }

            if (UIButton({602, by, 84, bh}, TextFormat("RES %d", g_transcodeMaxSide)) && !uiLocked) {
                g_transcodeMaxSide = g_transcodeMaxSide == 480 ? 360 : (g_transcodeMaxSide == 360 ? 240 : 480);
                SetStatus("New videos will load at max %d px (smaller = less RAM)", g_transcodeMaxSide);
            }

            if (recording) {
                Rectangle rr = {692, by, 98, bh};
                DrawRectangleRec(rr, (Color){120, 40, 44, 255});
                DrawRectangleLinesEx(rr, 2, (Color){235, 90, 90, 255});
                const char* rl = TextFormat("STOP %.0fs", recSeconds);
                DrawText(rl, (int)(rr.x + (rr.width - MeasureText(rl, 13)) / 2), (int)by + 6, 13, RAYWHITE);
                if (!uiLocked && CheckCollisionPointRec(GetMousePosition(), rr) &&
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    stopRecording();
                }
            } else {
                if (UIButton({692, by, 46, bh}, "RecA", 12) && !uiLocked) { recChoiceOpen = true; recChoiceVideo = false; }
                if (UIButton({744, by, 46, bh}, "RecV", 12) && !uiLocked) { recChoiceOpen = true; recChoiceVideo = true; }
            }
            // SAVE AS: sólo aparece cuando hay una toma reciente, y desaparece
            // sola cuando ya no la hay. Es lo que faltaba — la grabación caía en
            // temp/ y no había ninguna forma de sacarla de ahí desde el programa.
            if (!recording && recFinalPath[0] != '\0') {
                Rectangle sb = {796, by, 66, bh};
                DrawRectangleRec(sb, (Color){52, 74, 60, 255});
                DrawRectangleLinesEx(sb, 1, (Color){120, 190, 140, 255});
                DrawText("SAVE AS", (int)sb.x + 6, (int)by + 7, 11, (Color){170, 235, 190, 255});
                if (!uiLocked && CheckCollisionPointRec(GetMousePosition(), sb) &&
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    saveTakeAs();
            }
        }

        // --- Toolbar fila 2: CLIPS (página de 16) + Undo/Redo ---
        {
            float by = (float)row2Y + 4.0f;
            float bh = 24.0f;
            Vector2 m = GetMousePosition();

            DrawText("CLIP", 6, (int)by + 6, 13, activeBar == 0 ? RAYWHITE : (Color){160, 160, 180, 255});
            for (int i = 0; i < PAGE_SIZE; i++) {
                int slot = clipPage * PAGE_SIZE + i;
                Rectangle r = {(float)(46 + i * 27), by, 26, bh};
                bool hover = CheckCollisionPointRec(m, r);
                int state = clipState(slot);
                Color bg = hover ? (Color){72, 74, 100, 255} : (Color){40, 42, 58, 255};
                DrawRectangleRec(r, bg);
                Color txt = state == 2 ? (Color){120, 230, 140, 255}
                          : state == 1 ? (Color){110, 190, 240, 255}
                                       : (Color){95, 95, 115, 255};
                const char* num = slotLabel(slot);
                DrawText(num, (int)(r.x + (r.width - MeasureText(num, 11)) / 2), (int)(r.y + 7), 11, txt);
                bool isSel = activeBar == 0 && slot == selectedClipSlot;
                DrawRectangleLinesEx(r, isSel ? 2.0f : 1.0f, isSel ? RAYWHITE : (Color){80, 82, 105, 255});
                if (!uiLocked && hover && state > 0) previewSlot = slot;
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    activeBar = 0;
                    selectedClipSlot = slot;
                    if (state == 0) { // empty slot: ask file vs camera
                        slotChoiceOpen = true;
                        slotChoiceSlot = slot;
                        slotChoiceVideo = true;
                    }
                }
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) openEditor(slot);
            }
            if (UIButton({480, by, 20, bh}, "<", 12) && !uiLocked) clipPage = (clipPage + NUM_PAGES - 1) % NUM_PAGES;
            DrawText(TextFormat("%d-%d", clipPage * PAGE_SIZE + 1, clipPage * PAGE_SIZE + PAGE_SIZE), 504, (int)by + 7, 11, (Color){160, 160, 180, 255});
            if (UIButton({544, by, 20, bh}, ">", 12) && !uiLocked) clipPage = (clipPage + 1) % NUM_PAGES;

            if (UIButton({584, by, 46, bh}, "Undo", 12) && !uiLocked) doUndo();
            if (UIButton({632, by, 46, bh}, "Redo", 12) && !uiLocked) doRedo();

            // Octave selector for painting notes.
            DrawText("OCT", 686, (int)by + 6, 12, (Color){160, 160, 180, 255});
            if (UIButton({720, by, 22, bh}, "-", 14) && !uiLocked && g_octave > kMinOctave) { g_octave--; SetStatus("Octave %d", g_octave); }
            DrawText(TextFormat("%d", g_octave), 748, (int)by + 5, 16, (Color){255, 210, 100, 255});
            if (UIButton({766, by, 22, bh}, "+", 14) && !uiLocked && g_octave < kMaxOctave) { g_octave++; SetStatus("Octave %d", g_octave); }
        }

        // --- Toolbar fila 3: SAMPLES (página de 16) + velocidad ---
        {
            float by = (float)row3Y + 4.0f;
            float bh = 24.0f;
            Vector2 m = GetMousePosition();

            DrawText("SMP", 6, (int)by + 6, 13, activeBar == 1 ? RAYWHITE : (Color){160, 160, 180, 255});
            for (int i = 0; i < PAGE_SIZE; i++) {
                int slot = SAMPLE_BASE + samplePage * PAGE_SIZE + i;
                Rectangle r = {(float)(46 + i * 27), by, 26, bh};
                bool hover = CheckCollisionPointRec(m, r);
                int state = clipState(slot);
                Color bg = hover ? (Color){72, 74, 100, 255} : (Color){40, 42, 58, 255};
                DrawRectangleRec(r, bg);
                Color txt = state == 1 ? (Color){110, 190, 240, 255} : (Color){95, 95, 115, 255};
                const char* num = slotLabel(slot);
                DrawText(num, (int)(r.x + (r.width - MeasureText(num, 11)) / 2), (int)(r.y + 7), 11, txt);
                bool isSel = activeBar == 1 && slot == selectedSampleSlot;
                DrawRectangleLinesEx(r, isSel ? 2.0f : 1.0f, isSel ? RAYWHITE : (Color){80, 82, 105, 255});
                if (!uiLocked && hover && state > 0) previewSlot = slot;
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    activeBar = 1;
                    selectedSampleSlot = slot;
                    if (state == 0) { // empty slot: ask file vs mic
                        slotChoiceOpen = true;
                        slotChoiceSlot = slot;
                        slotChoiceVideo = false;
                    }
                }
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) openEditor(slot);
            }
            if (UIButton({480, by, 20, bh}, "<", 12) && !uiLocked) samplePage = (samplePage + NUM_PAGES - 1) % NUM_PAGES;
            DrawText(TextFormat("%c1-%c16", 'A' + samplePage, 'A' + samplePage), 504, (int)by + 7, 11, (Color){160, 160, 180, 255});
            if (UIButton({552, by, 20, bh}, ">", 12) && !uiLocked) samplePage = (samplePage + 1) % NUM_PAGES;

            DrawText("SPD", 584, (int)by + 6, 13, (Color){160, 160, 180, 255});
            for (int i = 0; i < kSpeedCount; i++) {
                Rectangle r = {(float)(616 + i * 34), by, 32, bh};
                bool hover = CheckCollisionPointRec(m, r);
                Color bg = hover ? (Color){72, 74, 100, 255} : (Color){40, 42, 58, 255};
                DrawRectangleRec(r, bg);
                DrawText(kSpeedLabels[i], (int)(r.x + (r.width - MeasureText(kSpeedLabels[i], 12)) / 2), (int)(r.y + 6), 12,
                         (Color){225, 200, 120, 255});
                DrawRectangleLinesEx(r, i == selectedSpeedIdx ? 2.0f : 1.0f,
                                     i == selectedSpeedIdx ? RAYWHITE : (Color){80, 82, 105, 255});
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) selectedSpeedIdx = i;
            }
        }

        // --- Paleta ---
        {
            for (int i = 0; i < PALETTE_CELLS; i++) {
                int x0 = i * leftPanelWidth / PALETTE_CELLS;
                int x1 = (i + 1) * leftPanelWidth / PALETTE_CELLS;
                Rectangle rec = {(float)x0, (float)paletteY, (float)(x1 - x0), (float)paletteHeight};

                if (i < NOTE_COUNT) {
                    // Piano note. Black keys (name has '#') get a darker band.
                    const PaletteEntry& entry = g_palette[i];
                    bool sharp = entry.label[1] == '#';
                    DrawRectangleRec(rec, entry.displayColor);
                    if (sharp) DrawRectangle((int)rec.x, (int)rec.y, (int)rec.width, 20, (Color){0, 0, 0, 90});
                    DrawText(entry.label, (int)(rec.x + (rec.width - MeasureText(entry.label, 16)) / 2), (int)rec.y + 6, 16,
                             sharp ? RAYWHITE : BLACK);
                    // Show the octave that painting would use (name + octave).
                    const char* full = SemitoneName(i + g_octave * 12);
                    DrawText(full, (int)(rec.x + (rec.width - MeasureText(full, 11)) / 2), (int)rec.y + paletteHeight - 16, 11,
                             sharp ? (Color){230, 230, 230, 200} : (Color){0, 0, 0, 150});
                } else if (i == TOOL_ARROW) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("TURN", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[A]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawArrowGlyph(rec.x + rec.width / 2, rec.y + paletteHeight - 20, 10, arrowDir, (Color){255, 210, 100, 255});
                } else if (i == TOOL_SUSTAIN) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("HOLD", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[S]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawText(kSustainLabels[sustainIdx],
                             (int)rec.x + 4, (int)rec.y + paletteHeight - 22, 14, (Color){255, 210, 100, 255});
                } else if (i == TOOL_FX) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("FX", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[F]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawText(kFxShort[fxTypeIdx], (int)rec.x + 4, (int)rec.y + paletteHeight - 20, 13, (Color){255, 210, 100, 255});
                } else if (i == TOOL_VOL) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("VOL", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[V]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawText(TextFormat("%d%%", (int)(kVolChoices[volIdx] * 100.0f + 0.5f)),
                             (int)rec.x + 4, (int)rec.y + paletteHeight - 20, 13, (Color){150, 235, 170, 255});
                } else if (i == TOOL_TIME) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("TIME", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[W]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawText(kTimeLabels[timeIdx], (int)rec.x + 4, (int)rec.y + paletteHeight - 20, 13,
                             (Color){255, 190, 110, 255});
                } else if (i == TOOL_ARP) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("ARP", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[G]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawText(kArpPatterns[arpIdx].name, (int)rec.x + 4, (int)rec.y + paletteHeight - 20, 12, (Color){255, 210, 100, 255});
                } else if (i == TOOL_MELODY) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("MEL", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[N]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    const MelodyClip& mel = g_melodies[melIdx];
                    DrawText(mel.label(melIdx).c_str(), (int)rec.x + 4, (int)rec.y + paletteHeight - 20, 12,
                             mel.empty() ? (Color){150, 150, 170, 255} : (Color){150, 220, 255, 255});
                } else if (i == TOOL_TELEPORT) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("PORT", (int)rec.x + 4, (int)rec.y + 8, 12, RAYWHITE);
                    DrawText("[P]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){150, 150, 170, 255});
                    DrawTeleGlyph(rec.x + rec.width / 2, rec.y + paletteHeight - 20, 10, kTeleColors[teleId % 8]);
                    DrawText(TextFormat("%d", teleId + 1), (int)(rec.x + rec.width / 2) - 3, (int)(rec.y + paletteHeight - 25), 10, kTeleColors[teleId % 8]);
                } else if (i == TOOL_MUTE) {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("MUTE", (int)rec.x + 4, (int)rec.y + 8, 12, (Color){235, 140, 140, 255});
                    DrawText("[M]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){160, 120, 120, 255});
                    DrawMuteGlyph(rec.x + rec.width / 2, rec.y + paletteHeight - 20, 10, (Color){235, 140, 140, 255});
                } else {
                    DrawRectangleRec(rec, (Color){40, 40, 50, 255});
                    DrawText("ERASE", (int)rec.x + 3, (int)rec.y + 9, 10, (Color){230, 120, 120, 255});
                    DrawText("[E]", (int)rec.x + 4, (int)rec.y + 24, 10, (Color){160, 120, 120, 255});
                    DrawLineEx({rec.x + 10, rec.y + 46}, {rec.x + rec.width - 10, rec.y + paletteHeight - 10}, 3, (Color){230, 120, 120, 255});
                    DrawLineEx({rec.x + rec.width - 10, rec.y + 46}, {rec.x + 10, rec.y + paletteHeight - 10}, 3, (Color){230, 120, 120, 255});
                }

                if (i == selectedTool) {
                    DrawRectangleLinesEx(rec, 3, RAYWHITE);
                } else {
                    DrawRectangleLinesEx(rec, 1, (Color){20, 20, 26, 255});
                }
            }
        }

        // --- Hover preview of a CLIP/SMP slot (popup above the toolbar) ---
        if (previewSlot >= 0) drawSlotPreview(previewSlot, GetMouseX());

        // --- Panel derecho ---
        DrawRectangle(leftPanelWidth, 0, rightPanelWidth, screenHeight, (Color){20, 20, 28, 255});
        DrawLine(leftPanelWidth, 0, leftPanelWidth, screenHeight, (Color){60, 60, 80, 255});
        DrawText("VIDEO", leftPanelWidth + 12, 8, 18, RAYWHITE);
        DrawText(TextFormat("noteys %d | voices %d/%d | %.0f MB", snapshot.count,
                            g_videoVoicePool.activeCount(), MAX_VIDEO_VOICES, bankRamMB()),
                 leftPanelWidth + 180, 11, 12, (Color){160, 160, 180, 255});

        // --- Barra de escenas ---
        {
            Vector2 m = GetMousePosition();
            const char* sceneLabel = (songMode && g_scenes[g_curScene].duration > 0.0f)
                                         ? TextFormat("SCENE %.1fs", sceneTimeLeft)
                                         : "SCENE";
            DrawText(sceneLabel, leftPanelWidth + 12, 38, 12, (Color){160, 160, 180, 255});

            int baseX = leftPanelWidth + 96;
            for (int i = 0; i < (int)g_scenes.size(); i++) {
                Rectangle r = {(float)(baseX + i * 28), 30, 26, 28};
                bool hover = CheckCollisionPointRec(m, r);
                Color bg = i == g_curScene ? (Color){64, 68, 96, 255}
                         : hover ? (Color){72, 74, 100, 255} : (Color){40, 42, 58, 255};
                DrawRectangleRec(r, bg);
                DrawText(TextFormat("%d", i + 1), (int)r.x + 9, (int)r.y + 2, 12, RAYWHITE);
                if (g_scenes[i].duration > 0.0f) {
                    DrawText(TextFormat("%.0fs", g_scenes[i].duration), (int)r.x + 3, (int)r.y + 16, 10, (Color){255, 210, 100, 255});
                } else {
                    DrawText("-", (int)r.x + 11, (int)r.y + 16, 10, (Color){110, 110, 130, 255});
                }
                DrawRectangleLinesEx(r, i == g_curScene ? 2.0f : 1.0f,
                                     i == g_curScene ? RAYWHITE : (Color){80, 82, 105, 255});

                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    clearUndoHistory();
                    activateScene(i);
                }
                if (!uiLocked && hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) askSceneDuration(i);
            }

            if ((int)g_scenes.size() < MAX_SCENES) {
                Rectangle r = {(float)(baseX + (int)g_scenes.size() * 28), 30, 26, 28};
                if (UIButton(r, "+") && !uiLocked) {
                    g_scenes.emplace_back();
                    clearUndoHistory();
                    activateScene((int)g_scenes.size() - 1);
                    SetStatus("Scene %d added (empty canvas)", (int)g_scenes.size());
                }
            }

            Rectangle songBtn = {(float)(screenWidth - 64), 30, 56, 28};
            bool songHover = CheckCollisionPointRec(m, songBtn);
            DrawRectangleRec(songBtn, songMode ? (Color){60, 96, 66, 255} : (songHover ? (Color){72, 74, 100, 255} : (Color){46, 48, 64, 255}));
            DrawRectangleLinesEx(songBtn, songMode ? 2.0f : 1.0f, songMode ? (Color){120, 230, 140, 255} : (Color){110, 112, 140, 255});
            DrawText("SONG", (int)songBtn.x + 10, (int)songBtn.y + 8, 13, songMode ? (Color){120, 230, 140, 255} : RAYWHITE);
            if (!uiLocked && songHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                songMode = !songMode;
                if (songMode) {
                    if (g_scenes[g_curScene].duration > 0.0f) {
                        sceneTimeLeft = g_scenes[g_curScene].duration;
                        SetStatus("SONG mode on: scenes advance automatically");
                    } else {
                        int found = -1;
                        for (int i = 0; i < (int)g_scenes.size(); i++) {
                            if (g_scenes[i].duration > 0.0f) { found = i; break; }
                        }
                        if (found >= 0) {
                            clearUndoHistory();
                            activateScene(found);
                            SetStatus("SONG mode on: scenes advance automatically");
                        } else {
                            songMode = false;
                            SetStatus("Set scene durations first (right-click a scene number)");
                        }
                    }
                } else {
                    SetStatus("SONG mode off");
                }
            }

            float ty = 62.0f;
            DrawText("TOOLS", leftPanelWidth + 12, (int)ty + 6, 12, (Color){160, 160, 180, 255});
            if (UIButton({(float)(leftPanelWidth + 68), ty, 44, 22}, "Dup", 12) && !uiLocked) duplicateScene();
            if (UIButton({(float)(leftPanelWidth + 116), ty, 44, 22}, "Del", 12) && !uiLocked) deleteScene();
            if (UIButton({(float)(leftPanelWidth + 164), ty, 30, 22}, "<", 12) && !uiLocked) moveScene(-1);
            if (UIButton({(float)(leftPanelWidth + 198), ty, 30, 22}, ">", 12) && !uiLocked) moveScene(1);

            // LIVE: ventana espejo para otro monitor / proyector.
            Rectangle liveBtn = {(float)(leftPanelWidth + 236), ty, 52, 22};
            bool liveHover = CheckCollisionPointRec(m, liveBtn);
            DrawRectangleRec(liveBtn, liveOn ? (Color){96, 60, 66, 255} : (liveHover ? (Color){72, 74, 100, 255} : (Color){46, 48, 64, 255}));
            DrawRectangleLinesEx(liveBtn, liveOn ? 2.0f : 1.0f, liveOn ? (Color){235, 90, 90, 255} : (Color){110, 112, 140, 255});
            DrawText("LIVE", (int)liveBtn.x + 12, (int)liveBtn.y + 5, 12, liveOn ? (Color){235, 120, 120, 255} : RAYWHITE);
            if (!uiLocked && liveHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) toggleLive();

            const char* vmLabel = g_exportH > g_exportW ? "9:16 vertical" : "16:9 normal";
            if (UIButton({(float)(screenWidth - 118), ty, 110, 22}, vmLabel, 12) && !uiLocked) toggleVideoMode();
        }

        const int videosY0 = 92;
        const int helpHeight = 186; // deja sitio para la barra de MODELOS 3D
        const int videosY1 = screenHeight - helpHeight;
        int activeVoices = g_videoVoicePool.activeCount();

        // --- Collage YTPMV ---
        //
        // SE REDIBUJA A 30 fps, NO A 60. Nadie lo consume más rápido: la
        // grabación toma 30, la ventana LIVE toma 30, y el material de vídeo
        // viene ya decodificado a 30 como mucho. Redibujarlo a 60 producía la
        // MITAD DE LOS FOTOGRAMAS IDÉNTICOS al anterior, a cambio de volver a
        // subir a la GPU todas las texturas de todas las voces activas — que es
        // exactamente donde se iba el tiempo (23 de los 27 ms del collage).
        //
        // Cuando se salta, la textura conserva lo último dibujado, así que tanto
        // la captura como la vista previa siguen leyendo algo válido.
        const double kCollageFps = 30.0;
        collageAccum += dt;
        const bool drawCollage = (collageAccum >= 1.0 / kCollageFps) || collageFirst;
        if (drawCollage) {
            collageAccum = fmod(collageAccum, 1.0 / kCollageFps);
            collageFirst = false;
        }
#ifdef UI_SMOKE_TEST
        const double tc0 = GetTime();
        if (drawCollage) g_collageDrawn++;
        g_collageAsked++;
#endif
        if (drawCollage) {
        BeginTextureMode(collageRT);
        ClearBackground(BLACK);
        {
            std::vector<const VideoVoice*> order;
            g_videoVoicePool.forEachActiveVoice([&](const VideoVoice& voice) {
                if (g_engine.getInstrumentBank().at(voice.sampleId).hasVideo()) order.push_back(&voice);
            });
            // Se ordena por CAPA, y dentro de cada capa por (clip, fotograma).
            //
            // La capa es lo que decide quién tapa a quién y no se puede tocar.
            // Pero DENTRO de una capa el orden no significaba nada (era el del
            // pool), y agrupar ahí las voces que usan el mismo clip en el mismo
            // fotograma ahorra subidas a la GPU: la caché guarda UN fotograma
            // por clip, así que con las voces mezcladas la secuencia A5, A9, A5
            // subía tres veces, y ordenadas sube dos.
            //
            // Y eso importa porque medirlo dejó claro que las subidas son el 97%
            // de lo que cuesta el collage: sin ellas, 24 ms pasan a 0,6.
            auto frameOf = [&](const VideoVoice* v) {
                const InstrumentSource& sc = g_engine.getInstrumentBank().at(v->sampleId);
                int f = (int)v->frameCursor;
                if (f < 0) f = 0;
                if (f >= (int)sc.videoFrames.size()) f = (int)sc.videoFrames.size() - 1;
                return f;
            };
            std::stable_sort(order.begin(), order.end(), [&](const VideoVoice* a, const VideoVoice* b) {
                const int la = g_clipFX[a->sampleId].layer * 2 + (g_clipFX[a->sampleId].center ? 1 : 0);
                const int lb = g_clipFX[b->sampleId].layer * 2 + (g_clipFX[b->sampleId].center ? 1 : 0);
                if (la != lb) return la < lb;
                if (a->sampleId != b->sampleId) return a->sampleId < b->sampleId;
                return frameOf(a) < frameOf(b);
            });

            for (const VideoVoice* pv : order) {
                const VideoVoice& voice = *pv;
                const InstrumentSource& src = g_engine.getInstrumentBank().at(voice.sampleId);
                const ClipFX& fx = g_clipFX[voice.sampleId];

                int frameIdx = (int)voice.frameCursor;
                if (frameIdx < 0) frameIdx = 0;
                if (frameIdx >= (int)src.videoFrames.size()) frameIdx = (int)src.videoFrames.size() - 1;

                Texture2D& tex = getOrCreateTexture(voice.sampleId, src.videoWidth, src.videoHeight, src.videoChannels);
                uploadSlotFrame(tex, voice.sampleId, frameIdx, src);

                float aspect = (float)tex.width / (float)tex.height;
                float zoomS = fx.zoomPulse ? (0.2f + 0.8f * fminf(voice.ageSeconds / 0.35f, 1.0f)) : 1.0f;
                float base = (float)(g_exportW < g_exportH ? g_exportW : g_exportH);
                float h = base * (fx.center ? 0.78f : 0.47f) * fx.scale * zoomS;
                float w = h * aspect;

                // Dónde cae el clip. Cuatro formas, en este orden de prioridad:
                // MOVE (recorre A->B), PLACE (donde lo puso el usuario),
                // CENTER (en medio) y, si no hay ninguna, el reparto al azar
                // de siempre — que sigue siendo el comportamiento por defecto.
                float px, py;
                if (fx.move) {
                    float t = fminf(voice.ageSeconds / 0.6f, 1.0f);
                    px = (fx.ax + (fx.bx - fx.ax) * t) * g_exportW;
                    py = (fx.ay + (fx.by - fx.ay) * t) * g_exportH;
                } else if (fx.place) {
                    px = fx.posX * g_exportW;
                    py = fx.posY * g_exportH;
                } else if (fx.center) {
                    px = g_exportW / 2.0f;
                    py = g_exportH / 2.0f;
                } else {
                    unsigned hash = (unsigned)voice.bichoIndex * 2654435761u;
                    px = g_exportW * (0.11f + (float)(hash % 1000u) / 999.0f * 0.78f);
                    py = g_exportH * (0.14f + (float)((hash >> 16) % 1000u) / 999.0f * 0.72f);
                }
                // El giro fijo del usuario y el giro animado se SUMAN: se puede
                // dejar un clip torcido 15 grados y además ponerlo a dar vueltas.
                float rot = fx.rotDeg + (fx.rotate ? fmodf(voice.ageSeconds * 60.0f, 360.0f) : 0.0f);

                bool mirrored = fx.flipX && voice.flipParity;
                Rectangle srcRec = {0, 0, mirrored ? -(float)tex.width : (float)tex.width, (float)tex.height};
                // Per-slot GLSL video effect (imported .fs/.glsl), if any.
                bool useShader = g_slotShaderOn[voice.sampleId] && g_slotShader[voice.sampleId].id != 0;
                if (useShader) {
                    Shader& sh = g_slotShader[voice.sampleId];
                    if (g_slotShaderLocTime[voice.sampleId] >= 0) { float tt = (float)GetTime(); SetShaderValue(sh, g_slotShaderLocTime[voice.sampleId], &tt, SHADER_UNIFORM_FLOAT); }
                    if (g_slotShaderLocRes[voice.sampleId] >= 0) { Vector2 rr = {(float)tex.width, (float)tex.height}; SetShaderValue(sh, g_slotShaderLocRes[voice.sampleId], &rr, SHADER_UNIFORM_VEC2); }
                    BeginShaderMode(sh);
                }
                // Opacidad y modo de fusión de la capa. El tinte lleva el alfa,
                // así que la transparencia funciona en TODOS los modos (y se
                // multiplica con el alfa que ya trajera un PNG o un GIF).
                Color tint = BlendTintFX(fx.blend, fx.opacity);
                BeginBlendModeFX(fx.blend);
                DrawTexturePro(tex, srcRec, {px, py, w, h}, {w / 2.0f, h / 2.0f}, rot, tint);
                EndBlendModeFX(fx.blend);
                if (useShader) EndShaderMode();
            }

#ifdef UI_SMOKE_TEST
            g_s2D.push_back(GetTime() - tc0);
            const double t3d0 = GetTime();
#endif
            // --- Pasada 3D: modelos disparados por noteys ---
            int nActive = 0;
            for (const ModelVoice& v : g_modelVoices) if (v.active) nActive++;
            if (nActive > 0) {
                Camera3D cam = {0};
                cam.position = {0.0f, 1.0f, 4.5f};
                cam.target = {0.0f, 0.9f, 0.0f};
                cam.up = {0.0f, 1.0f, 0.0f};
                cam.fovy = 45.0f;
                cam.projection = CAMERA_PERSPECTIVE;
                SetLightUniforms(cam.position);
                BeginMode3D(cam);
                int idx = 0;
                for (const ModelVoice& v : g_modelVoices) {
                    if (!v.active) continue;
                    ModelSlot& s = g_models[v.model];
                    if (!s.loaded) continue;
                    Vector3 pd, rd; float sd;
                    PoseModel(s, v.anim, v.frame, pd, rd, sd);
                    float spread = (nActive > 1) ? ((idx - (nActive - 1) * 0.5f) * 1.8f) : 0.0f;
                    DrawModelSlotAt(s, spread, pd, rd, sd);
                    idx++;
                }
                EndMode3D();
            }
#ifdef UI_SMOKE_TEST
            g_s3D.push_back(GetTime() - t3d0);
#endif
        }
        EndTextureMode();
        }   // fin de if (drawCollage)

        // --- Pasada de vídeo analógico (NTSC/VHS) sobre la MEZCLA FINAL ---
        // Va aquí y no dentro del bucle de capas a propósito: el aspecto de una
        // cinta lo da la señal ENTERA, no cada clip por su cuenta. Si estuviera
        // por capa, cada una tendría su propio ruido y su propio sangrado de
        // color y no parecería una emisión, sino un collage con filtros.
        //
        // El resultado se guarda en postRT y de ahí sale TODO: lo que se graba,
        // lo que ve la ventana LIVE y la vista previa. Así lo que se ve es
        // exactamente lo que se está grabando.
        RenderTexture2D& outRT = (g_ntsc.enabled && g_ntscShader.ok) ? postRT : collageRT;
        if (g_ntsc.enabled) {
            if (!g_ntscShader.ok) g_ntscShader.load();
            if (g_ntscShader.ok) {
                g_ntscShader.apply(g_ntsc, g_exportW, g_exportH);
                BeginTextureMode(postRT);
                ClearBackground(BLACK);
                BeginShaderMode(g_ntscShader.sh);
                // La textura de un render target viene con la Y al revés, y
                // como este paso la vuelve a escribir en otro target, se voltea
                // aquí para que las dos vueltas se cancelen.
                DrawTexturePro(collageRT.texture,
                               {0, 0, (float)g_exportW, -(float)g_exportH},
                               {0, 0, (float)g_exportW, (float)g_exportH}, {0, 0}, 0.0f, WHITE);
                EndShaderMode();
                EndTextureMode();
            }
        }

#ifdef UI_SMOKE_TEST
        { const double d = GetTime() - tc0; g_tCollage += d; g_sCollage.push_back(d); }
        const double tp0 = GetTime();
#endif

        // --- Captura del collage: exportación (30fps) y/o salida LIVE ---
        {
            bool wantRec = recording && recVideo && recPipe != nullptr;
            if (recording) recSeconds += dt;

            int recFrames = 0;
            if (wantRec) {
                recAccum += dt;
                while (recAccum >= 1.0f / 30.0f) {
                    recAccum -= 1.0f / 30.0f;
                    recFrames++;
                }
            }
            bool wantLive = false;
            if (liveOn) {
                liveAccum += dt;
                if (liveAccum >= 1.0f / 30.0f) {
                    liveAccum = fmodf(liveAccum, 1.0f / 30.0f);
                    wantLive = true;
                }
            }

            if (recFrames > 0 || wantLive) {
                Image img = LoadImageFromTexture(outRT.texture);
                for (int k = 0; k < recFrames; k++) {
                    fwrite(img.data, 1, (size_t)g_exportW * g_exportH * 4, recPipe);
                }
                if (wantLive) LiveShmWrite(img.data, g_exportW, g_exportH);
                UnloadImage(img);
            }
        }

#ifdef UI_SMOKE_TEST
        { const double d = GetTime() - tp0; g_tCapture += d; g_sCapture.push_back(d); }
#endif

        // --- Vista previa del collage ---
        {
            float availW = (float)rightPanelWidth;
            float availH = (float)(videosY1 - videosY0);
            float pscale = availW / g_exportW;
            if (g_exportH * pscale > availH) pscale = availH / g_exportH;
            float pw = g_exportW * pscale;
            float ph = g_exportH * pscale;
            Rectangle dst = {(float)leftPanelWidth + (availW - pw) / 2, videosY0 + (availH - ph) / 2, pw, ph};
            DrawRectangleLinesEx({dst.x - 1, dst.y - 1, dst.width + 2, dst.height + 2}, 1, (Color){70, 70, 95, 255});
            DrawTexturePro(outRT.texture, {0, 0, (float)g_exportW, -(float)g_exportH}, dst, {0, 0}, 0.0f, WHITE);
            DrawText(TextFormat("EXPORT PREVIEW (%dx%d, RecV)", g_exportW, g_exportH),
                     leftPanelWidth + 12, videosY1 - 16, 11, (Color){120, 120, 145, 255});

            if (activeVoices == 0) {
                const char* placeholder[] = {
                    "No video playing.",
                    "Load clips/samples from anywhere: drop",
                    "files here or click an empty CLIP/SMP slot.",
                    "Drop a .mid file to fill the TRACKER.",
                    "LIVE opens a mirror window for shows.",
                };
                for (int i = 0; i < (int)(sizeof(placeholder) / sizeof(placeholder[0])); i++) {
                    int tw2 = MeasureText(placeholder[i], 14);
                    DrawText(placeholder[i], leftPanelWidth + (rightPanelWidth - tw2) / 2,
                             (int)(dst.y + 60 + i * 22), 14, (Color){150, 150, 170, 255});
                }
            }

            if (recording && recVideo) {
                DrawCircle(leftPanelWidth + 22, (int)dst.y + 16, 7, (Color){235, 70, 70, 255});
                DrawText(TextFormat("REC %.1fs", recSeconds), leftPanelWidth + 34, (int)dst.y + 9, 15, (Color){235, 90, 90, 255});
            }
            if (liveOn) {
                DrawText("LIVE", leftPanelWidth + (int)dst.width - 44, (int)dst.y + 8, 14, (Color){235, 120, 120, 255});
            }
        }

        // --- Ayuda ---
        {
            Vector2 mm = GetMousePosition();
            int hy = videosY1 + 6;
            DrawLine(leftPanelWidth, videosY1, screenWidth, videosY1, (Color){60, 60, 80, 255});

            // ---------- MODELOS 3D (.glb / .vrm) ----------
            DrawText("MODELS 3D", leftPanelWidth + 12, hy, 13, (Color){150, 210, 235, 255});
            DrawText("(click empty=load .glb/.vrm | click=select | R-click=remove)", leftPanelWidth + 92, hy + 1, 10, (Color){130, 130, 155, 255});
            int mbx = leftPanelWidth + 12, mby = hy + 16;
            for (int i = 0; i < MAX_MODELS; i++) {
                Rectangle r = {(float)(mbx + i * 92), (float)mby, 88, 22};
                bool sel = (selectedModel == i);
                bool hov = !uiLocked && CheckCollisionPointRec(mm, r);
                bool has = g_models[i].loaded;
                DrawRectangleRec(r, sel ? (Color){60, 78, 96, 255} : (hov ? g_theme.buttonHover : g_theme.button));
                DrawRectangleLinesEx(r, sel ? 2.0f : 1.0f, has ? (Color){120, 200, 235, 255} : g_theme.border);
                if (has) {
                    const char* nm = GetFileName(g_models[i].path.c_str());
                    DrawText(nm, (int)r.x + 5, (int)r.y + 4, 10, RAYWHITE);
                    DrawText(TextFormat("%da%s", modelAnimTotal(g_models[i]), modelIsHumanoid(g_models[i]) ? "h" : (modelIsProcedural(g_models[i]) ? "*" : "")), (int)r.x + 5, (int)r.y + 13, 9, (Color){150, 200, 170, 255});
                } else {
                    DrawText("+ model", (int)r.x + 16, (int)r.y + 6, 11, (Color){150, 150, 175, 255});
                }
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    if (has) { selectedModel = i; }
                    else {
                        char path[512] = "";
                        if (NativeOpenDialog("Load a 3D model (.glb/.gltf/.vrm)...", "*.glb *.gltf *.vrm", path, sizeof(path)) == 1) {
                            if (LoadModelSlot(i, path)) { selectedModel = i; SetStatus("Model loaded: %s (%d anims)", GetFileName(path), g_models[i].animCount); }
                            else SetStatus("Could not load model %s", GetFileName(path));
                        }
                    }
                }
                if (has && hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                    modelEditorOpen = true; modelEditorSlot = i; modelEditorAnim = 0; modelEditorFrame = 0.0f;
                    selectedModel = i;
                }
            }

            // ---------- Animaciones del modelo seleccionado ----------
            int ay = mby + 26;
            if (selectedModel >= 0 && g_models[selectedModel].loaded) {
                ModelSlot& ms = g_models[selectedModel];
                DrawText(modelIsProcedural(ms) ? "ANIM*" : "ANIM:", leftPanelWidth + 12, ay + 4, 11, (Color){160, 160, 180, 255});
                int axp = leftPanelWidth + 54;
                for (int a = 0; a < modelAnimTotal(ms); a++) {
                    Rectangle r = {(float)axp, (float)ay, 26, 20};
                    int id = modelAnimId(selectedModel, a);
                    bool sel = (activeBar == 2 && selectedModelAnim == id);
                    bool hov = !uiLocked && CheckCollisionPointRec(mm, r);
                    DrawRectangleRec(r, sel ? (Color){70, 96, 110, 255} : (hov ? g_theme.buttonHover : g_theme.button));
                    DrawRectangleLinesEx(r, sel ? 2.0f : 1.0f, g_theme.border);
                    DrawText(TextFormat("%d", a), (int)r.x + (a < 10 ? 9 : 5), (int)r.y + 5, 10, RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        activeBar = 2; selectedModelAnim = id;
                        SetStatus("Model anim %d selected - paint on the CANVAS", a);
                    }
                    axp += 28;
                    if (axp + 26 > screenWidth - 8) { axp = leftPanelWidth + 54; ay += 22; }
                }
                // Nombre de la animación seleccionada.
                if (activeBar == 2 && modelOfId(selectedModelAnim) == selectedModel) {
                    int an = animOfId(selectedModelAnim);
                    if (an >= 0 && an < (int)ms.animNames.size())
                        DrawText(TextFormat("-> %s", ms.animNames[an].c_str()), leftPanelWidth + 12, ay + 24, 10, (Color){150, 210, 235, 255});
                }
                ay += 46;
            } else {
                DrawText("Select a model to see its animations. Paint a note with an anim; a", leftPanelWidth + 12, ay + 2, 10, (Color){150, 150, 170, 255});
                DrawText("notey that steps on it plays that animation in the collage.", leftPanelWidth + 12, ay + 16, 10, (Color){150, 150, 170, 255});
                ay += 40;
            }

            // ---------- Ayuda compacta ----------
            const char* lines[] = {
                "CLIP/SMP: click empty=load, R-click=trim editor | DEV: theme/pad/mic",
                "1-8 color | A arrow S hold F fx P portal M mute E erase | SPACE play",
            };
            for (int i = 0; i < 2; i++) DrawText(lines[i], leftPanelWidth + 12, ay + i * 15, 11, (Color){160, 160, 180, 255});
        }

        // --- Panel DEVICES (elegir puerto MIDI + diagnóstico de mando/teclado) ---
        if (showDevices) {
            Vector2 m = GetMousePosition();
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {70, 40, 1140, 660};
            DrawRectangleRec(panel, (Color){24, 25, 38, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            DrawText("DEVICES", (int)panel.x + 20, (int)panel.y + 14, 20, RAYWHITE);
            if (UIButton({panel.x + panel.width - 96, panel.y + 12, 80, 26}, "Close", 13) || IsKeyPressed(KEY_ESCAPE))
                showDevices = false;
            // Enseñar/ocultar las direcciones IP del panel. Vive en la barra de
            // título porque afecta a las dos columnas de la pestaña del teléfono.
            // Arranca SIEMPRE oculto: este panel se abre con la ventana
            // compartida o mientras se graba más a menudo de lo que parece, y la
            // dirección de casa no tiene por qué acabar en el vídeo.
            if (devTab == 1) {
                if (UIButton({panel.x + panel.width - 216, panel.y + 12, 110, 26},
                             g_showIp ? "Hide addresses" : "Show addresses", 11))
                    g_showIp = !g_showIp;
            }

            // Dos pestañas: la sección del TELÉFONO creció (app propia y app
            // MJPEG) y ya no cabía apretada en media columna junto al MIDI y
            // el mando.
            {
                const char* tabName[2] = {"CONTROLLERS & THEME", "PHONE / CAMERA / MIC"};
                for (int t = 0; t < 2; t++) {
                    Rectangle tb = {panel.x + 140 + t * 210, panel.y + 12, 200, 26};
                    bool sel = (devTab == t);
                    bool hov = CheckCollisionPointRec(m, tb);
                    DrawRectangleRec(tb, sel ? (Color){52, 78, 62, 255} : (hov ? (Color){48, 50, 66, 255} : (Color){34, 36, 50, 255}));
                    DrawRectangleLinesEx(tb, sel ? 2.0f : 1.0f, sel ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                    DrawText(tabName[t], (int)tb.x + 12, (int)tb.y + 7, 12, sel ? (Color){180, 240, 200, 255} : (Color){190, 190, 205, 255});
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) devTab = t;
                }
            }

            // ---------- MIDI IN ----------
            if (devTab == 0) {
            int mx = (int)panel.x + 20, my = (int)panel.y + 56;
            DrawText("MIDI INPUT - click a port to use it", mx, my, 15, (Color){150, 210, 235, 255});
            my += 24;
            int nPorts = 0;
            try { nPorts = g_midiIn ? (int)g_midiIn->getPortCount() : 0; } catch (...) { nPorts = 0; }
            if (!g_midiIn) {
                DrawText("(RtMidi input not available)", mx, my, 13, (Color){200, 140, 140, 255}); my += 20;
            } else if (nPorts == 0) {
                DrawText("No MIDI ports. Plug the keyboard in, then press Rescan.", mx, my, 13, (Color){200, 180, 140, 255});
                my += 22;
            } else {
                for (int i = 0; i < nPorts; i++) {
                    std::string nm;
                    try { nm = g_midiIn->getPortName((unsigned)i); } catch (...) { nm = "?"; }
                    bool open = (i == midiInPort);
                    Rectangle r = {(float)mx, (float)my, 480, 22};
                    bool hov = CheckCollisionPointRec(m, r);
                    DrawRectangleRec(r, open ? (Color){50, 80, 60, 255} : (hov ? (Color){48, 50, 66, 255} : (Color){32, 34, 48, 255}));
                    DrawRectangleLinesEx(r, 1, open ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                    DrawText(TextFormat("%d: %s%s", i, nm.c_str(), open ? "   [open]" : ""),
                             mx + 6, my + 4, 13, open ? (Color){170, 235, 190, 255} : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) openMidiPort(i);
                    my += 24;
                }
            }
            if (UIButton({(float)mx, (float)my + 2, 90, 24}, "Rescan", 12)) {
                // RtMidi re-enumera al reconstruir el cliente.
                try {
                    int keep = midiInPort;
                    if (g_midiIn) { g_midiIn->closePort(); delete g_midiIn; }
                    g_midiIn = new RtMidiIn();
                    midiInPort = -1;
                    if (keep >= 0 && keep < (int)g_midiIn->getPortCount()) openMidiPort(keep);
                    SetStatus("MIDI rescanned (%d port(s))", (int)g_midiIn->getPortCount());
                } catch (...) { g_midiIn = nullptr; }
            }
            // Último mensaje MIDI recibido (para ver si LLEGA algo).
            if (midiLastLen > 0) {
                const char* kind = "";
                unsigned char hi = midiLastBytes[0] & 0xF0;
                if (hi == 0x90) kind = "Note On"; else if (hi == 0x80) kind = "Note Off";
                else if (hi == 0xB0) kind = "CC"; else if (midiLastBytes[0] == 0xFA) kind = "Start";
                else if (midiLastBytes[0] == 0xFC) kind = "Stop";
                DrawText(TextFormat("last: %02X %02X %02X  %s", midiLastBytes[0], midiLastBytes[1], midiLastBytes[2], kind),
                         mx + 110, my + 6, 13, midiHudTimer > 0.0f ? (Color){235, 210, 130, 255} : (Color){130, 130, 150, 255});
            } else {
                DrawText("last: (nothing received yet - press a key)", mx + 110, my + 6, 13, (Color){130, 130, 150, 255});
            }
            } // devTab == 0 (MIDI)

            // ---------- CAMERA / MIC (recording) ----------
            if (devTab == 1) {
                int cxr = (int)panel.x + 20, cyr = (int)panel.y + 56;
                DrawText("CAMERA / MIC (recording)", cxr, cyr, 15, (Color){235, 200, 120, 255}); cyr += 22;
                DrawText("Used when you Record into an empty slot.", cxr, cyr, 11, (Color){150, 150, 175, 255}); cyr += 20;

                DrawText("Camera:", cxr, cyr, 12, (Color){160, 200, 235, 255}); cyr += 18;
                auto camRow = [&](const char* name) {
                    bool sel = (camDevice == name);
                    Rectangle r = {(float)cxr, (float)cyr, 320, 20};
                    bool hov = CheckCollisionPointRec(m, r);
                    DrawRectangleRec(r, sel ? (Color){50, 80, 60, 255} : (hov ? (Color){48, 50, 66, 255} : (Color){32, 34, 48, 255}));
                    DrawRectangleLinesEx(r, 1, sel ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                    DrawText(name, cxr + 6, cyr + 4, 11, sel ? (Color){170, 235, 190, 255} : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { camDevice = name; saveControls(); }
                    cyr += 22;
                };
                bool anyCam = false;
#if defined(_WIN32)
                // Windows: nombres DirectShow, enumerados con ffmpeg.
                for (const std::string& c : camSources) { camRow(c.c_str()); anyCam = true; }
                if (!anyCam) { DrawText("(no camera found - is ffmpeg on the PATH?)", cxr, cyr, 11, (Color){170, 150, 150, 255}); cyr += 20; }
#else
                for (int i = 0; i < 10; i++) {
                    char p[24]; snprintf(p, sizeof(p), "/dev/video%d", i);
                    if (!FileExists(p)) continue;
                    anyCam = true;
                    camRow(p);
                }
                if (!anyCam) { DrawText("(no /dev/video* found)", cxr, cyr, 11, (Color){170, 150, 150, 255}); cyr += 20; }
#endif

                cyr += 6;
                DrawText("Microphone (audio source):", cxr, cyr, 12, (Color){160, 200, 235, 255}); cyr += 18;
                auto micRow = [&](const char* name) {
                    bool sel = (micDevice == name);
                    Rectangle r = {(float)cxr, (float)cyr, 320, 20};
                    bool hov = CheckCollisionPointRec(m, r);
                    DrawRectangleRec(r, sel ? (Color){50, 80, 60, 255} : (hov ? (Color){48, 50, 66, 255} : (Color){32, 34, 48, 255}));
                    DrawRectangleLinesEx(r, 1, sel ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                    DrawText(name, cxr + 6, cyr + 4, 10, sel ? (Color){170, 235, 190, 255} : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { micDevice = name; saveControls(); }
                    cyr += 22;
                };
#if !defined(_WIN32)
                micRow("default");   // PulseAudio entiende "default"; dshow no
#endif
                int shown = 0;
                for (const std::string& s : micSources) {
                    if (shown++ >= 8) break;   // the column is its own tab now
                    micRow(s.c_str());
                }
                if (micSources.empty()) {
                    DrawText("(none detected - press Refresh)", cxr, cyr, 11, (Color){170, 150, 150, 255});
                    cyr += 20;
                }
                if (UIButton({(float)cxr, (float)cyr + 2, 90, 22}, "Refresh", 12)) refreshMicSources();

                // ================= PHONE as camera / mic =================
                // Three routes, laid out in their own columns and ordered by how
                // little the user has to do. Column 2 is OUR app (nothing to
                // configure), column 3 the third-party MJPEG camera apps.
                int px = (int)panel.x + 380;   // column 2
                int py = (int)panel.y + 56;

                // El estado del tailnet se relee de vez en cuando mientras el
                // panel está abierto (el usuario puede encender Tailscale en el
                // móvil AHORA MISMO y esperar verlo aparecer sin reiniciar nada).
                {
                    static float tsTimer = 0.0f;
                    tsTimer -= GetFrameTime();
                    if (tsTimer <= 0.0f && !g_tailscale.refreshing()) {
                        g_tailscale.refreshAsync();
                        tsTimer = 6.0f;
                    }
                    // Los requisitos SÍ se recalculan cada fotograma: no lanzan
                    // procesos (sólo leen lo último que trajo el hilo), y así el
                    // panel enseña el estado de verdad en cuanto llega, en vez de
                    // la foto del refresco anterior.
                    g_tailscale.requirements(true);
                }

                // ---------- 1) Pinguus Cam: our own app ----------
                DrawText("PHONE AS CAMERA - our app (no ads)", px, py, 15, (Color){150, 200, 255, 255});
                py += 22;

                // Selector de vía: Tailscale (recomendada) o Wi-Fi local.
                {
                    const char* rname[2] = {"Tailscale (anywhere)", "Local Wi-Fi"};
                    for (int r = 0; r < 2; r++) {
                        Rectangle rb = {(float)(px + r * 172), (float)py, 166, 22};
                        bool sel = (g_phoneRoute == r);
                        bool hov = CheckCollisionPointRec(m, rb);
                        DrawRectangleRec(rb, sel ? (Color){52, 78, 62, 255} : (hov ? g_theme.buttonHover : g_theme.button));
                        DrawRectangleLinesEx(rb, sel ? 2.0f : 1.0f, sel ? (Color){120, 210, 150, 255} : g_theme.border);
                        DrawText(rname[r], (int)rb.x + 10, (int)rb.y + 6, 11, sel ? (Color){180, 240, 200, 255} : (Color){190, 190, 205, 255});
                        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_phoneRoute = r; saveControls(); }
                    }
                    py += 26;
                }

                // ---------- Vía TAILSCALE ----------
                // Nada de esto reimplementa red: el servidor del APK y el enlace
                // UDP ya escuchan en todas las interfaces. Lo único que cambia es
                // QUÉ dirección se le enseña al usuario — la 100.x del tailnet, que
                // sólo alcanzan sus propios dispositivos y que funciona igual desde
                // la habitación de al lado que desde otro continente.
                bool tsReady = false;
                if (g_phoneRoute == 0) {
                    std::string tsIp = g_tailscale.selfIp();
                    tsReady = !tsIp.empty();

                    const std::vector<Tailscale::Requirement>& trs = g_tailscale.requirements();
                    for (size_t i = 0; i < trs.size(); i++) {
                        Color c = trs[i].ok ? (Color){150, 235, 170, 255} : (Color){225, 150, 150, 255};
                        DrawText(trs[i].ok ? "OK " : "-- ", px, py, 10, c);
                        DrawText(trs[i].what.substr(0, 56).c_str(), px + 22, py, 10, c);
                        py += 13;
                        if (!trs[i].ok) {
                            DrawText(trs[i].fix.substr(0, 58).c_str(), px + 22, py, 9, (Color){160, 160, 185, 255});
                            py += 12;
                            if (UIButton({(float)(px + 22), (float)py, trs[i].isLink ? 88.0f : 110.0f, 18},
                                         trs[i].isLink ? "Open the page" : "Copy command", 9)) {
                                if (trs[i].isLink) OpenURL(trs[i].fix.c_str());
                                else SetClipboardText(trs[i].fix.c_str());
                                SetStatus(trs[i].isLink ? "Opening tailscale.com..." : "Command copied - paste it in a terminal");
                            }
                            py += 22;
                        }
                    }
                    py += 4;

                    // La dirección de ESTE PC en el tailnet: lo único que hay que
                    // llevar al móvil, y con un botón para copiarla al chat.
                    DrawText("This computer in your tailnet:", px, py, 11, (Color){180, 180, 200, 255});
                    DrawText(tsReady ? ShownIp(tsIp) : "(not connected)", px + 168, py, 13,
                             tsReady ? (Color){235, 210, 130, 255} : (Color){150, 150, 170, 255});
                    py += 18;
                    if (tsReady && UIButton({(float)px, (float)py, 96, 20}, "Copy address", 10)) {
                        SetClipboardText(tsIp.c_str());
                        SetStatus("Tailnet address copied");
                    }
                    if (UIButton({(float)(px + 102), (float)py, 78, 20}, g_tailscale.refreshing() ? "checking" : "Refresh", 10))
                        g_tailscale.refreshAsync();
                    {
                        int online = 0;
                        std::vector<Tailscale::Peer> ps = g_tailscale.peers();
                        for (const Tailscale::Peer& p : ps) if (p.online) online++;
                        DrawText(TextFormat("%d device(s), %d online", (int)ps.size(), online),
                                 px + 186, py + 5, 10, (Color){150, 150, 175, 255});
                    }
                    py += 26;
                }

                {
                    bool conn = g_phone.connected();
                    bool looking = g_phone.phoneIsLooking();
                    // La dirección que hay que teclear/abrir en el móvil: la del
                    // tailnet si vamos por ahí, la del Wi-Fi si no.
                    std::vector<std::string> lanIps = PhoneLink::localIPv4();
                    std::string shareIp = (g_phoneRoute == 0) ? g_tailscale.selfIp()
                                                              : (lanIps.empty() ? std::string() : lanIps[0]);

                    DrawText("1. Get the app onto the phone:", px, py, 12, (Color){200, 200, 215, 255}); py += 18;
                    bool serving = g_apkServer.isRunning();
                    if (UIButton({(float)px, (float)py, 150, 24}, serving ? "Stop sharing app" : "Share the app", 12)) {
                        if (serving) g_apkServer.stop();
                        else if (!g_apkServer.start(kApkPort, kApkPath))
                            SetStatus("Could not open port %d - is another Pinguus running?", kApkPort);
                    }
                    if (serving) {
                        DrawText("open this in the phone's browser:", px + 160, py + 2, 10, (Color){150, 150, 175, 255});
                        // Sin dirección (Tailscale sin sesión, o sin red) no se
                        // pinta un "http://?:45814" que no lleva a ningún sitio.
                        if (shareIp.empty())
                            DrawText("(no address yet - see the checks above)", px + 160, py + 14, 11, (Color){200, 170, 140, 255});
                        else
                            DrawText(TextFormat("http://%s:%d", ShownIp(shareIp), kApkPort),
                                     px + 160, py + 14, 13, (Color){235, 210, 130, 255});
                        py += 30;
                        DrawText(TextFormat("downloaded %d time(s)", g_apkServer.downloads()), px, py, 10,
                                 g_apkServer.downloads() > 0 ? (Color){150, 235, 170, 255} : (Color){150, 150, 170, 255});
                        if (!shareIp.empty() && UIButton({(float)(px + 150), (float)py - 4, 78, 18}, "Copy link", 9)) {
                            SetClipboardText(TextFormat("http://%s:%d", shareIp.c_str(), kApkPort));
                            SetStatus("Link copied - paste it into the phone");
                        }
                        py += 18;
                    } else {
                        DrawText(g_phoneRoute == 0 ? "serves PinguusCam.apk over the tailnet:"
                                                   : "serves PinguusCam.apk over Wi-Fi:",
                                 px + 160, py + 2, 10, (Color){150, 150, 175, 255});
                        DrawText("no cable, no cloud, nothing to copy", px + 160, py + 14, 10, (Color){150, 150, 175, 255});
                        py += 32;
                    }

                    if (g_phoneRoute == 0) {
                        // Un tailnet no reparte broadcasts, así que el "SEARCH FOR
                        // PINGUUS" de la app no encuentra nada por ahí: se teclea la
                        // dirección una vez y queda guardada en el móvil.
                        DrawText("2. In the app: \"Type the address instead\" ->", px, py, 12, (Color){200, 200, 215, 255});
                        py += 16;
                        DrawText(shareIp.empty()
                                     ? "   (the address appears here once Tailscale is up)"
                                     : TextFormat("   %s   (search-by-broadcast does not cross a tailnet)", ShownIp(shareIp)),
                                 px, py, 10, (Color){150, 150, 175, 255});
                        py += 14;
                        DrawText("   then CONNECT TO THIS ADDRESS - it checks the PC answers first.",
                                 px, py, 10, (Color){150, 150, 175, 255});
                        py += 20;
                    } else {
                        DrawText("2. Open the app and press SEARCH FOR PINGUUS.", px, py, 12, (Color){200, 200, 215, 255}); py += 16;
                        DrawText("It finds this PC on its own - no address to type.", px, py, 10, (Color){150, 150, 175, 255}); py += 22;
                    }

                    // Estado del enlace: escuchando / un móvil preguntando / conectado.
                    const char* stxt = conn ? TextFormat("CONNECTED - %.0f fps", g_phone.framesPerSec())
                                            : (looking ? "a phone is looking for this PC..."
                                                       : (g_phone.isRunning() ? TextFormat("listening on port %d", kPhonePort)
                                                                              : "link is off"));
                    Color scol = conn ? (Color){150, 235, 170, 255}
                                      : (looking ? (Color){235, 210, 130, 255} : (Color){150, 150, 170, 255});
                    DrawRectangle(px, py, 12, 12, scol);
                    DrawText(stxt, px + 18, py, 12, scol);
                    py += 20;
                    if (!g_phone.isRunning()) {
                        if (UIButton({(float)px, (float)py, 110, 22}, "Turn link on", 11)) g_phone.start(kPhonePort);
                        py += 26;
                    }

                    // ---------- Qué móvil se usa ----------
                    // Con uno solo no hay nada que elegir y la lista sobra. En
                    // cuanto hay dos hace falta: cada uno manda su propio vídeo
                    // y hay que decir cuál se ve y cuál graba el PC.
                    {
                        std::vector<PhoneLink::Device> pds = g_phone.devices();
                        uint64_t sel = g_phone.selected();
                        if (pds.size() > 1) {
                            DrawText(TextFormat("%d phones connected - pick the camera:", (int)pds.size()),
                                     px, py, 11, (Color){200, 200, 215, 255});
                            py += 16;
                            for (size_t i = 0; i < pds.size() && i < 4; i++) {
                                Rectangle rb = {(float)px, (float)py, 300, 20};
                                bool isSel = (pds[i].key == sel);
                                bool hov = CheckCollisionPointRec(m, rb);
                                DrawRectangleRec(rb, isSel ? (Color){50, 80, 60, 255}
                                                           : (hov ? (Color){48, 50, 66, 255} : (Color){32, 34, 48, 255}));
                                DrawRectangleLinesEx(rb, isSel ? 2.0f : 1.0f,
                                                     isSel ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                                // El NOMBRE del móvil, no su dirección: es lo que
                                // el usuario reconoce, y además una dirección no
                                // tiene por qué acabar en pantalla.
                                DrawText(TextFormat("%s   %.0f fps%s", pds[i].name.c_str(), pds[i].fps,
                                                    pds[i].streaming ? "" : "  (quiet)"),
                                         px + 6, (int)py + 4, 11,
                                         isSel ? (Color){170, 235, 190, 255}
                                               : (pds[i].streaming ? RAYWHITE : (Color){150, 150, 170, 255}));
                                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                    g_phone.select(pds[i].key);
                                    SetStatus("Camera: %s", pds[i].name.c_str());
                                }
                                py += 22;
                            }
                            py += 2;
                        }

                        // Dejar que el móvil grabe solo. Por defecto sí — es la
                        // gracia de la función — pero por el tailnet ese móvil
                        // puede estar en cualquier parte, así que se puede cerrar.
                        Rectangle ab = {(float)px, (float)py, 16, 16};
                        DrawRectangleRec(ab, allowPhoneRec ? (Color){120, 210, 150, 255} : (Color){40, 42, 56, 255});
                        DrawRectangleLinesEx(ab, 1, (Color){110, 112, 140, 255});
                        DrawText("let the phone record into slots by itself", px + 22, (int)py + 3, 10,
                                 (Color){180, 180, 200, 255});
                        if (CheckCollisionPointRec(m, ab) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            allowPhoneRec = !allowPhoneRec;
                            saveControls();
                            SetStatus(allowPhoneRec ? "Phones may record on their own"
                                                    : "Phones can no longer record on their own");
                        }
                        py += 22;
                    }

                    // Vista previa en vivo (de cualquiera de las rutas).
                    std::vector<uint8_t> jpg; uint64_t v = 0;
                    bool got = g_phone.latestFrame(jpg, v);
                    if (!got) got = g_ipcam.latestFrame(jpg, v);
                    if (got && v != phonePrevVer) {
                        phonePrevVer = v;
                        Image im = LoadImageFromMemory(".jpg", jpg.data(), (int)jpg.size());
                        if (im.data) { UpdatePreviewTex(phonePrevTex, im); UnloadImage(im); }
                    }
                    if (phonePrevTex.id && (conn || g_ipcam.connected())) {
                        float pw = (float)phonePrevTex.width, ph = (float)phonePrevTex.height;
                        float sc = fminf(300.0f / pw, 190.0f / ph);
                        Rectangle dst = {(float)px, (float)py, pw * sc, ph * sc};
                        DrawTexturePro(phonePrevTex, {0, 0, pw, ph}, dst, {0, 0}, 0.0f, WHITE);
                        DrawRectangleLinesEx(dst, 1, (Color){120, 210, 150, 255});
                        py += (int)dst.height + 8;
                    } else {
                        DrawRectangleLines(px, py, 300, 100, (Color){70, 72, 96, 255});
                        DrawText("(live preview appears here)", px + 70, py + 44, 11, (Color){110, 112, 140, 255});
                        py += 108;
                    }
                    DrawText("Then: click an empty CLIP/SMP slot -> \"Record from PHONE\".",
                             px, py, 11, (Color){170, 170, 190, 255});
                }

                // ---------- 2) any MJPEG camera app ----------
                cxr = (int)panel.x + 750;
                cyr = (int)panel.y + 56;
                DrawText("OTHER CAMERA APPS", cxr, cyr, 15, (Color){150, 200, 255, 255}); cyr += 22;
                DrawText("Any camera app that serves MJPEG (\"IP Webcam\" and", cxr, cyr, 10, (Color){150, 150, 175, 255}); cyr += 12;
                DrawText("friends). Free, but they show ads.", cxr, cyr, 10, (Color){150, 150, 175, 255}); cyr += 18;

                bool camConn = g_ipcam.connected();
                if (g_ipcam.scanning()) {
                    DrawText(TextFormat("looking for a camera app... %d%%", g_ipcam.scanProgress()), cxr, cyr + 5, 12, (Color){235, 210, 130, 255});
                } else {
                    // Por Wi-Fi hay que barrer un /24 entero a ciegas. Por el
                    // tailnet no: Tailscale ya sabe qué dispositivos hay, así que
                    // se pregunta a esos cuatro y basta — y valen aunque el móvil
                    // esté en otro país.
                    bool viaTs = (g_phoneRoute == 0);
                    if (UIButton({(float)cxr, (float)cyr, 110, 24}, viaTs ? "Scan tailnet" : "Scan network", 12)) {
                        if (viaTs) {
                            std::vector<std::string> hosts;
                            for (const Tailscale::Peer& p : g_tailscale.cameraCandidates())
                                if (p.online) hosts.push_back(p.ip);
                            if (hosts.empty()) SetStatus("No other device is online in your tailnet");
                            else {
                                g_ipcam.scanHosts(hosts, g_ipcamPort);
                                SetStatus("Asking your %d tailnet device(s) for a camera app...", (int)hosts.size());
                            }
                        } else {
                            g_ipcam.scanLan(PhoneLink::localIPv4(), g_ipcamPort);
                            SetStatus("Scanning the local network for a phone camera app...");
                        }
                    }
                    std::string hh = g_ipcam.host();
                    // Reconectar al último teléfono recordado (controls.cfg) sin re-escanear.
                    if (!hh.empty() && !g_ipcam.previewRunning()) {
                        if (UIButton({(float)(cxr + 116), (float)cyr, 74, 24}, "Connect", 11)) {
                            g_ipcam.startPreview();
                            SetStatus("Connecting to the saved phone camera...");
                        }
                        DrawText(TextFormat("saved: %s", ShownIp(hh)), cxr + 196, cyr + 6, 10, (Color){170, 170, 190, 255});
                    } else {
                        DrawText(camConn ? TextFormat("connected: %s", ShownIp(hh))
                                         : (hh.empty() ? "no device selected" : TextFormat("%s (no signal)", ShownIp(hh))),
                                 cxr + 120, cyr + 6, 11, camConn ? (Color){150, 235, 170, 255} : (Color){170, 170, 190, 255});
                    }
                }
                cyr += 28;

                // Found devices — click one to use it.
                {
                    std::vector<IpCamLink::Found> fnd = g_ipcam.foundDevices();
                    if (fnd.empty() && !g_ipcam.scanning()) {
                        DrawText("(no camera app found yet)", cxr + 4, cyr, 10, (Color){150, 150, 170, 255});
                        cyr += 16;
                    }
                    int listY = cyr;
                    for (size_t i = 0; i < fnd.size() && i < 4; i++) {
                        Rectangle r = {(float)cxr, (float)cyr, 196, 18};
                        bool sel = (g_ipcam.host() == fnd[i].ip);
                        bool hov = CheckCollisionPointRec(m, r);
                        DrawRectangleRec(r, sel ? (Color){50, 80, 60, 255} : (hov ? (Color){48, 50, 66, 255} : (Color){32, 34, 48, 255}));
                        DrawRectangleLinesEx(r, 1, sel ? (Color){120, 210, 150, 255} : (Color){70, 72, 96, 255});
                        // Un dispositivo del tailnet se enseña por su NOMBRE, que
                        // además de no ser una IP es lo que el usuario reconoce.
                        std::string label = fnd[i].ip;
                        for (const Tailscale::Peer& p : g_tailscale.peers())
                            if (p.ip == fnd[i].ip) { label = p.name; break; }
                        bool named = (label != fnd[i].ip);
                        DrawText(TextFormat("%s  %dx%d", named ? label.c_str() : ShownIp(fnd[i].ip), fnd[i].w, fnd[i].h),
                                 cxr + 6, cyr + 3, 11, sel ? (Color){170, 235, 190, 255} : RAYWHITE);
                        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            g_ipcam.stopPreview();
                            g_ipcam.setHost(fnd[i].ip, g_ipcamPort);
                            g_ipcam.startPreview();
                            saveControls();
                            SetStatus("Phone camera: %s", named ? label.c_str() : "connected");
                        }
                        cyr += 20;
                    }
                    (void)listY;   // la vista previa vive ahora en la columna 2
                }
            } // devTab == 1 (phone / camera / mic)

            // ---------- GAMEPAD ----------
            if (devTab == 0) {
            int gx = (int)panel.x + 560, gy = (int)panel.y + 56;
            DrawText("GAMEPAD (any controller)", gx, gy, 15, (Color){150, 235, 170, 255}); gy += 22;
            if (gpIndex < 0) {
                DrawText("No controller detected. Connect any USB/Bluetooth gamepad.", gx, gy, 12, (Color){200, 180, 140, 255}); gy += 17;
                DrawText("Some pads (e.g. 8BitDo) need X-INPUT mode to be seen as a gamepad.", gx, gy, 12, (Color){180, 180, 200, 255}); gy += 20;
            } else {
                std::string down;
                for (int b = 1; b < 18; b++) if (IsGamepadButtonDown(gpIndex, b)) { down += GpButtonName(b); down += " "; }
                DrawText(TextFormat("#%d %s   down: %s", gpIndex, GetGamepadName(gpIndex),
                                    down.empty() ? "(press some)" : down.c_str()),
                         gx, gy, 12, (Color){170, 235, 190, 255});
                gy += 20;
            }

            // Remapeo: cada acción -> su botón, con "Set" para capturar el próximo.
            DrawText("Remap (click Set, then press a button on the pad):", gx, gy, 12, (Color){150, 210, 235, 255}); gy += 20;
            for (int a = 0; a < GA_COUNT; a++) {
                DrawText(TextFormat("%s", kGpActionNames[a]), gx, gy + 1, 12, (Color){190, 190, 205, 255});
                DrawText(GpButtonName(gpMap[a]), gx + 170, gy + 1, 12, (Color){235, 220, 140, 255});
                Rectangle sb = {(float)(gx + 250), (float)gy - 2, 66, 18};
                bool waiting = (gpRemapAction == a);
                bool sh = CheckCollisionPointRec(m, sb);
                DrawRectangleRec(sb, waiting ? (Color){96, 74, 46, 255} : (sh ? (Color){50, 52, 70, 255} : (Color){40, 42, 56, 255}));
                DrawRectangleLinesEx(sb, 1, (Color){110, 112, 140, 255});
                DrawText(waiting ? "press.." : "Set", (int)sb.x + 8, gy + 1, 11, RAYWHITE);
                if (sh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) gpRemapAction = waiting ? -1 : a;
                gy += 20;
            }
            if (UIButton({(float)gx, (float)gy + 2, 120, 22}, "Reset defaults", 12)) {
                for (int a = 0; a < GA_COUNT; a++) gpMap[a] = kGpDefault[a];
                gpRemapAction = -1;
                saveControls();
                SetStatus("Controls reset to defaults");
            }
            DrawText("NOTEYS: L-stick move | R-stick pick slot/bar",
                     gx, gy + 32, 11, (Color){150, 150, 175, 255});
            DrawText("LB/RB note | A note B notey X erase Y play",
                     gx, gy + 46, 11, (Color){150, 150, 175, 255});
            } // devTab == 0 (gamepad)

            // ---------- THEME (colores de la UI) — fila inferior ----------
            if (devTab == 0) {
                static bool customDirty = false;
                int ty = (int)panel.y + panel.height - 38;
                DrawText("UI THEME:", (int)panel.x + 20, ty + 7, 14, (Color){235, 200, 120, 255});
                int txp = (int)panel.x + 100;
                for (int i = 0; i <= kThemeCount; i++) { // +1 = "Custom"
                    bool isCustom = (i == kThemeCount);
                    const UITheme& th = isCustom ? g_customTheme : kThemes[i];
                    Rectangle r = {(float)txp, (float)ty, 120, 28};
                    bool sel = (g_themeIdx == i);
                    bool hov = CheckCollisionPointRec(m, r);
                    DrawRectangleRec(r, th.button);
                    DrawRectangleLinesEx(r, sel ? 2.5f : 1.0f, sel ? th.accent : th.border);
                    DrawRectangle(txp + 5, (int)ty + 7, 13, 14, th.bg);
                    DrawRectangle(txp + 20, (int)ty + 7, 13, 14, th.accent);
                    DrawText(isCustom ? "Custom" : th.name, txp + 38, (int)ty + 8, 12, (hov || sel) ? RAYWHITE : (Color){205, 205, 220, 255});
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        g_themeIdx = i; ApplyTheme(); saveControls();
                        SetStatus("Theme: %s", isCustom ? "Custom" : th.name);
                    }
                    txp += 124;
                }

                // Editor del tema propio (solo si Custom está activo): 6 partes,
                // cada una con sliders R/G/B en la columna derecha del panel.
                if (g_themeIdx == kThemeCount) {
                    int ex = (int)panel.x + 470, ey = (int)panel.y + 360;
                    DrawText("CUSTOM COLORS (drag R/G/B):", ex, ey, 14, (Color){235, 200, 120, 255});
                    // Vuelta atrás: deshacer un tema propio a mano es imposible
                    // cuando te has pasado y ya no distingues los controles.
                    if (UIButton({(float)(ex + 230), (float)ey - 4, 100, 20}, "Reset colors", 11)) {
                        g_customTheme = kCustomDefault;
                        ApplyTheme(); saveControls();
                        SetStatus("Custom theme colors reset");
                    }
                    ey += 24;
                    const char* chLbl[3] = {"R", "G", "B"};
                    const Color chCol[3] = {{200, 90, 90, 255}, {90, 200, 90, 255}, {90, 130, 220, 255}};
                    for (int p = 0; p < 6; p++) {
                        Color* c = ThemePart(g_customTheme, p);
                        DrawText(kThemePartNames[p], ex, ey + 2, 12, (Color){190, 190, 205, 255});
                        DrawRectangle(ex + 92, ey, 16, 16, *c);
                        DrawRectangleLines(ex + 92, ey, 16, 16, (Color){120, 120, 140, 255});
                        for (int ch = 0; ch < 3; ch++) {
                            unsigned char* comp = (ch == 0) ? &c->r : (ch == 1) ? &c->g : &c->b;
                            Rectangle sr = {(float)(ex + 116 + ch * 66), (float)ey, 60, 16};
                            DrawRectangleRec(sr, (Color){28, 30, 42, 255});
                            DrawRectangle((int)sr.x, (int)sr.y, (int)(sr.width * (*comp / 255.0f)), (int)sr.height, chCol[ch]);
                            DrawRectangleLinesEx(sr, 1, (Color){80, 82, 105, 255});
                            DrawText(TextFormat("%s%d", chLbl[ch], *comp), (int)sr.x + 3, (int)sr.y + 3, 9, RAYWHITE);
                            if (CheckCollisionPointRec(m, sr) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                                float t = (m.x - sr.x) / sr.width; t = t < 0 ? 0 : (t > 1 ? 1 : t);
                                *comp = (unsigned char)(t * 255.0f);
                                c->a = 255; ApplyTheme(); customDirty = true;
                            }
                        }
                        ey += 22;
                    }
                    DrawText("(saved automatically)", ex, ey + 2, 11, (Color){150, 150, 170, 255});
                    if (customDirty && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { saveControls(); customDirty = false; }
                }
            }
        }

        // --- Panel MODS (cargar/recargar scripts .lua) ---
        if (showMods) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {240, 80, 800, 560};
            DrawRectangleRec(panel, (Color){24, 25, 38, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            DrawText("MODS - Lua scripts", (int)panel.x + 20, (int)panel.y + 14, 20, RAYWHITE);
            if (UIButton({panel.x + panel.width - 96, panel.y + 12, 80, 26}, "Close", 13) || IsKeyPressed(KEY_ESCAPE))
                showMods = false;

            if (UIButton({panel.x + 20, panel.y + 50, 130, 26}, "Load .lua", 13)) {
                char path[512] = "";
                if (NativeOpenDialog("Load a Lua mod (.lua)...", "*.lua", path, sizeof(path)) == 1) {
                    std::string e;
                    if (g_scripts.loadFile(path, e)) SetStatus("Mod loaded: %s", GetFileName(path));
                    else SetStatus("Mod error: %s", e.c_str());
                }
            }
            if (UIButton({panel.x + 160, panel.y + 50, 130, 26}, "Reload all", 13)) {
                g_modCells.clear();          // se re-registran al recargar
                std::string e;
                if (g_scripts.reloadAll(e)) SetStatus("Reloaded %d mod(s)", (int)g_scripts.list().size());
                else SetStatus("Reload error: %s", e.c_str());
            }
            DrawText(TextFormat("Loaded: %d", (int)g_scripts.list().size()), (int)panel.x + 306, (int)panel.y + 56, 14, (Color){150, 210, 160, 255});

            int ly = (int)panel.y + 88;
            if (g_scripts.list().empty()) {
                DrawText("No mods loaded. Put .lua files in a 'mods/' folder (auto-loaded", (int)panel.x + 20, ly, 12, (Color){170, 170, 190, 255});
                DrawText("at startup) or click Load .lua. Examples are in mods/.", (int)panel.x + 20, ly + 16, 12, (Color){170, 170, 190, 255});
                ly += 40;
            } else {
                std::string names;
                for (const auto& md : g_scripts.list()) { names += md.name; names += "  "; }
                DrawText(TextFormat("Loaded: %s", names.c_str()), (int)panel.x + 20, ly, 12, RAYWHITE);
                ly += 22;
            }

            // Fase 2: celdas personalizadas registradas — clic para pintarlas.
            DrawText("Custom cells (click to select, then paint on the CANVAS):", (int)panel.x + 20, ly, 13, (Color){235, 200, 120, 255});
            ly += 20;
            if (g_modCells.empty()) {
                DrawText("(none - a mod calls pinguus.register_cell{...} to add them)", (int)panel.x + 24, ly, 12, (Color){150, 150, 170, 255});
                ly += 20;
            } else {
                int cxp = (int)panel.x + 24;
                for (int i = 0; i < (int)g_modCells.size(); i++) {
                    const ModCellDef& d = g_modCells[i];
                    Rectangle cb = {(float)cxp, (float)ly, 118, 24};
                    bool sel = (selectedTool == TOOL_MODCELL && selectedModCell == i);
                    bool hov = CheckCollisionPointRec(GetMousePosition(), cb);
                    DrawRectangleRec(cb, sel ? (Color){70, 90, 110, 255} : (hov ? (Color){50, 52, 70, 255} : (Color){38, 40, 54, 255}));
                    DrawRectangleLinesEx(cb, sel ? 2.0f : 1.0f, (Color){d.r, d.g, d.b, 255});
                    DrawText(d.glyph.c_str(), cxp + 6, ly + 5, 13, (Color){d.r, d.g, d.b, 255});
                    DrawText(d.name.c_str(), cxp + 28, ly + 6, 12, RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        selectedTool = TOOL_MODCELL; selectedModCell = i;
                        SetStatus("Cell '%s' selected - paint on the canvas", d.name.c_str());
                    }
                    cxp += 126;
                    if (cxp + 118 > (int)(panel.x + panel.width - 20)) { cxp = (int)panel.x + 24; ly += 28; }
                }
                if (cxp != (int)panel.x + 24) ly += 28;
                ly += 4;
            }

            // Mini-referencia de la API para el usuario.
            DrawText("API (main-thread, reactive):", (int)panel.x + 20, ly, 14, (Color){150, 210, 235, 255}); ly += 22;
            const char* apiLines[] = {
                "pinguus.on_update(function(dt) ... end)   -- called every frame",
                "pinguus.spawn(x,y,dir,slot,speed)  dir 0=R 1=D 2=L 3=U",
                "pinguus.paint(x,y,semitone,slot)   pinguus.erase(x,y)",
                "pinguus.play(slot,semitone)        -- live note (audio+video)",
                "pinguus.linear_set(col,row,slot,semitone)  pinguus.linear_clear()",
                "pinguus.clear_noteys()   pinguus.set_bpm(v)   pinguus.status(s)",
                "reads: grid_w() grid_h() bpm() playing() playhead_linear()",
                "       notey_count()  notey(i) -> {x,y,slot,playing}",
                "cells: register_cell{name=,glyph=,behavior=,...} or actions={{...},..}",
            };
            for (auto* s : apiLines) { DrawText(s, (int)panel.x + 28, ly, 12, (Color){175, 178, 200, 255}); ly += 18; }
            DrawText("Tip: edit a .lua and press 'Reload all' to see changes live.",
                     (int)panel.x + 20, (int)panel.y + panel.height - 30, 12, (Color){150, 150, 175, 255});
        }

        // --- Panel del efecto de vídeo analógico (NTSC / VHS) ---
        // Actúa sobre la MEZCLA FINAL, así que vive aquí y no en el editor de un
        // clip: no es un efecto de una capa, es cómo sale la señal entera.
        if (showNtsc) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {230, 60, 820, 600};
            DrawRectangleRec(panel, (Color){24, 25, 38, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            DrawText("ANALOG VIDEO (NTSC / VHS) - applied to the final mix",
                     (int)panel.x + 20, (int)panel.y + 14, 19, RAYWHITE);
            if (UIButton({panel.x + panel.width - 96, panel.y + 12, 80, 26}, "Close", 13) || IsKeyPressed(KEY_ESCAPE))
                showNtsc = false;

            Vector2 m = GetMousePosition();
            float y = panel.y + 50;

            // Interruptor grande: lo primero que se busca al abrir esto.
            {
                Rectangle ob = {panel.x + 20, y, 120, 30};
                DrawRectangleRec(ob, g_ntsc.enabled ? (Color){160, 70, 60, 255} : (Color){44, 46, 62, 255});
                DrawRectangleLinesEx(ob, g_ntsc.enabled ? 2.0f : 1.0f,
                                     g_ntsc.enabled ? RAYWHITE : (Color){110, 112, 140, 255});
                DrawText(g_ntsc.enabled ? "EFFECT: ON" : "EFFECT: OFF",
                         (int)ob.x + 14, (int)ob.y + 9, 13, RAYWHITE);
                if (CheckCollisionPointRec(m, ob) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    g_ntsc.enabled = !g_ntsc.enabled;
                    if (g_ntsc.enabled && !g_ntscShader.ok && !g_ntscShader.load()) {
                        g_ntsc.enabled = false;
                        SetStatus("The analog video shader failed to compile (see console)");
                    }
                }
            }
            if (UIButton({panel.x + 152, y, 74, 30}, "VHS", 13)) { g_ntsc.presetVHS(); g_ntsc.enabled = true; }
            if (UIButton({panel.x + 232, y, 100, 30}, "BROADCAST", 12)) { g_ntsc.presetBroadcast(); g_ntsc.enabled = true; }
            if (UIButton({panel.x + 338, y, 90, 30}, "RUINED", 13)) { g_ntsc.presetRuined(); g_ntsc.enabled = true; }
            if (UIButton({panel.x + 434, y, 76, 30}, "RESET", 13)) { bool on = g_ntsc.enabled; g_ntsc = NtscFX(); g_ntsc.enabled = on; }

            // Cargar un preset .json de ntsc-rs.
            if (UIButton({panel.x + 540, y, 170, 30}, "Load ntsc-rs .json...", 12)) {
                char path[512] = "";
                if (NativeOpenDialog("Load an ntsc-rs preset (.json)...", "*.json", path, sizeof(path)) == 1) {
                    NtscPresetResult r = LoadNtscPreset(path, g_ntsc);
                    if (!r.ok) {
                        SetStatus("Preset not loaded: %s", r.note.c_str());
                    } else {
                        g_ntsc.enabled = true;
                        if (!g_ntscShader.ok) g_ntscShader.load();
                        SetStatus("Preset %s: %d settings translated, %d had no equivalent here",
                                  GetFileName(path), r.applied, r.ignored);
                    }
                }
            }
            y += 40;

            DrawText("Presets from ntsc-rs are TRANSLATED, not emulated: this is a different implementation,",
                     (int)panel.x + 20, (int)y, 12, (Color){170, 145, 105, 255});
            DrawText("so a loaded preset lands in the same look, not on the exact same pixels.",
                     (int)panel.x + 20, (int)y + 15, 12, (Color){170, 145, 105, 255});
            y += 40;

            // Los deslizadores, en dos columnas.
            struct NSlider { const char* name; float* v; const char* help; };
            NSlider sliders[] = {
                {"Composite noise", &g_ntsc.noise,        "grain over the whole signal"},
                {"Luma noise",      &g_ntsc.lumaNoise,    "grain in the brightness only"},
                {"Chroma noise",    &g_ntsc.chromaNoise,  "grain in the colour only"},
                {"Snow",            &g_ntsc.snow,         "isolated white specks"},
                {"Chroma delay",    &g_ntsc.chromaDelay,  "colour smears sideways off the edges"},
                {"Chroma phase",    &g_ntsc.phaseNoise,   "hue wobbles line to line"},
                {"Ringing",         &g_ntsc.ringing,      "echo/halo next to vertical edges"},
                {"Sharpening",      &g_ntsc.sharpen,      "composite preemphasis"},
                {"Edge wave",       &g_ntsc.edgeWave,     "the picture wobbles like a loose tape"},
                {"Wave speed",      &g_ntsc.edgeWaveSpeed,"how fast that wobble moves"},
                {"Head switch",     &g_ntsc.headSwitch,   "torn band at the very bottom"},
                {"Chroma loss",     &g_ntsc.chromaLoss,   "some lines lose colour entirely"},
                {"Scanlines",       &g_ntsc.scanlines,    "darker every other line"},
                {"Tape speed blur", &g_ntsc.tapeBlur,     "SP is sharp, EP is mushy"},
            };
            const int nSliders = (int)(sizeof(sliders) / sizeof(sliders[0]));
            const float colW = (panel.width - 60) / 2.0f;
            for (int i = 0; i < nSliders; i++) {
                int col = i % 2, row = i / 2;
                float sx = panel.x + 20 + col * (colW + 20);
                float sy = y + row * 34;
                DrawText(sliders[i].name, (int)sx, (int)sy + 3, 12, (Color){200, 200, 220, 255});
                Rectangle bar = {sx + 118, sy, colW - 178, 18};
                DrawRectangleRec(bar, (Color){36, 38, 52, 255});
                DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * (*sliders[i].v)), (int)bar.height,
                              g_ntsc.enabled ? (Color){150, 90, 80, 255} : (Color){70, 72, 96, 255});
                DrawRectangleLinesEx(bar, 1, (Color){90, 92, 118, 255});
                DrawText(TextFormat("%3d%%", (int)(*sliders[i].v * 100.0f + 0.5f)),
                         (int)(bar.x + bar.width + 8), (int)sy + 3, 12, (Color){225, 200, 120, 255});
                if (CheckCollisionPointRec(m, bar) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float t = (m.x - bar.x) / bar.width;
                    *sliders[i].v = t < 0 ? 0 : (t > 1 ? 1 : t);
                }
                if (CheckCollisionPointRec(m, bar))
                    DrawText(sliders[i].help, (int)panel.x + 20,
                             (int)panel.y + panel.height - 46, 12, (Color){150, 200, 235, 255});
            }
            y += ((nSliders + 1) / 2) * 34 + 10;

            DrawText("The effect runs on the FINAL mix - what you see in the preview on the right is exactly",
                     (int)panel.x + 20, (int)y, 12, (Color){150, 150, 175, 255});
            DrawText("what gets recorded and what the LIVE window shows. It is saved with the project.",
                     (int)panel.x + 20, (int)y + 15, 12, (Color){150, 150, 175, 255});
        }


        // --- Editor del BANCO DE MELODÍAS (la celda MELODY de la paleta) ---
        //
        // Aquí se fabrican las ocho melodías: se escucha un sonido, salen las
        // notas, y quedan guardadas listas para estamparlas en el lienzo con la
        // celda MELODY — igual que el arpegio estampa su acorde.
        if (showMelody) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {180, 40, 920, 640};
            DrawRectangleRec(panel, (Color){24, 25, 38, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            DrawText("MELODY BANK - sing something, place it like a chord",
                     (int)panel.x + 20, (int)panel.y + 14, 19, RAYWHITE);
            if (UIButton({panel.x + panel.width - 96, panel.y + 12, 80, 26}, "Close", 13) || IsKeyPressed(KEY_ESCAPE))
                showMelody = false;

            Vector2 m = GetMousePosition();
            float y = panel.y + 48;

            // ---- Las ocho ranuras ----
            {
                const float bw = (panel.width - 40) / (float)MELODY_BANK_SIZE;
                for (int i = 0; i < MELODY_BANK_SIZE; i++) {
                    Rectangle r = {panel.x + 20 + i * bw, y, bw - 6, 44};
                    const bool sel = (melIdx == i);
                    const MelodyClip& mc = g_melodies[i];
                    DrawRectangleRec(r, sel ? (Color){60, 80, 110, 255}
                                            : (mc.empty() ? (Color){34, 36, 50, 255} : (Color){44, 52, 70, 255}));
                    DrawRectangleLinesEx(r, sel ? 3.0f : 1.0f, sel ? RAYWHITE : (Color){90, 92, 118, 255});
                    DrawText(TextFormat("%d", i + 1), (int)r.x + 6, (int)r.y + 4, 13,
                             sel ? RAYWHITE : (Color){170, 172, 200, 255});
                    if (mc.empty()) {
                        DrawText("empty", (int)r.x + 6, (int)r.y + 24, 11, (Color){110, 110, 135, 255});
                    } else {
                        DrawText(TextFormat("%d notes", (int)mc.notes.size()), (int)r.x + 6, (int)r.y + 22, 11,
                                 (Color){150, 220, 255, 255});
                        // Miniatura de la forma de la melodía: sube, baja o se
                        // queda plana, que es lo que la distingue de un vistazo.
                        int lo, hi; mc.range(lo, hi);
                        if (hi <= lo) hi = lo + 1;
                        const int w = mc.widthSteps();
                        for (const auto& n : mc.notes) {
                            float nx = r.x + 6 + (n.step / (float)(w > 0 ? w : 1)) * (r.width - 14);
                            float ny = r.y + 40 - ((n.semitone - lo) / (float)(hi - lo)) * 10.0f;
                            DrawRectangle((int)nx, (int)ny, 2, 2, (Color){190, 210, 255, 255});
                        }
                    }
                    if (CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        melIdx = i;
                        melAnalysed = false;
                        melNote.clear();
                    }
                }
            }
            y += 52;
            DrawText(TextFormat("Editing melody %d. Pick it in the palette (MEL) and click the canvas to stamp it.",
                                melIdx + 1),
                     (int)panel.x + 20, (int)y, 12, (Color){180, 180, 205, 255});
            y += 22;

            // ---- De dónde sale el sonido ----
            const InstrumentSource& msrc = g_engine.getInstrumentBank().at(melSrcSlot);
            const bool hasAudio = msrc.audio.pPCM != nullptr && msrc.audio.totalFrames > 0;

            DrawText("SOURCE", (int)panel.x + 20, (int)y + 7, 12, (Color){180, 180, 205, 255});
            if (UIButton({panel.x + 80, y, 24, 26}, "<", 13)) {
                melSrcSlot--; if (melSrcSlot < 0) melSrcSlot = MAX_SLOTS - 1;
                melAnalysed = false;
            }
            DrawText(slotLabel(melSrcSlot), (int)panel.x + 114, (int)y + 7, 14,
                     hasAudio ? (Color){235, 200, 120, 255} : (Color){130, 130, 155, 255});
            if (UIButton({panel.x + 156, y, 24, 26}, ">", 13)) {
                melSrcSlot++; if (melSrcSlot >= MAX_SLOTS) melSrcSlot = 0;
                melAnalysed = false;
            }
            if (UIButton({panel.x + 190, y, 96, 26}, "Use selected", 11)) {
                melSrcSlot = activeSlot();
                if (isModelSlot(melSrcSlot)) melSrcSlot = SAMPLE_BASE;
                melAnalysed = false;
            }
            DrawText(hasAudio ? TextFormat("%.2f s of audio", (double)msrc.audio.totalFrames / 44100.0)
                              : "this slot has no audio",
                     (int)panel.x + 296, (int)y + 7, 12,
                     hasAudio ? (Color){150, 235, 170, 255} : (Color){235, 140, 140, 255});

            // Grabar del micrófono AQUÍ MISMO: es el camino que da sentido a
            // todo esto — tarareas y ya tienes la melodía en el banco.
            if (UIButton({panel.x + panel.width - 232, y, 210, 26},
                         TextFormat("Record %ds from MIC into %s", camRecSeconds, slotLabel(melSrcSlot)), 11)) {
                recordCamMic(melSrcSlot, false);
                melAnalysed = false;
            }
            y += 34;

            // ---- Ajustes del detector ----
            auto slider = [&](float sx, const char* label, float* v, float lo, float hi, const char* fmt) {
                DrawText(label, (int)sx, (int)y + 4, 12, (Color){180, 180, 205, 255});
                Rectangle bar = {sx + 92, y, 120, 18};
                DrawRectangleRec(bar, (Color){36, 38, 52, 255});
                DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * ((*v - lo) / (hi - lo))),
                              (int)bar.height, (Color){70, 110, 150, 255});
                DrawRectangleLinesEx(bar, 1, (Color){90, 92, 118, 255});
                DrawText(TextFormat(fmt, *v), (int)(bar.x + bar.width + 8), (int)y + 4, 12,
                         (Color){225, 200, 120, 255});
                if (CheckCollisionPointRec(m, bar) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float t = (m.x - bar.x) / bar.width;
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    *v = lo + t * (hi - lo);
                    melAnalysed = false;
                }
            };
            slider(panel.x + 20, "SENSITIVITY", &melOpt.silence, 0.001f, 0.08f, "%.3f");
            slider(panel.x + 470, "TOLERANCE", &melOpt.threshold, 0.08f, 0.40f, "%.2f");
            y += 26;
            slider(panel.x + 20, "MIN NOTE", &melOpt.minNoteSec, 0.03f, 0.40f, "%.2f s");
            slider(panel.x + 470, "SPLIT AT", &melOpt.newNoteSemis, 0.25f, 2.0f, "%.2f st");
            y += 26;
            DrawText("SENSITIVITY: how loud counts as sound | TOLERANCE: how forgiving the pitch detector is - raise it to catch more of a rough recording",
                     (int)panel.x + 20, (int)y, 11, (Color){130, 130, 155, 255});
            DrawText("MIN NOTE: shorter than this is a click | SPLIT AT: how far the pitch must move to start a NEW note (low values split a vibrato)",
                     (int)panel.x + 20, (int)y + 14, 11, (Color){130, 130, 155, 255});
            y += 32;

            // ---- Transporte, cuantización y ANALIZAR ----
            DrawText("TRANSPOSE", (int)panel.x + 20, (int)y + 6, 12, (Color){180, 180, 205, 255});
            if (UIButton({panel.x + 96, y, 24, 24}, "-", 13)) { melOpt.transpose -= 12; melAnalysed = false; }
            DrawText(TextFormat("%+d st", melOpt.transpose), (int)panel.x + 128, (int)y + 6, 12, (Color){225, 200, 120, 255});
            if (UIButton({panel.x + 184, y, 24, 24}, "+", 13)) { melOpt.transpose += 12; melAnalysed = false; }
            if (UIButton({panel.x + 214, y, 30, 24}, "0", 12)) { melOpt.transpose = 0; melAnalysed = false; }
            {
                Rectangle qb = {panel.x + 254, y, 96, 24};
                if (UIButton(qb, melQuantize ? "QUANT: on" : "QUANT: off", 11)) melQuantize = !melQuantize;
                if (melQuantize) DrawRectangleLinesEx(qb, 2, (Color){120, 230, 140, 255});
            }

            // CUÁNTO DURA UNA CASILLA. Es el mando que decide si una melodía
            // larga cabe en el lienzo o se sale por el borde.
            {
                static const char* kDivName[3] = {"1/16", "1/8", "1/4"};
                static const int   kDivVal[3]  = {4, 2, 1};
                DrawText("CELL", (int)panel.x + 360, (int)y + 6, 11, (Color){180, 180, 205, 255});
                for (int d = 0; d < 3; d++) {
                    Rectangle rb = {panel.x + 396.0f + d * 44.0f, y, 40, 24};
                    const bool on = (melDiv == kDivVal[d]);
                    if (UIButton(rb, kDivName[d], 11)) melDiv = kDivVal[d];
                    if (on) DrawRectangleLinesEx(rb, 2, (Color){120, 230, 140, 255});
                }
                // FIT elige la resolución más fina que TODAVÍA cabe: lo que se
                // quiere casi siempre es la melodía entera con el mayor detalle
                // posible, y a ojo eso son tres pruebas.
                if (melAnalysed && !melResult.notes.empty() &&
                    UIButton({panel.x + 532, y, 46, 24}, "FIT", 11)) {
                    melDiv = 1;
                    for (int d = 0; d < 3; d++) {
                        auto probe = PitchToNotes::ToSteps(melResult, bpmDisplay, kDivVal[d],
                                                           melQuantize, 0);
                        if (!probe.empty() && probe.back().step + 1 <= g_gridW) { melDiv = kDivVal[d]; break; }
                    }
                }
            }

            if (UIButton({panel.x + panel.width - 322, y, 140, 24}, "ANALYSE", 13)) {
                if (!hasAudio) {
                    melNote = "that slot has no audio - record from the mic or pick another slot";
                    melAnalysed = false;
                } else {
                    melResult = PitchToNotes::Analyse(msrc.audio.pPCM, (size_t)msrc.audio.totalFrames,
                                                      44100, melOpt);
                    melAnalysed = true;
                    melNote = melResult.notes.empty()
                        ? "no notes found - raise TOLERANCE, or lower SENSITIVITY if you sang quietly"
                        : TextFormat("%d notes found (%d%% of the sound had a clear pitch) - press SAVE to keep them",
                                     (int)melResult.notes.size(),
                                     melResult.framesAnalysed > 0
                                         ? melResult.framesVoiced * 100 / melResult.framesAnalysed : 0);
                }
            }
            // Guardar en la ranura elegida. El análisis NO se guarda solo: así
            // se puede probar con los mandos hasta que suene bien sin machacar
            // lo que ya había en la ranura a cada intento.
            if (melAnalysed && !melResult.notes.empty() &&
                UIButton({panel.x + panel.width - 174, y, 150, 24},
                         TextFormat("SAVE into %d", melIdx + 1), 12)) {
                auto st = PitchToNotes::ToSteps(melResult, bpmDisplay, melDiv, melQuantize, MAX_GW);
                g_melodies[melIdx].notes = st;
                g_melodies[melIdx].name = DefaultMelodyName(melIdx, (int)st.size());
                selectedTool = TOOL_MELODY;
                melNote = TextFormat("Saved into melody %d - pick MEL in the palette and click the canvas",
                                     melIdx + 1);
            }
            y += 30;

            if (!melNote.empty())
                DrawText(melNote.c_str(), (int)panel.x + 20, (int)y, 13,
                         (melAnalysed && melResult.notes.empty()) ? (Color){235, 180, 140, 255}
                                                                 : (Color){150, 235, 170, 255});
            // CUÁNTAS CASILLAS va a ocupar, frente a las que hay. "45 of 32"
            // explica de un vistazo por qué se corta una melodía larga, y es
            // justo lo que arregla el botón FIT.
            if (melAnalysed && !melResult.notes.empty()) {
                auto probe = PitchToNotes::ToSteps(melResult, bpmDisplay, melDiv, melQuantize, 0);
                const int need = probe.empty() ? 0 : probe.back().step + 1;
                const bool fits = need <= g_gridW;
                const char* txt = fits ? TextFormat("takes %d of the %d cells across", need, g_gridW)
                                       : TextFormat("takes %d cells but the grid is %d wide - press FIT",
                                                    need, g_gridW);
                DrawText(txt, (int)(panel.x + panel.width - 20 - MeasureText(txt, 12)), (int)y + 1, 12,
                         fits ? (Color){150, 235, 170, 255} : (Color){235, 140, 140, 255});
            }
            y += 22;

            // ---- Vista previa: lo analizado, o lo que ya hay guardado ----
            Rectangle roll = {panel.x + 20, y, panel.width - 40, 170};
            DrawRectangleRec(roll, (Color){18, 19, 28, 255});
            DrawRectangleLinesEx(roll, 1, (Color){70, 72, 96, 255});
            const MelodyClip& saved = g_melodies[melIdx];
            if (melAnalysed && !melResult.notes.empty()) {
                float tEnd = 0.0f;
                int loSemi = 127, hiSemi = -127;
                for (const auto& n : melResult.notes) {
                    if (n.startSec + n.lenSec > tEnd) tEnd = n.startSec + n.lenSec;
                    if (n.semitone < loSemi) loSemi = n.semitone;
                    if (n.semitone > hiSemi) hiSemi = n.semitone;
                }
                if (tEnd <= 0.0f) tEnd = 1.0f;
                if (hiSemi - loSemi < 12) { int c = (loSemi + hiSemi) / 2; loSemi = c - 6; hiSemi = c + 6; }
                const float rowH = roll.height / (float)(hiSemi - loSemi + 1);
                for (const auto& n : melResult.notes) {
                    float nx = roll.x + (n.startSec / tEnd) * roll.width;
                    float nw = (n.lenSec / tEnd) * roll.width;
                    if (nw < 2.0f) nw = 2.0f;
                    float ny = roll.y + (hiSemi - n.semitone) * rowH;
                    Color c = g_palette[((n.semitone % 12) + 12) % 12].displayColor;
                    c.a = (unsigned char)(120 + 135 * n.peak);
                    DrawRectangle((int)nx, (int)ny + 1, (int)nw, (int)rowH - 1, c);
                }
                const char* info = TextFormat("just analysed: %.1f s  |  range %s .. %s", tEnd,
                                              SemitoneName(loSemi), SemitoneName(hiSemi));
                int iw = MeasureText(info, 11);
                DrawRectangle((int)roll.x + 4, (int)(roll.y + roll.height - 18), iw + 8, 15, (Color){0, 0, 0, 200});
                DrawText(info, (int)roll.x + 8, (int)(roll.y + roll.height - 16), 11, (Color){190, 190, 215, 255});
            } else if (!saved.empty()) {
                // Lo guardado se dibuja en PASOS, que es como se va a estampar.
                int lo, hi; saved.range(lo, hi);
                if (hi - lo < 12) { int c = (lo + hi) / 2; lo = c - 6; hi = c + 6; }
                const int w = saved.widthSteps();
                const float colW = roll.width / (float)(w > 0 ? w : 1);
                const float rowH = roll.height / (float)(hi - lo + 1);
                for (const auto& n : saved.notes) {
                    float nx = roll.x + n.step * colW;
                    float nw = colW * n.lenSteps;
                    if (nw < 2.0f) nw = 2.0f;
                    float ny = roll.y + (hi - n.semitone) * rowH;
                    Color c = g_palette[((n.semitone % 12) + 12) % 12].displayColor;
                    c.a = (unsigned char)(120 + 135 * n.peak);
                    DrawRectangle((int)nx, (int)ny + 1, (int)nw, (int)rowH - 1, c);
                }
                const char* info = TextFormat("saved melody %d: %d notes over %d cells  |  range %s .. %s",
                                              melIdx + 1, (int)saved.notes.size(), w,
                                              SemitoneName(lo), SemitoneName(hi));
                int iw = MeasureText(info, 11);
                DrawRectangle((int)roll.x + 4, (int)(roll.y + roll.height - 18), iw + 8, 15, (Color){0, 0, 0, 200});
                DrawText(info, (int)roll.x + 8, (int)(roll.y + roll.height - 16), 11, (Color){190, 190, 215, 255});
            } else {
                const char* hint = melAnalysed ? "nothing detected"
                                               : "this slot is empty - pick a sound above and press ANALYSE";
                DrawText(hint, (int)(roll.x + (roll.width - MeasureText(hint, 14)) / 2),
                         (int)(roll.y + roll.height / 2 - 7), 14, (Color){110, 110, 135, 255});
            }
            y += roll.height + 12;

            // ---- Qué hacer con la melodía guardada ----
            const bool hasSaved = !saved.empty();
            if (hasSaved) {
                if (UIButton({panel.x + 20, y, 130, 28}, TextFormat("CLEAR %d", melIdx + 1), 12)) {
                    g_melodies[melIdx].clear();
                    melNote = TextFormat("Melody %d cleared", melIdx + 1);
                }
                // Los otros dos destinos siguen ahí: el lienzo es lo normal,
                // pero una melodía larga cabe mejor en el modo lineal.
                if (UIButton({panel.x + 160, y, 160, 28}, "Write to LINEAR", 12)) {
                    linearClearAll();
                    int lane = 0, maxCol = 0;
                    for (const auto& sn : saved.notes) {
                        if (sn.step >= LINEAR_COLS) break;
                        lane = (lane + 1) % (g_linearRows > 0 ? g_linearRows : 4);
                        linearPlace(sn.step, lane, activeSlot(), sn.semitone, 0);
                        if (sn.step > maxCol) maxCol = sn.step;
                    }
                    g_linearLength = maxCol + 1 < 4 ? 4 : maxCol + 1;
                    RequestLinearParams(g_linearLength, g_linearLoop);
                    activeView = 2;
                    showMelody = false;
                    SetStatus("Melody %d written to LINEAR", melIdx + 1);
                }
                if (UIButton({panel.x + 330, y, 170, 28}, "Write to TRACKER ch1", 12)) {
                    for (int rr = 0; rr < TRACKER_ROWS; rr++) {
                        g_tracker[0][rr] = TrkCell();
                        RequestTrackerCell(0, rr, -1, 0, 0);
                    }
                    for (const auto& sn : saved.notes) {
                        if (sn.step < 0 || sn.step >= TRACKER_ROWS) continue;
                        g_tracker[0][sn.step] = {activeSlot(), sn.semitone, 0};
                        RequestTrackerCell(0, sn.step, activeSlot(), SemitoneToColorId(sn.semitone), 0);
                    }
                    activeView = 1;
                    showMelody = false;
                    SetStatus("Melody %d written to TRACKER channel 1", melIdx + 1);
                }
            }

            DrawText("Sing, hum or whistle ONE note at a time - this follows a single melody, not chords.",
                     (int)panel.x + 20, (int)(panel.y + panel.height - 30), 12, (Color){170, 145, 105, 255});
        }

        // --- Editor de MODELO 3D (posición/rotación/escala + luz) ---
        if (modelEditorOpen && modelEditorSlot >= 0 && modelEditorSlot < MAX_MODELS && g_models[modelEditorSlot].loaded) {
            ModelSlot& s = g_models[modelEditorSlot];
            Vector2 mm = GetMousePosition();
            // Avanza la animación de preview: loopea el RANGO recortado para que
            // veas exactamente lo que se reproducirá.
            {
                int st = animTrimStartF(s, modelEditorAnim), en = animTrimEndF(s, modelEditorAnim);
                if (modelEditorFrame < (float)st) modelEditorFrame = (float)st;
                modelEditorFrame += 60.0f * dt;
                if (en > st && modelEditorFrame >= (float)en) modelEditorFrame = (float)st + fmodf(modelEditorFrame - (float)st, (float)(en - st));
            }
            // Render del modelo a la textura de preview.
            {
                BeginTextureMode(modelPreviewRT);
                ClearBackground((Color){28, 30, 40, 255});
                Camera3D pc = {0};
                pc.position = {0.0f, 1.1f, 4.2f}; pc.target = {0.0f, 0.9f, 0.0f};
                pc.up = {0.0f, 1.0f, 0.0f}; pc.fovy = 45.0f; pc.projection = CAMERA_PERSPECTIVE;
                SetLightUniforms(pc.position);
                BeginMode3D(pc);
                Vector3 pd, rd; float sd;
                PoseModel(s, modelEditorAnim, modelEditorFrame, pd, rd, sd);
                DrawModelSlotAt(s, 0.0f, pd, rd, sd);
                DrawGrid(8, 0.4f);
                EndMode3D();
                EndTextureMode();
            }

            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 190});
            Rectangle panel = {180, 60, 920, 600};
            DrawRectangleRec(panel, g_theme.panel);
            DrawRectangleLinesEx(panel, 2, g_theme.border);
            DrawText(TextFormat("MODEL %d - %s  (%d anims)", modelEditorSlot, GetFileName(s.path.c_str()), s.animCount),
                     (int)panel.x + 20, (int)panel.y + 14, 18, RAYWHITE);
            if (UIButton({panel.x + panel.width - 96, panel.y + 12, 80, 26}, "Close", 13) || IsKeyPressed(KEY_ESCAPE))
                modelEditorOpen = false;

            // Preview 3D (izquierda).
            Rectangle prev = {panel.x + 20, panel.y + 50, 360, 360};
            DrawRectangleLinesEx({prev.x - 1, prev.y - 1, prev.width + 2, prev.height + 2}, 1, g_theme.border);
            DrawTexturePro(modelPreviewRT.texture, {0, 0, 360, -360}, prev, {0, 0}, 0.0f, WHITE);

            // Slider genérico (arrastra) — devuelve true si cambió.
            auto slider = [&](float x, float y, const char* label, float* val, float lo, float hi, const char* fmt) {
                DrawText(label, (int)x, (int)y + 2, 12, (Color){190, 190, 205, 255});
                Rectangle r = {x + 84, y, 150, 16};
                DrawRectangleRec(r, (Color){28, 30, 42, 255});
                float t = (*val - lo) / (hi - lo); t = t < 0 ? 0 : (t > 1 ? 1 : t);
                DrawRectangle((int)r.x, (int)r.y, (int)(r.width * t), (int)r.height, (Color){80, 140, 180, 255});
                DrawRectangleLinesEx(r, 1, (Color){80, 82, 105, 255});
                DrawText(TextFormat(fmt, *val), (int)r.x + r.width + 8, (int)y + 2, 12, RAYWHITE);
                if (!modelEditorOpen) return;
                if (CheckCollisionPointRec(mm, r) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float nt = (mm.x - r.x) / r.width; nt = nt < 0 ? 0 : (nt > 1 ? 1 : nt);
                    *val = lo + nt * (hi - lo);
                }
            };

            int cx = (int)panel.x + 410, cy = (int)panel.y + 54;
            DrawText("TRANSFORM", cx, cy, 14, (Color){150, 210, 235, 255}); cy += 22;
            slider((float)cx, (float)cy, "Pos X", &s.pos.x, -3.0f, 3.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Pos Y", &s.pos.y, -3.0f, 3.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Pos Z", &s.pos.z, -3.0f, 3.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Rot X", &s.rot.x, 0.0f, 360.0f, "%.0f"); cy += 22;
            slider((float)cx, (float)cy, "Rot Y", &s.rot.y, 0.0f, 360.0f, "%.0f"); cy += 22;
            slider((float)cx, (float)cy, "Rot Z", &s.rot.z, 0.0f, 360.0f, "%.0f"); cy += 22;
            slider((float)cx, (float)cy, "Scale", &s.userScale, 0.2f, 3.0f, "%.2f"); cy += 26;
            {   // Toon (cel) shading — para el look anime.
                DrawText("Toon shading", cx, cy + 2, 12, (Color){190, 190, 205, 255});
                Rectangle tb = {(float)(cx + 96), (float)cy, 60, 18};
                bool hov = CheckCollisionPointRec(mm, tb);
                DrawRectangleRec(tb, s.toon ? (Color){70, 96, 110, 255} : (hov ? g_theme.buttonHover : g_theme.button));
                DrawRectangleLinesEx(tb, s.toon ? 2.0f : 1.0f, g_theme.border);
                DrawText(s.toon ? "ON" : "OFF", (int)tb.x + 18, (int)tb.y + 3, 12, RAYWHITE);
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { s.toon = !s.toon; ApplyModelShader(s); }
                cy += 30;
            }
            if (s.humanoid) {   // Importar animación .vrma (retargeteada al humanoide).
                if (UIButton({(float)cx, (float)cy, 150, 22}, "Import anim (.vrma/.bvh)", 10)) {
                    char path[512] = "";
                    if (NativeOpenDialog("Import a humanoid animation (.vrma/.bvh)...",
                                         "*.vrma *.bvh *.glb *.gltf", path, sizeof(path)) == 1) {
                        VrmaClip clip;
                        if (loadHumanoidAnimFile(path, clip)) {
                            s.clips.push_back(clip); s.animNames.push_back(clip.name);
                            modelEditorAnim = kHumanoidAnimCount + (int)s.clips.size() - 1; modelEditorFrame = 0.0f;
                            SetStatus("%s imported: %s (%.1fs)", clip.isBvh ? "BVH" : "VRMA",
                                      clip.name.c_str(), clip.duration);
                        } else SetStatus("Not a valid humanoid animation: %s", GetFileName(path));
                    }
                }
                DrawText(TextFormat("%d imported", (int)s.clips.size()), cx + 158, cy + 4, 11, (Color){150, 200, 170, 255});
                cy += 26;
                // Un .bvh de mocap no dice hacia dónde mira el personaje, así que
                // la mitad de ellos salen espejados. Este interruptor lo arregla
                // en el acto y se ve en la vista previa de al lado.
                {
                    int ci = modelEditorAnim - kHumanoidAnimCount;
                    if (ci >= 0 && ci < (int)s.clips.size()) {
                        VrmaClip& vc = s.clips[ci];
                        Rectangle fb = {(float)cx, (float)cy, 150, 20};
                        bool fh = CheckCollisionPointRec(mm, fb);
                        DrawRectangleRec(fb, vc.flipY ? (Color){70, 96, 110, 255} : (fh ? g_theme.buttonHover : g_theme.button));
                        DrawRectangleLinesEx(fb, vc.flipY ? 2.0f : 1.0f, g_theme.border);
                        DrawText(vc.flipY ? "Facing: flipped 180" : "Flip facing 180", cx + 8, (int)cy + 4, 10, RAYWHITE);
                        if (fh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) vc.flipY = !vc.flipY;
                        DrawText(vc.isBvh ? "(mocap .bvh: use it if the motion looks mirrored)"
                                          : "(.vrma is auto-corrected; rarely needed)",
                                 cx + 158, (int)cy + 5, 9, (Color){150, 150, 175, 255});
                    }
                }
                cy += 26;
            }

            DrawText("LIGHTING (global)", cx, cy, 14, (Color){235, 200, 120, 255}); cy += 22;
            slider((float)cx, (float)cy, "Ambient", &g_ambient, 0.0f, 1.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Intensity", &g_lightIntensity, 0.0f, 2.5f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Dir X", &g_lightDir.x, -1.0f, 1.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Dir Y", &g_lightDir.y, -1.0f, 1.0f, "%.2f"); cy += 22;
            slider((float)cx, (float)cy, "Dir Z", &g_lightDir.z, -1.0f, 1.0f, "%.2f"); cy += 24;
            {   // color de la luz (R/G/B)
                float r = g_lightColor.r, g = g_lightColor.g, b = g_lightColor.b;
                slider((float)cx, (float)cy, "Light R", &r, 0.0f, 255.0f, "%.0f"); cy += 22;
                slider((float)cx, (float)cy, "Light G", &g, 0.0f, 255.0f, "%.0f"); cy += 22;
                slider((float)cx, (float)cy, "Light B", &b, 0.0f, 255.0f, "%.0f"); cy += 22;
                g_lightColor = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
            }

            // Animación de preview (fila de botones bajo el preview).
            DrawText(modelIsProcedural(s) ? "Preview anim (procedural):" : "Preview anim:", (int)panel.x + 20, (int)prev.y + prev.height + 8, 12, (Color){160, 160, 180, 255});
            int axp = (int)panel.x + 20, ayp = (int)prev.y + prev.height + 26;
            for (int a = 0; a < modelAnimTotal(s); a++) {
                Rectangle r = {(float)axp, (float)ayp, 26, 20};
                bool sel = (modelEditorAnim == a);
                bool hov = CheckCollisionPointRec(mm, r);
                DrawRectangleRec(r, sel ? (Color){70, 96, 110, 255} : (hov ? g_theme.buttonHover : g_theme.button));
                DrawRectangleLinesEx(r, sel ? 2.0f : 1.0f, g_theme.border);
                DrawText(TextFormat("%d", a), (int)r.x + (a < 10 ? 9 : 5), (int)r.y + 5, 10, RAYWHITE);
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { modelEditorAnim = a; modelEditorFrame = 0.0f; }
                axp += 28;
                if (axp + 26 > (int)prev.x + (int)prev.width) { axp = (int)panel.x + 20; ayp += 22; }
            }

            // --- TRIM / LOOP de la animación seleccionada (elige de qué frame a
            // qué frame se reproduce/loopea; sirve para .vrma y .glb) ---
            {
                int a = modelEditorAnim;
                int fc = modelAnimFrames(s, a);
                int tx = (int)panel.x + 20, ty = ayp + 34;
                DrawText(TextFormat("ANIM %d TRIM (loop range) - %d frames", a, fc), tx, ty, 13, (Color){150, 235, 170, 255});
                ty += 20;
                float maxf = (float)(fc > 1 ? fc : 1);
                float fs = (float)animTrimStartF(s, a);
                float fe = (float)animTrimEndF(s, a);
                slider((float)tx, (float)ty, "Start", &fs, 0.0f, maxf - 1.0f, "%.0f"); ty += 22;
                slider((float)tx, (float)ty, "End", &fe, 1.0f, maxf, "%.0f"); ty += 24;
                if (a >= 0 && a < MAX_MODEL_ANIMS && fc > 1) {
                    int ns = (int)(fs + 0.5f), ne = (int)(fe + 0.5f);
                    if (ns < 0) ns = 0;
                    if (ns > fc - 1) ns = fc - 1;
                    if (ne <= ns) ne = ns + 1;
                    if (ne > fc) ne = fc;
                    s.animTrimStart[a] = ns;
                    s.animTrimEnd[a]   = (ne >= fc) ? 0 : ne; // fc = "hasta el final" (0)
                    // Loop ON/OFF
                    DrawText("Loop", tx, ty + 2, 12, (Color){190, 190, 205, 255});
                    Rectangle lb = {(float)(tx + 46), (float)ty, 52, 18};
                    bool lh = CheckCollisionPointRec(mm, lb);
                    DrawRectangleRec(lb, s.animLoop[a] ? (Color){70, 110, 80, 255} : (lh ? g_theme.buttonHover : g_theme.button));
                    DrawRectangleLinesEx(lb, s.animLoop[a] ? 2.0f : 1.0f, g_theme.border);
                    DrawText(s.animLoop[a] ? "ON" : "OFF", (int)lb.x + 13, (int)lb.y + 3, 12, RAYWHITE);
                    if (lh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) s.animLoop[a] = !s.animLoop[a];
                    // Rango completo
                    Rectangle fb = {(float)(tx + 112), (float)ty, 92, 18};
                    bool fh = CheckCollisionPointRec(mm, fb);
                    DrawRectangleRec(fb, fh ? g_theme.buttonHover : g_theme.button);
                    DrawRectangleLinesEx(fb, 1, g_theme.border);
                    DrawText("Full range", (int)fb.x + 9, (int)fb.y + 3, 11, RAYWHITE);
                    if (fh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { s.animTrimStart[a] = 0; s.animTrimEnd[a] = 0; }
                } else {
                    DrawText("(animation too short to trim)", tx, ty, 11, (Color){150, 150, 170, 255});
                }
            }

            if (UIButton({panel.x + 20, panel.y + panel.height - 40, 120, 28}, "Remove model", 12)) {
                UnloadModelSlot(modelEditorSlot);
                if (selectedModel == modelEditorSlot) selectedModel = -1;
                modelEditorOpen = false;
                SetStatus("Model removed");
            }
            if (UIButton({panel.x + 150, panel.y + panel.height - 40, 120, 28}, "Reset transform", 12)) {
                s.pos = {0, 0, 0}; s.rot = {0, 0, 0}; s.userScale = 1.0f;
            }
        }

        // --- Mensaje de estado ---
        if (g_statusTimer > 0.0f && g_status[0] != '\0') {
            int tw = MeasureText(g_status, 16);
            DrawRectangle(6, 6, tw + 16, 28, (Color){0, 0, 0, 190});
            DrawText(g_status, 14, 12, 16, (Color){255, 230, 120, 255});
        }

        // --- Tooltip de celda ---
        if (hoverCellX >= 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const MirrorCell& mc = g_mirror[hoverCellY * g_gridW + hoverCellX];
            if (!cellIsBlank(mc)) {
                const char* what = mc.clip >= SAMPLE_BASE ? "sample" : "clip";
                const char* tip = "";
                if (mc.kind == CELL_COLOR || mc.kind == CELL_SUSTAIN) {
                    tip = TextFormat("%s %s | note %s", what, slotLabel(mc.clip), SemitoneName(mc.pitchIdx));
                } else if (mc.kind == CELL_ARROW) {
                    tip = TextFormat("turn %s", kDirName[mc.dir]);
                } else if (mc.kind == CELL_MUTE) {
                    tip = "mute (cuts the notey's sound & video)";
                } else if (mc.kind == CELL_FX) {
                    tip = TextFormat("%s | %s %s | note %s", kFxNames[mc.fxType], what, slotLabel(mc.clip), SemitoneName(mc.pitchIdx));
                } else if (mc.kind == CELL_TELEPORT) {
                    tip = mc.fxType == 0 ? TextFormat("portal %d entrance A -> exit B", mc.dir + 1)
                                         : TextFormat("portal %d exit B", mc.dir + 1);
                } else if (mc.kind == CELL_EMPTY) {
                    tip = "rest (the notey waits here, silent)";
                }
                // Los atributos superpuestos se añaden al final, así que una
                // celda con nota + eco + espera + volumen lo cuenta todo. Se
                // arma en un buffer propio: TextFormat rota entre unos pocos
                // buffers internos y encadenarlo consigo mismo es frágil.
                char tipBuf[256];
                int tl = snprintf(tipBuf, sizeof(tipBuf), "%s", tip);
                if (mc.hold > 0.0f && tl < (int)sizeof(tipBuf))
                    tl += snprintf(tipBuf + tl, sizeof(tipBuf) - tl, "%shold %.1fs", tl ? " | " : "", mc.hold);
                if (mc.vol < 1.0f && tl < (int)sizeof(tipBuf))
                    tl += snprintf(tipBuf + tl, sizeof(tipBuf) - tl, "%svol %d%%", tl ? " | " : "",
                                   (int)(mc.vol * 100.0f + 0.5f));
                if (mc.tmul != 1.0f && tl < (int)sizeof(tipBuf))
                    snprintf(tipBuf + tl, sizeof(tipBuf) - tl, "%sspeed x%.2g", tl ? " | " : "", mc.tmul);
                tip = tipBuf;
                int tw = MeasureText(tip, 14);
                int tx = mouseX + 14, ty = mouseY + 18;
                if (tx + tw + 12 > screenWidth) tx = mouseX - tw - 18;
                DrawRectangle(tx, ty, tw + 12, 22, (Color){0, 0, 0, 210});
                DrawRectangleLines(tx, ty, tw + 12, 22, (Color){100, 100, 130, 255});
                DrawText(tip, tx + 6, ty + 4, 14, RAYWHITE);
            }
        }

        // --- Modal: grabar desde el inicio o desde aquí ---
        if (recChoiceOpen) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {(float)(screenWidth / 2 - 230), 250, 460, 220};
            DrawRectangleRec(panel, (Color){26, 27, 40, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            const char* title = recChoiceVideo ? "Record VIDEO + audio" : "Record AUDIO";
            DrawText(title, (int)(panel.x + (panel.width - MeasureText(title, 18)) / 2), (int)panel.y + 16, 18, RAYWHITE);
            const char* q = "Start recording from...";
            DrawText(q, (int)(panel.x + (panel.width - MeasureText(q, 14)) / 2), (int)panel.y + 48, 14, (Color){165, 165, 185, 255});

            if (UIButton({panel.x + 30, panel.y + 80, 190, 44}, "START (reset all)", 14)) {
                clearUndoHistory();
                activateScene(g_curScene);
                RequestResetPlayhead();
                if (paused) togglePause();
                startRecording(recChoiceVideo);
                recChoiceOpen = false;
            }
            if (UIButton({panel.x + 240, panel.y + 80, 190, 44}, "HERE (as playing)", 14)) {
                startRecording(recChoiceVideo);
                recChoiceOpen = false;
            }
            // Dónde acaba la toma. Por defecto en temp/ y sin preguntar, para
            // poder encadenar tomas sin un diálogo por medio; con esto puesto,
            // pregunta al terminar cada una.
            {
                Rectangle ab = {panel.x + 30, panel.y + 134, 400, 26};
                if (UIButton(ab, recAskWhere ? "Ask me where to save each take"
                                             : "Takes land in temp/ (press SAVE AS to keep one)", 12)) {
                    recAskWhere = !recAskWhere;
                    saveControls();
                }
                if (recAskWhere) DrawRectangleLinesEx(ab, 2, (Color){120, 230, 140, 255});
            }
            if (UIButton({panel.x + panel.width / 2 - 60, panel.y + 170, 120, 30}, "Cancel", 13) ||
                IsKeyPressed(KEY_ESCAPE)) {
                recChoiceOpen = false;
            }
        }

        // --- Empty-slot chooser: load a file OR record from camera/mic ---
        if (slotChoiceOpen) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
            Rectangle panel = {(float)(screenWidth / 2 - 240), 230, 480, 262};
            DrawRectangleRec(panel, (Color){26, 27, 40, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});
            const char* title = slotChoiceVideo ? TextFormat("Fill CLIP %s", slotLabel(slotChoiceSlot))
                                                 : TextFormat("Fill SMP %s", slotLabel(slotChoiceSlot));
            DrawText(title, (int)(panel.x + (panel.width - MeasureText(title, 18)) / 2), (int)panel.y + 14, 18, RAYWHITE);

            if (UIButton({panel.x + 30, panel.y + 46, 200, 40},
                         slotChoiceVideo ? "Load file (video/image/GIF)" : "Load file (audio)", 12)) {
                int s = slotChoiceSlot;
                slotChoiceOpen = false;
                char path[512] = "";
                int r2 = slotChoiceVideo
                             ? NativeOpenDialog("Load video / image / GIF...", "*.mp4 *.webm *.mov *.avi *.mkv *.mpg *.mpeg *.gif *.png *.jpg *.jpeg *.bmp", path, sizeof(path))
                             : NativeOpenDialog("Load audio sample...", "*.wav *.mp3 *.ogg *.flac", path, sizeof(path));
                if (r2 == 1 && loadFileIntoSlot(s, path))
                    SetStatus("%s %s loaded", slotChoiceVideo ? "Clip" : "Sample", slotLabel(s));
            }
            if (UIButton({panel.x + 250, panel.y + 46, 200, 40},
                         slotChoiceVideo ? "Record CAMERA + mic" : "Record MICROPHONE", 12)) {
                int s = slotChoiceSlot;
                bool v = slotChoiceVideo;
                slotChoiceOpen = false;
                recordCamMic(s, v);
            }
            // Record from the PHONE (either the MJPEG app or the UDP script).
            {
                bool camOn = g_ipcam.connected();
                bool phoneOn = camOn || g_phone.connected();
                Color pc = phoneOn ? (Color){60, 120, 70, 255} : (Color){60, 62, 84, 255};
                Rectangle pb = {panel.x + 30, panel.y + 92, 420, 34};
                bool hov = CheckCollisionPointRec(GetMousePosition(), pb);
                DrawRectangleRec(pb, hov ? (Color){(unsigned char)(pc.r + 20), (unsigned char)(pc.g + 20), (unsigned char)(pc.b + 20), 255} : pc);
                DrawRectangleLinesEx(pb, 1, phoneOn ? (Color){120, 210, 150, 255} : (Color){90, 92, 120, 255});
                const char* src = camOn ? " [app]" : " [udp]";
                const char* pl = !phoneOn
                    ? "Record from PHONE  (no phone yet - open DEV)"
                    : (slotChoiceVideo ? TextFormat("Record from PHONE (camera + mic)%s", src)
                                       : TextFormat("Record from PHONE (mic)%s", src));
                DrawText(pl, (int)pb.x + 14, (int)pb.y + 10, 12, phoneOn ? (Color){190, 240, 200, 255} : (Color){170, 170, 190, 255});
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int s = slotChoiceSlot; bool v = slotChoiceVideo;
                    if (phoneOn) { slotChoiceOpen = false; recordFromPhone(s, v); }
                    else {
                        if (!g_phone.isRunning()) g_phone.start(kPhonePort);
                        SetStatus("No phone yet - open DEV, share the app, then press SEARCH on the phone");
                    }
                }
            }

            // Duration selector for the recording.
            DrawText("Record length:", (int)panel.x + 30, (int)panel.y + 150, 13, (Color){165, 165, 185, 255});
            const int durs[] = {2, 4, 8, 15};
            for (int d = 0; d < 4; d++) {
                Rectangle db = {panel.x + 150 + d * 52, panel.y + 146, 46, 24};
                bool sel = camRecSeconds == durs[d];
                DrawRectangleRec(db, sel ? (Color){64, 68, 96, 255} : (Color){40, 42, 58, 255});
                DrawRectangleLinesEx(db, sel ? 2.0f : 1.0f, sel ? RAYWHITE : (Color){80, 82, 105, 255});
                DrawText(TextFormat("%ds", durs[d]), (int)db.x + 12, (int)db.y + 5, 13, RAYWHITE);
                if (CheckCollisionPointRec(GetMousePosition(), db) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    camRecSeconds = durs[d];
            }
            DrawText("Camera/mic use ffmpeg. For a phone, open DEV -> PHONE / CAMERA / MIC.",
                     (int)panel.x + 20, (int)panel.y + 184, 11, (Color){150, 130, 100, 255});

            if (UIButton({panel.x + panel.width / 2 - 60, panel.y + 216, 120, 30}, "Cancel", 13) ||
                IsKeyPressed(KEY_ESCAPE)) {
                slotChoiceOpen = false;
            }
        }

        // --- Editor de recorte (video o sample) ---
        if (editorOpen && !editorIsAudio) {
            const InstrumentSource& src = g_engine.getInstrumentBank().at(editorSlot);
            if (!src.hasVideo()) {
                editorOpen = false;
            } else {
                int total = (int)src.videoFrames.size();
                float fps = src.videoFramerate > 0.0 ? (float)src.videoFramerate : 30.0f;
                if (edEnd > total) edEnd = total;
                if (edEnd < 1) edEnd = total;
                if (edStart > edEnd - 1) edStart = edEnd - 1;
                if (edStart < 0) edStart = 0;

                DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
                // Panel algo más alto para que la fila de acciones (Empty slot /
                // Reset / Apply / Close) quede DEBAJO de la fila de FX, sin solaparse.
                Rectangle panel = {150, 36, 980, 664};
                DrawRectangleRec(panel, (Color){26, 27, 40, 255});
                DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});

                DrawText(TextFormat("TRIM CLIP %s   (%dx%d, %.1fs total)", slotLabel(editorSlot),
                                    src.videoWidth, src.videoHeight, total / fps),
                         (int)panel.x + 20, (int)panel.y + 14, 18, RAYWHITE);

                // Visual swap controls (top-right, clear of the FX row below).
                if (UIButton({panel.x + panel.width - 350, panel.y + 8, 200, 26}, "Replace visual (image/GIF)", 12)) {
                    replaceSlotVisual(editorSlot);
                    editorOpen = false;
                }
                if (!g_slotVisualPath[editorSlot].empty()) {
                    if (UIButton({panel.x + panel.width - 142, panel.y + 8, 130, 26}, "Remove visual", 12)) {
                        revertSlotVisual(editorSlot);
                        editorOpen = false;
                    }
                }
                // Pure Data / PlugData insert effect (top-left, under title).
                {
                    bool hasPd = !g_slotPdPath[editorSlot].empty();
                    if (UIButton({panel.x + panel.width - 520, panel.y + 8, 90, 26},
                                 hasPd ? "Pd fx: on" : "Pd effect", 12)) {
                        loadPdEffect(editorSlot);
                    }
                    if (hasPd) {
                        if (UIButton({panel.x + panel.width - 424, panel.y + 8, 60, 26}, "clear", 11)) {
                            clearPdEffect(editorSlot);
                        }
                    }
                }

                // Playback: while playing, follow the audio preview (so video
                // and sound stay locked); if the clip has no audio, just roll
                // the frames. When stopped, the frame is frozen.
                bool clipHasAudio = src.hasAudio();
                if (edPlaying) {
                    long long a0 = (long long)((double)edStart / fps * 44100.0);
                    long long a1 = (long long)((double)edEnd / fps * 44100.0);
                    if (clipHasAudio) {
                        RequestPreviewPlay(editorSlot, a0, a1); // keep range in sync
                        edCursor = (float)((double)snapshot.previewCursor / 44100.0 * fps);
                    } else {
                        if (edDrag == 0) edCursor += fps * dt;
                    }
                    if (edCursor >= (float)edEnd || edCursor < (float)edStart) edCursor = (float)edStart;
                }
                int previewIdx = (int)edCursor;
                if (previewIdx < 0) previewIdx = 0;
                if (previewIdx >= total) previewIdx = total - 1;

                Texture2D& tex = getOrCreateTexture(editorSlot, src.videoWidth, src.videoHeight, src.videoChannels);
                uploadSlotFrame(tex, editorSlot, previewIdx, src);

                Rectangle prev = {panel.x + 20, panel.y + 44, panel.width - 40, 330};
                DrawRectangleRec(prev, BLACK);
                float scale = prev.width / tex.width;
                if (tex.height * scale > prev.height) scale = prev.height / tex.height;
                float dw = tex.width * scale, dh = tex.height * scale;
                // If this slot has a GLSL shader, preview it applied here too, so
                // the user sees the effect while editing (it also runs in the collage).
                bool edUseShader = g_slotShaderOn[editorSlot] && g_slotShader[editorSlot].id != 0;
                if (edUseShader) {
                    Shader& sh = g_slotShader[editorSlot];
                    if (g_slotShaderLocTime[editorSlot] >= 0) { float tt = (float)GetTime(); SetShaderValue(sh, g_slotShaderLocTime[editorSlot], &tt, SHADER_UNIFORM_FLOAT); }
                    if (g_slotShaderLocRes[editorSlot] >= 0) { Vector2 rr = {(float)tex.width, (float)tex.height}; SetShaderValue(sh, g_slotShaderLocRes[editorSlot], &rr, SHADER_UNIFORM_VEC2); }
                    BeginShaderMode(sh);
                }
                DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height},
                               {prev.x + (prev.width - dw) / 2, prev.y + (prev.height - dh) / 2, dw, dh},
                               {0, 0}, 0.0f, WHITE);
                if (edUseShader) EndShaderMode();

                // Play / Stop button (on top of the video, top-left corner).
                {
                    const char* pl = edPlaying ? "STOP" : (clipHasAudio ? "PLAY" : "PLAY (no audio)");
                    Rectangle pb = {prev.x + 8, prev.y + 8, clipHasAudio ? 66.0f : 120.0f, 26};
                    if (UIButton(pb, pl, 12)) {
                        if (edPlaying) stopEdPreview();
                        else {
                            edPlaying = true;
                            edCursor = (float)edStart;
                            if (clipHasAudio) {
                                long long a0 = (long long)((double)edStart / fps * 44100.0);
                                long long a1 = (long long)((double)edEnd / fps * 44100.0);
                                RequestPreviewPlay(editorSlot, a0, a1);
                            }
                        }
                    }
                }

                // GLSL video shader (top-right of the preview): import a .fs/.glsl
                // the user wrote separately; it runs on this clip in the collage.
                {
                    bool hasSh = g_slotShaderOn[editorSlot];
                    Rectangle sb = {prev.x + prev.width - (hasSh ? 172.0f : 108.0f), prev.y + 8, 100, 26};
                    if (UIButton(sb, hasSh ? "GLSL: on" : "GLSL fx", 12)) {
                        loadShaderEffect(editorSlot);
                    }
                    if (hasSh) {
                        if (UIButton({sb.x + 104, sb.y, 60, 26}, "clear", 11)) {
                            clearShaderEffect(editorSlot);
                        }
                    }
                }

                {
                    ClipFX& fx = g_clipFX[editorSlot];
                    if (fx.move) {
                        Vector2 pm = GetMousePosition();
                        auto normX = [&](float x) { return (x - prev.x) / prev.width; };
                        auto normY = [&](float y) { return (y - prev.y) / prev.height; };
                        auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(pm, prev)) {
                            edMoveDragging = true;
                            fx.ax = clamp01(normX(pm.x));
                            fx.ay = clamp01(normY(pm.y));
                            fx.bx = fx.ax;
                            fx.by = fx.ay;
                        }
                        if (edMoveDragging) {
                            fx.bx = clamp01(normX(pm.x));
                            fx.by = clamp01(normY(pm.y));
                            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) edMoveDragging = false;
                        }

                        Vector2 pa = {prev.x + fx.ax * prev.width, prev.y + fx.ay * prev.height};
                        Vector2 pb = {prev.x + fx.bx * prev.width, prev.y + fx.by * prev.height};
                        DrawLineEx(pa, pb, 3, (Color){255, 230, 120, 220});
                        DrawCircleV(pa, 8, (Color){120, 230, 140, 255});
                        DrawCircleV(pb, 8, (Color){230, 120, 120, 255});
                        DrawText("A", (int)pa.x - 4, (int)pa.y - 6, 13, BLACK);
                        DrawText("B", (int)pb.x - 4, (int)pb.y - 6, 13, BLACK);
                        // A la derecha del boton PLAY, que ocupa la esquina.
                        DrawText("MOVE: drag on the preview to set the A -> B path (each note travels it)",
                                 (int)prev.x + 140, (int)prev.y + 14, 13, (Color){255, 230, 120, 255});
                    } else if (fx.place) {
                        // ---- ESCENARIO: colocar el clip a mano ----
                        // Sobre la vista previa se dibuja el FOTOGRAMA QUE SE
                        // EXPORTA con su proporción real (16:9 o 9:16), y dentro
                        // la caja del clip tal y como saldrá: en su sitio, a su
                        // tamaño y con su giro. Se arrastra con el ratón.
                        //
                        // Se muestra el marco de exportación y no la vista
                        // previa a pelo porque la posición se guarda en 0..1 DEL
                        // FOTOGRAMA: colocar sobre otra cosa mentiría en cuanto
                        // se cambiara de 16:9 a vertical.
                        float fa = (float)g_exportW / (float)g_exportH;
                        float sw2 = prev.width, sh2 = prev.width / fa;
                        if (sh2 > prev.height) { sh2 = prev.height; sw2 = sh2 * fa; }
                        Rectangle stage = {prev.x + (prev.width - sw2) / 2,
                                           prev.y + (prev.height - sh2) / 2, sw2, sh2};
                        DrawRectangleRec(stage, (Color){12, 12, 18, 255});
                        // Rejilla de tercios: para alinear a ojo sin sufrir.
                        for (int gl = 1; gl < 3; gl++) {
                            DrawLine((int)(stage.x + stage.width * gl / 3), (int)stage.y,
                                     (int)(stage.x + stage.width * gl / 3), (int)(stage.y + stage.height),
                                     (Color){50, 52, 70, 255});
                            DrawLine((int)stage.x, (int)(stage.y + stage.height * gl / 3),
                                     (int)(stage.x + stage.width), (int)(stage.y + stage.height * gl / 3),
                                     (Color){50, 52, 70, 255});
                        }
                        DrawRectangleLinesEx(stage, 2, (Color){110, 112, 140, 255});

                        // La caja del clip, calculada IGUAL que en el collage:
                        // mismo alto base, misma escala. Lo que se ve aquí es
                        // lo que va a salir.
                        float aspectC = (float)tex.width / (float)tex.height;
                        float baseC = (float)(g_exportW < g_exportH ? g_exportW : g_exportH);
                        float hC = baseC * 0.47f * fx.scale;
                        float wC = hC * aspectC;
                        float k = stage.width / (float)g_exportW;
                        float cw = wC * k, ch = hC * k;
                        float cx = stage.x + fx.posX * stage.width;
                        float cy = stage.y + fx.posY * stage.height;

                        Color ptint = BlendTintFX(fx.blend, fx.opacity);
                        BeginBlendModeFX(fx.blend);
                        DrawTexturePro(tex, {0, 0, (float)tex.width, (float)tex.height},
                                       {cx, cy, cw, ch}, {cw / 2.0f, ch / 2.0f}, fx.rotDeg, ptint);
                        EndBlendModeFX(fx.blend);
                        // Contorno del clip, sin girar, para que se vea el asa
                        // aunque la opacidad esté a cero.
                        DrawCircleV({cx, cy}, 7, (Color){255, 230, 120, 230});
                        DrawCircleLines((int)cx, (int)cy, 7, BLACK);

                        Vector2 pm = GetMousePosition();
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(pm, stage))
                            edMoveDragging = true;
                        if (edMoveDragging) {
                            float nx = (pm.x - stage.x) / stage.width;
                            float ny = (pm.y - stage.y) / stage.height;
                            fx.posX = nx < 0.0f ? 0.0f : (nx > 1.0f ? 1.0f : nx);
                            fx.posY = ny < 0.0f ? 0.0f : (ny > 1.0f ? 1.0f : ny);
                            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) edMoveDragging = false;
                        }
                        // La rueda gira el clip aquí mismo, que es más cómodo
                        // que ir al botón de abajo mientras se coloca.
                        if (CheckCollisionPointRec(pm, stage)) {
                            float wheel = GetMouseWheelMove();
                            if (wheel != 0.0f) {
                                fx.rotDeg += wheel * (IsKeyDown(KEY_LEFT_SHIFT) ? 1.0f : 15.0f);
                                fx.rotDeg = fmodf(fx.rotDeg, 360.0f);
                            }
                        }
                        DrawText("PLACE: drag the clip where you want it | mouse wheel rotates (hold SHIFT for 1 degree steps)",
                                 (int)prev.x + 140, (int)prev.y + 14, 13, (Color){255, 230, 120, 255});
                        DrawText(TextFormat("x %.0f%%  y %.0f%%  rot %.0f deg", fx.posX * 100.0f, fx.posY * 100.0f, fx.rotDeg),
                                 (int)stage.x + 6, (int)(stage.y + stage.height - 18), 12, (Color){190, 210, 255, 255});
                    }
                }

                Rectangle bar = {panel.x + 20, panel.y + 388, panel.width - 40, 34};
                DrawRectangleRec(bar, (Color){40, 42, 58, 255});
                auto frameToX = [&](float fr) { return bar.x + fr / (float)total * bar.width; };
                auto xToFrame = [&](float x) {
                    int fr = (int)((x - bar.x) / bar.width * total + 0.5f);
                    if (fr < 0) fr = 0;
                    if (fr > total) fr = total;
                    return fr;
                };

                float sx = frameToX((float)edStart);
                float ex = frameToX((float)edEnd);
                DrawRectangle((int)sx, (int)bar.y, (int)(ex - sx), (int)bar.height, (Color){62, 96, 72, 255});
                float cursorX = frameToX(edCursor);
                DrawLineEx({cursorX, bar.y}, {cursorX, bar.y + bar.height}, 2, (Color){255, 230, 120, 255});
                DrawLineEx({sx, bar.y - 5}, {sx, bar.y + bar.height + 5}, 4, (Color){120, 230, 140, 255});
                DrawLineEx({ex, bar.y - 5}, {ex, bar.y + bar.height + 5}, 4, (Color){230, 120, 120, 255});

                Vector2 m = GetMousePosition();
                bool overBar = CheckCollisionPointRec(m, {bar.x - 12, bar.y - 10, bar.width + 24, bar.height + 20});
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overBar) {
                    if (fabsf(m.x - sx) < 10.0f) edDrag = 1;
                    else if (fabsf(m.x - ex) < 10.0f) edDrag = 2;
                    else edDrag = 3;
                }
                if (edDrag != 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    int fr = xToFrame(m.x);
                    if (edDrag == 1) {
                        edStart = fr;
                        if (edStart > edEnd - 1) edStart = edEnd - 1;
                        if (edStart < 0) edStart = 0;
                    } else if (edDrag == 2) {
                        edEnd = fr;
                        if (edEnd < edStart + 1) edEnd = edStart + 1;
                        if (edEnd > total) edEnd = total;
                    } else {
                        edCursor = (float)fr;
                        if (edCursor >= (float)edEnd) edCursor = (float)(edEnd - 1);
                        if (edCursor < (float)edStart) edCursor = (float)edStart;
                    }
                } else {
                    edDrag = 0;
                }

                DrawText(TextFormat("start %.2fs   end %.2fs   length %.2fs   drag handles, click bar to scrub",
                                    edStart / fps, edEnd / fps, (edEnd - edStart) / fps),
                         (int)panel.x + 20, (int)panel.y + 428, 13, (Color){165, 165, 185, 255});
                DrawText("Trim is non-destructive: the full clip stays loaded, playback just uses the range. FX apply instantly.",
                         (int)panel.x + 20, (int)panel.y + 446, 12, (Color){170, 145, 105, 255});

                {
                    ClipFX& fx = g_clipFX[editorSlot];
                    Vector2 fm = GetMousePosition();
                    float fy = panel.y + 466;
                    auto fxToggle = [&](Rectangle r, const char* label, bool& value) {
                        if (UIButton(r, label, 11)) value = !value;
                        if (value) DrawRectangleLinesEx(r, 2, (Color){120, 230, 140, 255});
                    };
                    fxToggle({panel.x + 20, fy, 52, 26}, "FLIP", fx.flipX);
                    fxToggle({panel.x + 76, fy, 52, 26}, "ZOOM", fx.zoomPulse);
                    fxToggle({panel.x + 132, fy, 56, 26}, "SPIN", fx.rotate);

                    // CENTER, MOVE y PLACE son tres formas de decidir DÓNDE cae
                    // el clip, así que se excluyen: encender una apaga las otras.
                    // Dejarlas convivir sólo servía para que el usuario ajustara
                    // una y no viera ningún cambio porque mandaba otra.
                    {
                        Rectangle rc = {panel.x + 192, fy, 58, 26};
                        if (UIButton(rc, "CENTER", 11)) { fx.center = !fx.center; if (fx.center) { fx.move = false; fx.place = false; } }
                        if (fx.center) DrawRectangleLinesEx(rc, 2, (Color){120, 230, 140, 255});
                        Rectangle rm = {panel.x + 254, fy, 52, 26};
                        if (UIButton(rm, "MOVE", 11)) { fx.move = !fx.move; if (fx.move) { fx.center = false; fx.place = false; } edMoveDragging = false; }
                        if (fx.move) DrawRectangleLinesEx(rm, 2, (Color){120, 230, 140, 255});
                        Rectangle rp = {panel.x + 310, fy, 54, 26};
                        if (UIButton(rp, "PLACE", 11)) { fx.place = !fx.place; if (fx.place) { fx.center = false; fx.move = false; } edMoveDragging = false; }
                        if (fx.place) DrawRectangleLinesEx(rp, 2, (Color){120, 230, 140, 255});
                    }

                    if (UIButton({panel.x + 380, fy, 24, 26}, "-", 13) && fx.scale > 0.25f) fx.scale -= 0.25f;
                    DrawText(TextFormat("size %.2f", fx.scale), (int)panel.x + 408, (int)fy + 7, 12, (Color){225, 200, 120, 255});
                    if (UIButton({panel.x + 470, fy, 24, 26}, "+", 13) && fx.scale < 4.0f) fx.scale += 0.25f;

                    if (UIButton({panel.x + 514, fy, 24, 26}, "-", 13) && fx.layer > 1) fx.layer--;
                    DrawText(TextFormat("layer %d", fx.layer), (int)panel.x + 542, (int)fy + 7, 12, (Color){225, 200, 120, 255});
                    if (UIButton({panel.x + 600, fy, 24, 26}, "+", 13) && fx.layer < 8) fx.layer++;

                    // ---- Segunda fila: transparencia, fusión y giro fijo ----
                    fy = panel.y + 500;
                    DrawText("OPACITY", (int)panel.x + 20, (int)fy + 7, 12, (Color){180, 180, 205, 255});
                    Rectangle ob = {panel.x + 84, fy + 4, 150, 18};
                    DrawRectangleRec(ob, (Color){36, 38, 52, 255});
                    DrawRectangle((int)ob.x, (int)ob.y, (int)(ob.width * fx.opacity), (int)ob.height,
                                  (Color){70, 150, 90, 255});
                    DrawRectangleLinesEx(ob, 1, (Color){90, 92, 118, 255});
                    DrawText(TextFormat("%d%%", (int)(fx.opacity * 100.0f + 0.5f)),
                             (int)(ob.x + ob.width + 8), (int)fy + 7, 12, (Color){225, 200, 120, 255});
                    if (CheckCollisionPointRec(fm, ob) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        float t = (fm.x - ob.x) / ob.width;
                        fx.opacity = t < 0 ? 0 : (t > 1 ? 1 : t);
                    }

                    DrawText("BLEND", (int)panel.x + 288, (int)fy + 7, 12, (Color){180, 180, 205, 255});
                    if (UIButton({panel.x + 336, fy, 92, 26}, kBlendNames[fx.blend], 11))
                        fx.blend = (fx.blend + 1) % BLEND_FX_COUNT;

                    DrawText("ROTATION", (int)panel.x + 442, (int)fy + 7, 12, (Color){180, 180, 205, 255});
                    if (UIButton({panel.x + 508, fy, 24, 26}, "-", 13)) fx.rotDeg = fmodf(fx.rotDeg - 15.0f + 360.0f, 360.0f);
                    DrawText(TextFormat("%.0f deg", fx.rotDeg), (int)panel.x + 536, (int)fy + 7, 12, (Color){225, 200, 120, 255});
                    if (UIButton({panel.x + 596, fy, 24, 26}, "+", 13)) fx.rotDeg = fmodf(fx.rotDeg + 15.0f, 360.0f);
                    if (UIButton({panel.x + 624, fy, 34, 26}, "0", 12)) fx.rotDeg = 0.0f;

                    if (UIButton({panel.x + 672, fy, 96, 26}, "Reset look", 11)) {
                        int keepLayer = fx.layer;
                        fx = ClipFX();
                        fx.layer = keepLayer;
                    }

                    DrawText(TextFormat("BLEND %s: %s", kBlendNames[fx.blend], kBlendHelp[fx.blend]),
                             (int)panel.x + 20, (int)panel.y + 534, 12, (Color){150, 200, 235, 255});
                    DrawText("PLACE puts the clip exactly where you drag it; CENTER pins it to the middle; with neither, notes scatter it around.",
                             (int)panel.x + 20, (int)panel.y + 550, 12, (Color){150, 150, 175, 255});
                    DrawText("Position is stored as a fraction of the frame, so it survives switching between 16:9 and 9:16.",
                             (int)panel.x + 20, (int)panel.y + 566, 12, (Color){150, 150, 175, 255});
                }

                // Empty this slot (unload everything so it's blank again).
                if (UIButton({panel.x + 20, panel.y + panel.height - 38, 120, 28}, "Empty slot", 12)) {
                    clearSlot(editorSlot);
                    editorOpen = false;
                }
                if (UIButton({panel.x + panel.width - 232, panel.y + panel.height - 38, 100, 28}, "Apply")) {
                    // Non-destructive: just set the range (full clip kept in RAM).
                    g_engine.getInstrumentBank().SetVideoTrim(editorSlot, edStart, edEnd);
                    g_slotTrimStart[editorSlot] = edStart;
                    g_slotTrimLen[editorSlot] = (edEnd >= total) ? 0 : (edEnd - edStart);
                    SetStatus("Clip %s trimmed to %.2fs (full clip kept)", slotLabel(editorSlot), (edEnd - edStart) / fps);
                    editorOpen = false;
                }
                if (UIButton({panel.x + panel.width - 344, panel.y + panel.height - 38, 106, 28}, "Reset trim", 12)) {
                    g_engine.getInstrumentBank().ClearTrim(editorSlot);
                    g_slotTrimStart[editorSlot] = 0;
                    g_slotTrimLen[editorSlot] = 0;
                    edStart = 0; edEnd = total;
                    SetStatus("Trim reset - full clip %s", slotLabel(editorSlot));
                }
                if (UIButton({panel.x + panel.width - 122, panel.y + panel.height - 38, 100, 28}, "Close") ||
                    IsKeyPressed(KEY_ESCAPE)) {
                    editorOpen = false;
                }
            }
        } else if (editorOpen && editorIsAudio) {
            // ---------- Editor de SAMPLE: forma de onda + recorte ----------
            const InstrumentSource& src = g_engine.getInstrumentBank().at(editorSlot);
            if (!src.hasAudio()) {
                editorOpen = false;
            } else {
                int total = (int)src.audio.totalFrames;
                const float sr = 44100.0f;
                if (edEnd > total) edEnd = total;
                if (edStart > edEnd - 1) edStart = edEnd - 1;
                if (edStart < 0) edStart = 0;

                DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 180});
                Rectangle panel = {180, 120, 920, 480};
                DrawRectangleRec(panel, (Color){26, 27, 40, 255});
                DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});

                DrawText(TextFormat("TRIM SAMPLE %s   (%.2fs total)", slotLabel(editorSlot), total / sr),
                         (int)panel.x + 20, (int)panel.y + 14, 18, RAYWHITE);

                // Visual controls (top-right): give this sample an image/GIF,
                // or remove it and go back to audio-only.
                const InstrumentSource& es = g_engine.getInstrumentBank().at(editorSlot);
                if (UIButton({panel.x + panel.width - 350, panel.y + 8, 200, 26},
                             es.hasVideo() ? "Change visual (image/GIF)" : "Set visual (image/GIF)", 12)) {
                    replaceSlotVisual(editorSlot);
                    editorOpen = false;
                }
                if (!g_slotVisualPath[editorSlot].empty()) {
                    if (UIButton({panel.x + panel.width - 142, panel.y + 8, 130, 26}, "Remove visual", 12)) {
                        revertSlotVisual(editorSlot);
                        editorOpen = false;
                    }
                }
                // Pure Data / PlugData insert effect for this sample.
                {
                    bool hasPd = !g_slotPdPath[editorSlot].empty();
                    if (UIButton({panel.x + panel.width - 520, panel.y + 8, 90, 26},
                                 hasPd ? "Pd fx: on" : "Pd effect", 12)) {
                        loadPdEffect(editorSlot);
                    }
                    if (hasPd) {
                        if (UIButton({panel.x + panel.width - 424, panel.y + 8, 60, 26}, "clear", 11)) {
                            clearPdEffect(editorSlot);
                        }
                    }
                }

                // Forma de onda con la selección resaltada.
                Rectangle wave = {panel.x + 20, panel.y + 48, panel.width - 40, 280};
                DrawRectangleRec(wave, BLACK);
                int cols = (int)edWaveMin.size();
                float selX0 = wave.x + (float)edStart / total * wave.width;
                float selX1 = wave.x + (float)edEnd / total * wave.width;
                DrawRectangle((int)selX0, (int)wave.y, (int)(selX1 - selX0), (int)wave.height, (Color){40, 70, 48, 255});
                float midY = wave.y + wave.height / 2;
                for (int cIdx = 0; cIdx < cols; cIdx++) {
                    float x = wave.x + (float)cIdx / cols * wave.width;
                    float y0 = midY - edWaveMax[cIdx] * (wave.height / 2 - 4);
                    float y1 = midY - edWaveMin[cIdx] * (wave.height / 2 - 4);
                    bool inSel = x >= selX0 && x <= selX1;
                    DrawLineEx({x, y0}, {x, y1}, 1,
                               inSel ? (Color){120, 230, 140, 255} : (Color){90, 110, 130, 255});
                }
                DrawLineEx({wave.x, midY}, {wave.x + wave.width, midY}, 1, (Color){60, 60, 80, 160});

                // Moving playhead while previewing.
                if (edPlaying) {
                    float phx = wave.x + (float)snapshot.previewCursor / total * wave.width;
                    if (phx >= wave.x && phx <= wave.x + wave.width) {
                        DrawLineEx({phx, wave.y}, {phx, wave.y + wave.height}, 2, (Color){255, 230, 120, 255});
                    }
                    RequestPreviewPlay(editorSlot, edStart, edEnd); // keep range in sync
                }

                // Play / Stop button (top-left of the waveform).
                {
                    Rectangle pb = {wave.x + 8, wave.y + 8, 66, 26};
                    if (UIButton(pb, edPlaying ? "STOP" : "PLAY", 12)) {
                        if (edPlaying) stopEdPreview();
                        else { edPlaying = true; RequestPreviewPlay(editorSlot, edStart, edEnd); }
                    }
                }

                // Barra con handles (mismo manejo que el editor de video).
                Rectangle bar = {panel.x + 20, panel.y + 346, panel.width - 40, 40};
                DrawRectangleRec(bar, (Color){40, 42, 58, 255});
                float sx = bar.x + (float)edStart / total * bar.width;
                float ex = bar.x + (float)edEnd / total * bar.width;
                DrawRectangle((int)sx, (int)bar.y, (int)(ex - sx), (int)bar.height, (Color){62, 96, 72, 255});
                DrawLineEx({sx, bar.y - 5}, {sx, bar.y + bar.height + 5}, 4, (Color){120, 230, 140, 255});
                DrawLineEx({ex, bar.y - 5}, {ex, bar.y + bar.height + 5}, 4, (Color){230, 120, 120, 255});

                Vector2 m = GetMousePosition();
                bool overBar = CheckCollisionPointRec(m, {bar.x - 12, bar.y - 10, bar.width + 24, bar.height + 20}) ||
                               CheckCollisionPointRec(m, wave);
                auto xToSample = [&](float x) {
                    float bx = x < wave.x + 1 ? x : x; // misma escala en wave y bar
                    Rectangle& ref = CheckCollisionPointRec(m, wave) ? wave : bar;
                    int s = (int)((bx - ref.x) / ref.width * total + 0.5f);
                    if (s < 0) s = 0;
                    if (s > total) s = total;
                    return s;
                };
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overBar) {
                    if (fabsf(m.x - sx) < 10.0f) edDrag = 1;
                    else if (fabsf(m.x - ex) < 10.0f) edDrag = 2;
                    else edDrag = (xToSample(m.x) < (edStart + edEnd) / 2) ? 1 : 2;
                }
                if (edDrag != 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    int s = xToSample(m.x);
                    if (edDrag == 1) {
                        edStart = s;
                        if (edStart > edEnd - 1) edStart = edEnd - 1;
                        if (edStart < 0) edStart = 0;
                    } else {
                        edEnd = s;
                        if (edEnd < edStart + 1) edEnd = edStart + 1;
                        if (edEnd > total) edEnd = total;
                    }
                } else {
                    edDrag = 0;
                }

                DrawText(TextFormat("start %.3fs   end %.3fs   length %.3fs   drag green/red handles (or click sides)",
                                    edStart / sr, edEnd / sr, (edEnd - edStart) / sr),
                         (int)panel.x + 20, (int)panel.y + 396, 13, (Color){165, 165, 185, 255});
                DrawText("Trim is non-destructive: the full sample stays loaded; playback uses the range.",
                         (int)panel.x + 20, (int)panel.y + 416, 12, (Color){170, 145, 105, 255});

                if (UIButton({panel.x + 20, panel.y + panel.height - 40, 120, 28}, "Empty slot", 12)) {
                    clearSlot(editorSlot);
                    editorOpen = false;
                }
                if (UIButton({panel.x + panel.width - 232, panel.y + panel.height - 40, 100, 28}, "Apply")) {
                    g_engine.getInstrumentBank().SetAudioTrim(editorSlot, (unsigned long long)edStart, (unsigned long long)edEnd);
                    g_slotTrimStart[editorSlot] = edStart;
                    g_slotTrimLen[editorSlot] = (edEnd >= total) ? 0 : (edEnd - edStart);
                    SetStatus("Sample %s trimmed to %.3fs (full sample kept)", slotLabel(editorSlot), (edEnd - edStart) / sr);
                    editorOpen = false;
                }
                if (UIButton({panel.x + panel.width - 344, panel.y + panel.height - 40, 106, 28}, "Reset trim", 12)) {
                    g_engine.getInstrumentBank().ClearTrim(editorSlot);
                    g_slotTrimStart[editorSlot] = 0;
                    g_slotTrimLen[editorSlot] = 0;
                    edStart = 0; edEnd = total;
                    SetStatus("Trim reset - full sample %s", slotLabel(editorSlot));
                }
                if (UIButton({panel.x + panel.width - 122, panel.y + panel.height - 40, 100, 28}, "Close") ||
                    IsKeyPressed(KEY_ESCAPE)) {
                    editorOpen = false;
                }
            }
        }
        // Whenever the editor is not open, make sure preview audio is stopped
        // (covers every close path: buttons, ESC, Apply, visual swaps...).
        if (!editorOpen) stopEdPreview();

        // --- Pantalla inicial ---
        if (startupOpen) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 200});
            Rectangle panel = {(float)(screenWidth / 2 - 300), 140, 600, 400};
            DrawRectangleRec(panel, (Color){26, 27, 40, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});

            const char* title = "PINGUUS";
            DrawText(title, (int)(panel.x + (panel.width - MeasureText(title, 42)) / 2), (int)panel.y + 26, 42, (Color){255, 210, 100, 255});
            const char* sub = "Plunderphonics Painter";
            DrawText(sub, (int)(panel.x + (panel.width - MeasureText(sub, 18)) / 2), (int)panel.y + 74, 18, RAYWHITE);

            const char* prompt = "New canvas - pick a grid size:";
            DrawText(prompt, (int)(panel.x + (panel.width - MeasureText(prompt, 16)) / 2), (int)panel.y + 130, 16, (Color){165, 165, 185, 255});

            for (int i = 0; i < 4; i++) {
                Rectangle r = {panel.x + 60 + i * 125, panel.y + 165, 110, 56};
                const char* lbl = TextFormat("%d x %d", kGridPresetW[i], kGridPresetH[i]);
                if (UIButton(r, lbl, 16)) {
                    g_gridW = kGridPresetW[i];
                    g_gridH = kGridPresetH[i];
                    RequestSetGridSize(g_gridW, g_gridH);
                    g_scenes.clear();
                    g_scenes.emplace_back();
                    g_curScene = 0;
                    g_mirror = g_scenes[0].cells;
                    clearUndoHistory();
                    startupOpen = false;
                    SetStatus("New %dx%d canvas - paint away!", g_gridW, g_gridH);
                }
                if (kGridPresetW[i] == 32) {
                    DrawText("default", (int)r.x + 30, (int)r.y + 58, 11, (Color){120, 230, 140, 255});
                }
            }

            const char* orTxt = "- or -";
            DrawText(orTxt, (int)(panel.x + (panel.width - MeasureText(orTxt, 14)) / 2), (int)panel.y + 258, 14, (Color){120, 120, 145, 255});

            if (UIButton({panel.x + panel.width / 2 - 90, panel.y + 285, 180, 40}, "Load project...", 15)) {
                char path[512] = "project.smt";
                int r = NativeOpenDialog("Load project...", "*.smt", path, sizeof(path));
                if (r != 0) {
                    loadProject(path);
                    startupOpen = false;
                }
            }

            const char* note = "Load clips & samples from anywhere: drop files or click an empty slot.";
            DrawText(note, (int)(panel.x + (panel.width - MeasureText(note, 12)) / 2), (int)panel.y + 355, 12, (Color){120, 120, 145, 255});
        }

        // ---------------- Aviso de ffmpeg ----------------
        // Lo último que se dibuja, para que quede por encima de cualquier panel:
        // sin ffmpeg no entra ningún vídeo que no sea .mpg, ni por el diálogo ni
        // arrastrándolo, y callárselo es lo que hacía que pareciera roto.
        if (g_ffmpegState == 0) {
            std::string fnote = FfmpegNote();
            int barH = 30;
            DrawRectangle(0, screenHeight - barH, screenWidth, barH, (Color){70, 40, 30, 240});
            DrawRectangle(0, screenHeight - barH, screenWidth, 1, (Color){225, 150, 150, 255});
            DrawText("ffmpeg is missing - videos (except .mpg) will not load and you cannot export video.",
                     10, screenHeight - barH + 9, 12, (Color){235, 195, 165, 255});

            int bx = screenWidth - 250;
#if defined(_WIN32)
            if (g_ffmpegBusy) {
                DrawText(fnote.empty() ? "working..." : fnote.c_str(),
                         bx - 60, screenHeight - barH + 10, 11, (Color){235, 210, 130, 255});
            } else if (UIButton({(float)bx, (float)(screenHeight - barH + 4), 150, 22},
                                "Install ffmpeg for me", 11)) {
                InstallFfmpegWindowsAsync();
            }
#else
            if (UIButton({(float)bx, (float)(screenHeight - barH + 4), 150, 22}, "Copy install command", 11)) {
                SetClipboardText(FfmpegFixCommand());
                SetFfmpegNote("command copied - paste it in a terminal");
            }
#endif
            if (!fnote.empty() && !g_ffmpegBusy)
                DrawText(fnote.substr(0, 40).c_str(), bx - 250, screenHeight - barH + 10, 11,
                         (Color){170, 200, 235, 255});
            if (UIButton({(float)(screenWidth - 90), (float)(screenHeight - barH + 4), 80, 22}, "Re-check", 11))
                DetectFfmpeg();
        }

        // --- Modal de cierre: qué hacer con el proyecto y con lo grabado ---
        //
        // Son DOS decisiones distintas y aquí se pueden tomar por separado: se
        // puede querer quedarse con los clips y no con el proyecto (te llevas
        // el material), o con las dos cosas, o con ninguna. Va el último de
        // todo el dibujado para quedar por encima de cualquier otro panel.
        if (quitDialogOpen) {
            DrawRectangle(0, 0, screenWidth, screenHeight, (Color){0, 0, 0, 200});
            Rectangle panel = {(float)(screenWidth / 2 - 270), 210, 540, 290};
            DrawRectangleRec(panel, (Color){26, 27, 40, 255});
            DrawRectangleLinesEx(panel, 2, (Color){110, 112, 140, 255});

            const char* title = "Close Pinguus?";
            DrawText(title, (int)(panel.x + (panel.width - MeasureText(title, 20)) / 2), (int)panel.y + 16, 20, RAYWHITE);

            int nClips = (int)g_sessionClips.size();
            const char* sub = nClips == 0
                ? "Nothing was recorded this session."
                : TextFormat("%d clip%s recorded this session, waiting in %s/",
                             nClips, nClips == 1 ? "" : "s", kTempDir);
            DrawText(sub, (int)(panel.x + (panel.width - MeasureText(sub, 13)) / 2), (int)panel.y + 46, 13,
                     nClips ? (Color){190, 230, 200, 255} : (Color){150, 150, 170, 255});

            // Guardar el proyecto: el .smt va donde el usuario elija; los clips
            // se quedan donde están (en temp/), que es a lo que apunta el .smt.
            if (UIButton({panel.x + 30, panel.y + 82, 480, 40}, "YES - save project (.smt) and keep the clips", 13)) {
                char path[512] = "project.smt";
                int r = NativeSaveDialog("Save project as...", "project.smt", path, sizeof(path));
                if (r != 0) {
                    if (r == 1) EnsureExtension(path, sizeof(path), ".smt");
                    saveProject(path);
                    quitNow = true;
                } else {
                    SetStatus("Save cancelled - still open");
                }
            }
            if (UIButton({panel.x + 30, panel.y + 130, 480, 40},
                         nClips ? TextFormat("Keep only the %d clip%s - no project file", nClips, nClips == 1 ? "" : "s")
                                : "Quit without saving the project", 13)) {
                quitNow = true;
            }
            if (UIButton({panel.x + 30, panel.y + 178, 480, 40},
                         nClips ? TextFormat("NO - delete this session's %d clip%s and quit", nClips, nClips == 1 ? "" : "s")
                                : "NO - quit without saving", 13)) {
                int n = discardSessionClips();
                printf("Pinguus: %d grabacion(es) de esta sesion borradas de %s/\n", n, kTempDir);
                quitNow = true;
            }
            if (UIButton({panel.x + panel.width / 2 - 70, panel.y + 232, 140, 34}, "Cancel", 13) ||
                IsKeyPressed(KEY_ESCAPE)) {
                quitDialogOpen = false;
            }
            DrawText("Clips from earlier sessions in temp/ are never touched.",
                     (int)panel.x + 30, (int)panel.y + 272, 11, (Color){140, 140, 160, 255});
        }

#ifdef UI_SMOKE_TEST
        {
            static double tFrame0 = 0.0;
            const double now = GetTime();
            if (tFrame0 > 0.0) {
                const double whole = now - tFrame0;
                g_tUi += whole - (g_tCollage + g_tCapture - g_tPrevPhases);
                g_sFrame.push_back(whole);
            }
            g_tPrevPhases = g_tCollage + g_tCapture;
            tFrame0 = now;
            g_frames++;
        }
#endif
        EndDrawing();
    }

    if (recording) stopRecording();

    for (auto& pair : textureCache) {
        UnloadTexture(pair.second);
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slotShaderOn[i]) UnloadShader(g_slotShader[i]);
    }
    for (int i = 0; i < MAX_MODELS; i++) UnloadModelSlot(i);
    if (g_lightShader.id != 0) UnloadShader(g_lightShader);
    if (g_toonShader.id != 0) UnloadShader(g_toonShader);
    UnloadRenderTexture(modelPreviewRT);
    UnloadRenderTexture(collageRT);
    UnloadRenderTexture(postRT);
    g_ntscShader.unload();

    g_phone.stop();
    g_apkServer.stop();
    // La descarga de ffmpeg puede seguir viva: un std::thread joinable que se
    // destruye llama a terminate(), o sea que el programa se cerraría de golpe
    // justo al salir.
    if (g_ffmpegThread.joinable()) g_ffmpegThread.join();
    g_ipcam.stopPreview();
    g_ipcam.joinScan();
    if (phonePrevTex.id) UnloadTexture(phonePrevTex);

    ma_device_uninit(&device);
    g_scriptCtx = nullptr;
    g_scripts.shutdown();
    if (g_midiOut) delete g_midiOut;
    if (g_midiIn) delete g_midiIn;

    CloseWindow();
    return 0;
}
