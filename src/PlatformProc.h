// ============================================================================
// PlatformProc.h — Lanzar programas auxiliares (ffmpeg, diálogos, pactl...)
// y abrir la memoria compartida de la ventana LIVE.
//
// POR QUÉ ESTÁ EN SU PROPIO .CPP
//   La implementación en Windows necesita <windows.h> ENTERO. Pero windows.h
//   declara Rectangle(), DrawText(), CloseWindow(), LoadImage()... con los
//   mismos nombres que la API de raylib, así que incluirlo junto a raylib.h
//   rompe la compilación (main.cpp deja de poder declarar un Rectangle).
//   main.cpp resuelve su lado definiendo NOGDI/NOUSER, lo que deja fuera
//   justamente las partes de windows.h que ESTE código necesita. Separarlo es
//   la salida limpia: aquí se ve windows.h completo y nunca se ve raylib.
//
//   (Este choque de nombres es la razón por la que el ejecutable de Windows no
//   compilaba; se descubrió al cruzar el build con mingw desde Linux.)
//
// EN WINDOWS, además, los procesos se lanzan con CREATE_NO_WINDOW: sin eso,
// cada llamada a ffmpeg hace parpadear una ventana negra de consola por encima
// de la aplicación.
// ============================================================================
#ifndef PINGUUS_PLATFORMPROC_H
#define PINGUUS_PLATFORMPROC_H

#include <string>
#include <cstddef>

// Ejecuta un comando de shell y espera. Devuelve su código de salida
// (-1 si ni siquiera se pudo lanzar).
int RunCommand(const char* cmd);

// Igual, pero además recoge lo que el programa escriba por stdout+stderr.
int RunCommandRead(const char* cmd, std::string& out);

// Código que devuelve RunCommandTimeout cuando ha tenido que matar al hijo.
// (Un valor que ningún programa normal usa como salida.)
#define RUNCMD_TIMED_OUT 124

// Como RunCommand, pero MATA el proceso si pasa de timeoutSeconds de reloj.
//
// Existe porque `ffmpeg -t 4` limita la duración de SALIDA, no el tiempo real:
// grabando de una webcam virtual sin nadie emitiendo al otro lado, ffmpeg se
// queda esperando fotogramas que no llegan y no termina NUNCA. Se vio uno vivo
// 72 minutos por una grabación de 4 segundos, reteniendo /dev/video9 y dejando
// a Pinguus congelado dentro de system(). Con esto, el peor caso es esperar el
// límite y recibir RUNCMD_TIMED_OUT.
int RunCommandTimeout(const char* cmd, int timeoutSeconds);

// ---------------------------------------------------------------------------
// Diálogos de archivo NATIVOS de Windows (comdlg32).
//
// Antes esto se hacía lanzando powershell con un OpenFileDialog de WinForms, y
// en Windows no se abría NADA: los procesos se crean con STARTF_USESTDHANDLES
// y, en una aplicación compilada con -mwindows, GetStdHandle(STD_INPUT_HANDLE)
// devuelve NULL — powershell arranca con una entrada estándar inválida y se
// muere antes de enseñar el diálogo. Encima el filtro se pasaba separado por
// espacios cuando comdlg32 exige ';', así que ni siquiera habría listado los
// archivos. Llamar directamente a GetOpenFileNameW/GetSaveFileNameW quita el
// intermediario, el problema de comillas y la dependencia de powershell.
//
// `extPattern` es la misma lista separada por espacios que ya usa el resto del
// programa ("*.wav *.mp3"); aquí se traduce al formato de comdlg32.
// Devuelven 1 si el usuario eligió un archivo, 0 si canceló, -1 si el diálogo
// no se pudo abrir. Fuera de Windows devuelven siempre -1 (main.cpp usa
// kdialog/zenity/osascript).
// ---------------------------------------------------------------------------
int NativeOpenDialogWin(const char* title, const char* extPattern, char* out, std::size_t outSz);
int NativeSaveDialogWin(const char* title, const char* defaultName, char* out, std::size_t outSz);

// Memoria compartida de la salida LIVE (la ventana espejo del collage).
// `create` = true en el proceso principal, false en el proceso --live.
// Devuelve el puntero al bloque, o nullptr si no se pudo mapear.
unsigned char* LiveShmMap(bool create, std::size_t size);
void           LiveShmUnmap(unsigned char* mem, std::size_t size);

#endif // PINGUUS_PLATFORMPROC_H
