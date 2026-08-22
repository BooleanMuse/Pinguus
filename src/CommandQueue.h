#pragma once
// CommandQueue.h
//
// Cola lock-free de un solo productor (hilo principal) y un solo consumidor
// (hilo de audio). Implementada como un buffer circular con dos índices
// atómicos. No usa mutex, no usa malloc en tiempo real, no se bloquea nunca.
//
// REGLA DE ORO: el hilo de audio NUNCA debe esperar. Un mutex, una llamada a
// malloc/new, o cualquier I/O dentro del callback de audio puede causar que
// el sistema operativo posponga ese hilo unos milisegundos — suficiente para
// que se oiga como un clic o un silencio. Por eso esta cola usa solo
// operaciones atómicas con la semántica de memoria mínima necesaria
// (acquire/release), nunca un lock.
//
// Por qué SPSC (no MPMC): solo hay un hilo escribiendo (el principal, al
// reaccionar a teclado/mouse) y uno leyendo (el de audio, dentro del
// callback). Una cola SPSC es más simple y más rápida que una genérica
// multi-productor; si en el futuro se agregan más hilos escritores, esta
// cola dejaría de ser segura y habría que migrar a otra implementación.

#include <atomic>
#include <cstring>

enum class CommandType : unsigned char {
    None = 0,
    PaintCell,        // pintar/asignar un módulo a una celda del grid
    SpawnBicho,       // crear un bicho en una celda con una dirección y sample
    SetTempo,         // cambiar el BPM
    RemoveBicho,      // eliminar un bicho activo (por índice de pool)
    ClearGrid,        // vaciar todas las celdas del grid
    ClearBichos,      // desactivar todos los bichos del pool
    SetPaused,        // pausar/reanudar el secuenciador (bichos y sonido)
    SetGridSize,      // cambiar el tamaño del grid (al crear/cargar proyecto)
    SetTrackerCell,   // escribir una celda del tracker (canal, fila, nota)
    ClearTracker,     // vaciar todo el patrón del tracker
    ResetPlayhead,    // tracker vuelve a la fila 0 (para grabar desde el inicio)
    PreviewPlay,      // reproducir el audio de un slot en [a0,a1) en bucle (editor de trim)
    PreviewStop,      // detener la reproducción de preview
    SetBichoMuted,    // silenciar/activar un notey (sigue moviéndose, no dispara)
    SetMasterVolume,  // volumen general de la salida
    SetBichoVolume,   // volumen de un notey concreto
    SetLinearCell,    // escribir una celda del modo lineal (columna, fila, nota)
    ClearLinear,      // vaciar todas las celdas del modo lineal
    SetLinearParams,  // largo del loop y flag de loop del modo lineal
    SetBichoStopped,  // parar/reanudar un notey concreto (no avanza ni suena)
    TriggerLive,      // tocar un slot al tono dado en una voz "en vivo" (MIDI/gamepad)
    TriggerControllable, // igual, pero en la voz dedicada del Notey Controlable (retrigger)
    PaintCompound,    // pintar una celda COMPUESTA (lista de acciones, Fase 3)
    // --- Modo BEATBOX (sampleadora de pads) ---
    PadTrigger,       // golpear un pad (con velocidad)
    PadRelease,       // soltar un pad (solo hace algo en modo GATE)
    PadStopAll,       // callar todos los pads de golpe
    SetPadConfig,     // slot, tono, volumen, modo, grupo de corte y fx de un pad
    SetPadTransport,  // patrón: reproducir / grabar / cuantizar / largo
    ClearPadPattern,  // vaciar el patrón entero (o solo la pista de un pad)
    SetPadStep,       // poner/quitar un golpe en un tick concreto del patrón
    SetMasterFx,      // efecto maestro en vivo: tipo, dos mandos y on/off
};

// Estructura de comando: union manual de los parámetros posibles. Se eligió
// un tamaño fijo (sin punteros a memoria recién asignada) para que encolar
// un comando nunca dispare una asignación dinámica.
struct Command {
    CommandType type = CommandType::None;

    // Usados por PaintCell
    int cellX = 0;
    int cellY = 0;
    unsigned char colorId = 0;
    int modifier = 0;       // se castea a ModifierType en el consumidor
    int targetX = 0;
    int targetY = 0;
    int nextDx = 0;
    int nextDy = 0;
    unsigned char midiNote = 0;
    float sustainSeconds = 1.5f; // duración de la pausa en celdas Sustain
    // Atributos superpuestos de la celda (ver GridCell): espera, volumen y
    // velocidad de la nota. Neutros por defecto, así que quien no los rellene
    // pinta exactamente igual que antes.
    float holdSeconds = 0.0f;
    float cellVolMul = 1.0f;
    float cellTimeMul = 1.0f;
    int sampleId = 0;       // qué slot de InstrumentBank queda asociado a esta celda
                             // (también reusado por SpawnBicho más abajo — mismo campo,
                             // mismo significado: "qué instrumento del banco corresponde")

    // Usados por SpawnBicho
    int spawnX = 0;
    int spawnY = 0;
    int spawnDx = 1;
    int spawnDy = 0;
    float spawnTempoMul = 1.0f; // multiplicador de tempo propio del bicho (x0.25..x4)
    int mutedFlag = 0;          // SpawnBicho: estado inicial | SetBichoMuted: nuevo estado
    int stoppedFlag = 0;        // SetBichoStopped: 1 = parado, 0 = corriendo
    float volumeVal = 1.0f;     // SpawnBicho: volumen inicial | Set*Volume: nuevo valor

    // Usados por SetTempo
    float newBpm = 120.0f;

    // Usados por RemoveBicho
    int bichoIndex = -1;

    // Usados por SetPaused (1 = pausado, 0 = corriendo)
    int pausedState = 0;

    // Usados por SetGridSize
    int newGridW = 32;
    int newGridH = 24;

    // Usados por SetTrackerCell (sampleId=-1 borra la celda; colorId y
    // modifier se reusan: pitch y efecto 0=none/1=reverb/2=echo/3=reverse)
    int trkChannel = 0;
    int trkRow = 0;

    // Usados por PreviewPlay: rango de audio [a0,a1) en muestras (sampleId = slot)
    long long previewA0 = 0;
    long long previewA1 = 0;

    // Usados por SetLinearCell (sampleId=-1 borra la celda; colorId y modifier
    // se reusan igual que en el tracker: pitch y efecto). SetLinearParams usa
    // linLen (columnas del loop) y linLoop (1=loop, 0=una sola pasada).
    int linCol = 0;
    int linRow = 0;
    int linLen = 32;
    int linLoop = 1;

    // Usados por PaintCompound (Fase 3): hasta 4 acciones para la celda.
    int compCount = 0;
    unsigned char compType[4] = {0, 0, 0, 0};
    int compA[4] = {0, 0, 0, 0};
    int compB[4] = {0, 0, 0, 0};
    float compF[4] = {0, 0, 0, 0};

    // Usados por los comandos del modo BEATBOX. `padIndex` es el pad GLOBAL
    // (banco*16 + pad), no el pad dentro del banco visible.
    int   padIndex = 0;
    float padVelocity = 1.0f;   // PadTrigger: 0..1
    int   padPitch = 0;         // SetPadConfig: semitonos
    float padVol = 1.0f;        // SetPadConfig
    int   padMode = 0;          // SetPadConfig: 0 oneshot, 1 gate, 2 loop
    int   padChoke = 0;         // SetPadConfig: 0 = sin grupo, 1..4
    int   padFx = 0;            // SetPadConfig: 0 none,1 reverb,2 echo,3 reverse,4 chorus
    int   padTick = 0;          // SetPadStep
    int   padOn = 0;            // SetPadStep: 1 poner, 0 quitar
    int   padPlayFlag = 0;      // SetPadTransport: 1 = el patrón corre
    int   padRecFlag = 0;       // SetPadTransport: 1 = grabando
    int   padQuant = 0;         // SetPadTransport: 0 off, 1 1/16, 2 1/8, 3 1/4, 4 1/32
    int   padSteps = 16;        // SetPadTransport: largo del bucle
    int   mfxType = 0;          // SetMasterFx
    float mfxX = 0.5f, mfxY = 0.5f;
    int   mfxOn = 0;
};

template <size_t Capacity>
class CommandQueue {
public:
    CommandQueue() : head(0), tail(0) {}

    // Llamado SOLO desde el hilo principal (productor).
    // Devuelve false si la cola está llena (el comando se descarta;
    // el llamador puede decidir reintentar en el siguiente frame).
    bool push(const Command& cmd) {
        const size_t currentTail = tail.load(std::memory_order_relaxed);
        const size_t nextTail = (currentTail + 1) % Capacity;

        // Si nextTail alcanzara a head, la cola está llena.
        if (nextTail == head.load(std::memory_order_acquire)) {
            return false;
        }

        buffer[currentTail] = cmd;
        // release: garantiza que la escritura de 'buffer' arriba sea visible
        // para el hilo de audio ANTES de que vea el nuevo valor de 'tail'.
        tail.store(nextTail, std::memory_order_release);
        return true;
    }

    // Llamado SOLO desde el hilo de audio (consumidor).
    // Devuelve false si no hay comandos pendientes.
    bool pop(Command& out) {
        const size_t currentHead = head.load(std::memory_order_relaxed);

        if (currentHead == tail.load(std::memory_order_acquire)) {
            return false; // cola vacía
        }

        out = buffer[currentHead];
        head.store((currentHead + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    Command buffer[Capacity];
    std::atomic<size_t> head; // próximo índice a leer (consumidor)
    std::atomic<size_t> tail; // próximo índice a escribir (productor)
};
