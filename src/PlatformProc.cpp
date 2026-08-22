// Implementación de PlatformProc.h. Ver esa cabecera para el porqué de que
// esto viva aparte de main.cpp (choque de nombres entre windows.h y raylib).
#include "PlatformProc.h"

#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
#else
  #include <sys/wait.h>
  #include <sys/mman.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <csignal>
#endif

// ---------------------------------------------------------------------------
// Ejecutar comandos
// ---------------------------------------------------------------------------
#if defined(_WIN32)

int RunCommand(const char* cmd) {
    std::string line = std::string("cmd.exe /C ") + cmd;
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int RunCommandTimeout(const char* cmd, int timeoutSeconds) {
    std::string line = std::string("cmd.exe /C ") + cmd;
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)timeoutSeconds * 1000);
    DWORD code = 1;
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        code = (DWORD)RUNCMD_TIMED_OUT;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int RunCommandRead(const char* cmd, std::string& out) {
    out.clear();
    SECURITY_ATTRIBUTES sa; ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // Entrada estándar: el DISPOSITIVO NUL, nunca GetStdHandle().
    // Pinguus se compila con -mwindows, o sea sin consola, así que
    // GetStdHandle(STD_INPUT_HANDLE) devuelve NULL. Y al pedir
    // STARTF_USESTDHANDLES, ese NULL llega al hijo como un descriptor
    // inválido: los programas que tocan su stdin al arrancar (powershell, sin
    // ir más lejos) se caen ahí mismo, antes de hacer nada. De ahí venía que
    // en Windows la detección de Tailscale no encontrase nunca nada.
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, NULL);

    std::string line = std::string("cmd.exe /C ") + cmd;
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return -1;
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    // Cerramos NUESTRA punta de escritura: si no, ReadFile nunca ve el fin
    // del flujo y el programa se queda colgado esperando para siempre.
    CloseHandle(wr);

    char chunk[1024];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk) - 1, &n, NULL) && n > 0) {
        chunk[n] = '\0';
        out += chunk;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

#else   // Linux / macOS

int RunCommand(const char* cmd) { return system(cmd); }

int RunCommandTimeout(const char* cmd, int timeoutSeconds) {
    // system() no sirve aquí: no devuelve el PID, así que un hijo colgado no se
    // puede matar. Con fork+exec sí, y el proceso va en su propio grupo para
    // llevarse por delante también a lo que él haya lanzado.
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char*)nullptr);
        _exit(127);
    }
    // Sondeo cada 100 ms: sencillo, y a esta escala (segundos) sobra de largo.
    const int ticks = timeoutSeconds * 10;
    for (int i = 0; i < ticks; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
        if (r < 0) return -1;
        usleep(100000);
    }
    // Se acabó el tiempo. TERM al grupo, y si no se va por las buenas, KILL.
    kill(-pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) return RUNCMD_TIMED_OUT;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return RUNCMD_TIMED_OUT;
}

int RunCommandRead(const char* cmd, std::string& out) {
    out.clear();
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
}

#endif

// ---------------------------------------------------------------------------
// Diálogos de archivo nativos (Windows). Ver el porqué en PlatformProc.h.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

static std::wstring Widen(const char* s) {
    if (s == NULL || *s == '\0') return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static void Narrow(const wchar_t* w, char* out, std::size_t outSz) {
    out[0] = '\0';
    if (w == NULL) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outSz, NULL, NULL);
    out[outSz - 1] = '\0';
}

// comdlg32 quiere el filtro como cadenas terminadas en '\0' pegadas una detrás
// de otra y rematadas con otro '\0': "Descripción\0*.a;*.b\0Todos\0*.*\0\0".
// Y separa los patrones por PUNTO Y COMA, no por espacios, que es como los
// tiene el resto del programa — traducirlo es justo lo que faltaba.
static std::vector<wchar_t> BuildFilter(const char* extPattern) {
    std::wstring pats = Widen(extPattern);
    for (size_t i = 0; i < pats.size(); i++) if (pats[i] == L' ') pats[i] = L';';

    std::vector<wchar_t> f;
    auto push = [&f](const std::wstring& s) {
        f.insert(f.end(), s.begin(), s.end());
        f.push_back(L'\0');
    };
    if (!pats.empty()) {
        push(L"Supported files (" + pats + L")");
        push(pats);
    }
    push(L"All files (*.*)");
    push(L"*.*");
    f.push_back(L'\0');          // el cierre doble
    return f;
}

// El diálogo se cuelga de la ventana activa de ESTE hilo, que es la de Pinguus
// (se llama siempre desde el hilo de la interfaz). Sin dueño, Windows lo puede
// dejar DETRÁS de la ventana del programa: se abre, pero no lo ves, que es
// indistinguible de que no se abra.
static HWND DialogOwner() { return GetActiveWindow(); }

int NativeOpenDialogWin(const char* title, const char* extPattern, char* out, std::size_t outSz) {
    std::vector<wchar_t> filter = BuildFilter(extPattern);
    std::wstring wtitle = Widen(title);
    std::vector<wchar_t> file(4096, L'\0');

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = DialogOwner();
    ofn.lpstrFilter = filter.data();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = wtitle.empty() ? NULL : wtitle.c_str();
    // OFN_NOCHANGEDIR es obligatorio aquí: Pinguus busca assets/ y mods/ en el
    // directorio ACTUAL, así que un diálogo que lo cambie rompe todo lo que
    // venga después.
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        // 0 con error 0 = el usuario canceló; con error = no se pudo abrir.
        return CommDlgExtendedError() == 0 ? 0 : -1;
    }
    Narrow(file.data(), out, outSz);
    return out[0] != '\0' ? 1 : 0;
}

int NativeSaveDialogWin(const char* title, const char* defaultName, char* out, std::size_t outSz) {
    // El nombre propuesto trae la extensión buena; se reutiliza como filtro
    // para que el desplegable enseñe algo con sentido.
    std::string pat = "*.*";
    const char* dot = defaultName != NULL ? strrchr(defaultName, '.') : NULL;
    if (dot != NULL && dot[1] != '\0') pat = std::string("*") + dot;

    std::vector<wchar_t> filter = BuildFilter(pat.c_str());
    std::wstring wtitle = Widen(title);
    std::wstring wname = Widen(defaultName);
    std::vector<wchar_t> file(4096, L'\0');
    if (!wname.empty() && wname.size() < file.size())
        memcpy(file.data(), wname.c_str(), (wname.size() + 1) * sizeof(wchar_t));
    // Tiene que sobrevivir hasta después de GetSaveFileNameW: si se construye
    // dentro de la asignación de lpstrDefExt, el temporal muere en el punto y
    // coma y la estructura se queda con un puntero colgando.
    std::wstring wdefExt = (dot != NULL && dot[1] != '\0') ? Widen(dot + 1) : std::wstring();

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = DialogOwner();
    ofn.lpstrFilter = filter.data();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = wtitle.empty() ? NULL : wtitle.c_str();
    ofn.lpstrDefExt = wdefExt.empty() ? NULL : wdefExt.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        return CommDlgExtendedError() == 0 ? 0 : -1;
    }
    Narrow(file.data(), out, outSz);
    return out[0] != '\0' ? 1 : 0;
}

#else   // Linux / macOS: main.cpp usa kdialog / zenity / osascript.

int NativeOpenDialogWin(const char*, const char*, char* out, std::size_t) { out[0] = '\0'; return -1; }
int NativeSaveDialogWin(const char*, const char*, char* out, std::size_t) { out[0] = '\0'; return -1; }

#endif

// ---------------------------------------------------------------------------
// Memoria compartida de la ventana LIVE
// ---------------------------------------------------------------------------
unsigned char* LiveShmMap(bool create, std::size_t size) {
#if defined(_WIN32)
    HANDLE h = create
                   ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, (DWORD)size, "pinguus_live")
                   : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "pinguus_live");
    if (h == NULL) return nullptr;
    return (unsigned char*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    int fd = shm_open("/pinguus_live", create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
    if (fd < 0) return nullptr;
    if (create && ftruncate(fd, (off_t)size) != 0) { close(fd); return nullptr; }
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    return (unsigned char*)p;
#endif
}

void LiveShmUnmap(unsigned char* mem, std::size_t size) {
    if (!mem) return;
#if defined(_WIN32)
    (void)size;
    UnmapViewOfFile(mem);
#else
    munmap(mem, size);
#endif
}
