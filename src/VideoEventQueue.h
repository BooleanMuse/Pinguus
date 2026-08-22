#pragma once
// VideoEventQueue.h
//
// Cola lock-free SPSC, hermana de CommandQueue.h pero en la dirección
// OPUESTA: aquí el único productor es el hilo de AUDIO y el único
// consumidor es el hilo PRINCIPAL (Raylib).
//
// Por qué existe: el hilo de audio sigue siendo, igual que antes, el único
// dueño de "qué bicho está sonando, con qué pitch, en qué celda". Pero
// ahora que un bicho puede tener un InstrumentSource con VIDEO, alguien
// tiene que decirle al hilo principal "este bicho con este instrumento
// empezó/sigue sonando a esta velocidad de reproducción" para que el panel
// derecho pueda animar el frame de video correspondiente — sin que el hilo
// principal tenga que adivinar leyendo el snapshot de posiciones (que no
// lleva información de pitch ni de qué instrumento es).
//
// Igual que CommandQueue, un único hilo de audio escribe (dentro de
// AudioCallback) y un único hilo principal lee (dentro del loop de
// Raylib). Si en el futuro hubiera más de un hilo de audio o más de un
// consumidor, esta cola dejaría de ser segura.
//
// REGLA DE ORO (la misma que ya conoces de CommandQueue.h): el hilo de
// audio nunca debe esperar. push() nunca bloquea; si la cola está llena,
// el evento más viejo se descarta en vez de arriesgar una espera.

#include <atomic>
#include <cstddef>

enum class VideoEventType : unsigned char {
    None = 0,
    VoiceStarted,   // un bicho con video empezó a sonar: asigna/reusa una VideoVoice
    VoiceStopped,   // el bicho dejó de sonar (sample agotado o silenciado): libera la voice
};

struct VideoEvent {
    VideoEventType type = VideoEventType::None;

    // Identifica DE FORMA ESTABLE la voz de reproducción a lo largo de su
    // vida (desde VoiceStarted hasta su VoiceStopped correspondiente). Se
    // usa el índice del Bicho en el pool — ya es único mientras el bicho
    // esté activo, así no hace falta inventar otro espacio de ids.
    int bichoIndex = -1;

    int sampleId = 0;       // qué slot de InstrumentBank usar para los frames
    float playbackRate = 1.0f; // misma tasa que el cursor de audio del bicho
    bool reverse = false;   // igual semántica que directionSign<0 en SequencerEngine
};

template <size_t Capacity>
class VideoEventQueue {
public:
    VideoEventQueue() : head(0), tail(0) {}

    // Llamado SOLO desde el hilo de audio (productor).
    bool push(const VideoEvent& ev) {
        const size_t currentTail = tail.load(std::memory_order_relaxed);
        const size_t nextTail = (currentTail + 1) % Capacity;

        if (nextTail == head.load(std::memory_order_acquire)) {
            return false; // cola llena: se descarta, igual que CommandQueue::push
        }

        buffer[currentTail] = ev;
        tail.store(nextTail, std::memory_order_release);
        return true;
    }

    // Llamado SOLO desde el hilo principal (consumidor).
    bool pop(VideoEvent& out) {
        const size_t currentHead = head.load(std::memory_order_relaxed);

        if (currentHead == tail.load(std::memory_order_acquire)) {
            return false; // cola vacía
        }

        out = buffer[currentHead];
        head.store((currentHead + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    VideoEvent buffer[Capacity];
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
};
