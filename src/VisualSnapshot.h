#pragma once
// VisualSnapshot.h
//
// El hilo de audio es ahora el único dueño del estado real de los bichos
// (posición, si están sonando, etc.). Pero el hilo principal necesita ESA
// información para dibujar el grid en pantalla. Como no puede leer
// directamente el estado del hilo de audio sin arriesgar una carrera de
// datos, el hilo de audio publica periódicamente una "foto" (snapshot) de
// las posiciones, y el hilo principal solo lee la foto más reciente.
//
// Técnica: doble buffer con un índice atómico que indica cuál de los dos
// buffers es el "actual" (completo y seguro de leer). El hilo de audio
// escribe siempre en el buffer que NO está marcado como actual, y al
// terminar de escribir, voltea el índice. El hilo principal lee el índice,
// y copia ese buffer — nunca hay una escritura a medias visible para el lector.

#include <atomic>

#define MAX_BICHOS_SNAPSHOT 1024

struct BichoVisual {
    bool isActive = false;
    int x = 0;
    int y = 0;
    bool isPlaying = false;
    int poolIndex = -1; // índice real en el pool del motor — permite que la UI
                        // pida RemoveBicho de un bicho concreto (borrado por click)
    int sampleId = 0;   // qué instrumento usa, para colorearlo en la UI
    bool muted = false; // silenciado: se mueve pero no dispara clips/samples
    bool stopped = false; // parado: congelado en su sitio (play/stop individual)
};

// Modo BEATBOX: el patrón se copia entero en cada foto. Son 2 KB (256 ticks x
// una máscara de 64 bits), unos 170 KB/s a tamaños de búfer normales — nada
// comparado con lo que ya se copia de bichos. A cambio, el patrón lo GRABA el
// hilo de audio con su propio reloj (que es el que sabe exactamente cuándo
// sonó el golpe) y la interfaz no necesita mantener una copia paralela que
// pudiera desincronizarse.
#define SNAP_PAD_TICKS 256

struct SnapshotBuffer {
    BichoVisual bichos[MAX_BICHOS_SNAPSHOT];
    int count = 0;
    int trackerRow = -1; // fila actual del tracker (para dibujar el playhead)
    int linearCol = -1;  // columna actual del modo lineal (playhead horizontal)
    bool previewActive = false;      // el editor está reproduciendo audio
    long long previewCursor = 0;     // muestra actual de la reproducción de preview

    // --- Modo BEATBOX ---
    unsigned long long padPlaying = 0;   // bit N = el pad global N está sonando
    unsigned long long padLooping = 0;   // bit N = ese pad está en bucle (para el borde)
    int  padTick = -1;                   // tick del patrón (playhead); -1 = parado
    int  padSteps = 16;                  // largo del bucle en semicorcheas
    bool padPatPlaying = false;
    bool padRecording = false;
    unsigned int padPatVersion = 0;
    unsigned long long padPattern[SNAP_PAD_TICKS] = {0};
};

class VisualSnapshotPublisher {
public:
    VisualSnapshotPublisher() : currentIndex(0) {}

    // Llamado SOLO desde el hilo de audio, una vez por callback (no por
    // muestra; una vez por buffer es más que suficiente para 60fps de
    // dibujo). Escribe en el buffer inactivo y luego publica el índice.
    SnapshotBuffer& beginWrite() {
        // Escribe siempre en el buffer que NO es el actual.
        int writeIndex = 1 - currentIndex.load(std::memory_order_relaxed);
        return buffers[writeIndex];
    }

    void commitWrite() {
        int writeIndex = 1 - currentIndex.load(std::memory_order_relaxed);
        // release: garantiza que todo lo escrito en buffers[writeIndex] sea
        // visible para el hilo principal antes de que vea el nuevo índice.
        currentIndex.store(writeIndex, std::memory_order_release);
    }

    // Llamado SOLO desde el hilo principal, una vez por frame de dibujo.
    const SnapshotBuffer& read() const {
        int readIndex = currentIndex.load(std::memory_order_acquire);
        return buffers[readIndex];
    }

private:
    SnapshotBuffer buffers[2];
    std::atomic<int> currentIndex;
};
