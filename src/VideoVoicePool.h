#pragma once
// VideoVoicePool.h
//
// Vive ENTERAMENTE en el hilo principal (Raylib). Es el equivalente visual
// de lo que Bicho+renderAudio hacen en el hilo de audio, pero para frames
// de video en vez de muestras PCM: cada VideoVoice activa tiene un cursor
// de frame en punto flotante que avanza cada frame de PANTALLA (no de
// audio) a una velocidad derivada del mismo playbackRate que ya viene
// calculado por SequencerEngine — así que un color que pitch-shiftea el
// audio +700 cents también acelera el video +700 cents "de tiempo", igual
// de espíritu al truco clásico de YTPMV (cambiar velocidad = cambiar tono).
//
// Por qué esto NO vive en el hilo de audio: dibujar (incluso solo subir
// bytes a una textura de Raylib) no es seguro de hacer desde dentro de un
// callback de audio en tiempo real — eso violaría la regla de oro de
// CommandQueue.h de "el hilo de audio nunca debe esperar" (subir una
// textura a la GPU puede bloquear). Por eso el hilo de audio solo AVISA
// (VideoEventQueue) y el hilo principal hace el trabajo real de avance y
// dibujo.
//
// CUPO LIMITADO A PROPÓSITO (MAX_VIDEO_VOICES): aunque el usuario pueda
// colocar tantos bichos con video como quiera (estilo Mario Maker), dibujar
// decenas de videos a la vez tiene un costo real de CPU/GPU (UpdateTexture
// por cada uno, cada frame). Por eso el panel derecho muestra como máximo N
// voces de video a la vez; los bichos con video que excedan el cupo siguen
// sonando con normalidad (su audio no se ve afectado), simplemente no
// obtienen un rectángulo visual hasta que se libere un slot.

#include "VideoEventQueue.h"
#include "InstrumentBank.h"

#define MAX_VIDEO_VOICES 32

struct VideoVoice {
    bool isActive = false;
    int bichoIndex = -1;   // a qué bicho pertenece esta voz (id estable mientras viva)
    int sampleId = 0;      // qué InstrumentSource del banco está reproduciendo
    float frameCursor = 0.0f;
    float frameRate = 1.0f; // multiplicador sobre el framerate ORIGINAL del clip
    bool reverse = false;
    float ageSeconds = 0.0f; // tiempo desde el (re)disparo — anima efectos (zoom/rotación)
    bool flipParity = false; // alterna en cada nota — para el efecto FLIP por nota
};

class VideoVoicePool {
public:
    // Llamado SOLO desde el hilo principal, una vez por frame de pantalla,
    // ANTES de advanceAll(). Drena la cola y abre/cierra voces según
    // corresponda. Recibe el banco para poder inicializar correctamente el
    // cursor de una voz nueva en modo reversa (necesita saber cuántos
    // frames tiene el clip para arrancar en el último, no en un valor a
    // corregir después).
    template <size_t Capacity>
    void consumeEvents(VideoEventQueue<Capacity>& queue, const InstrumentBank& bank) {
        VideoEvent ev;
        while (queue.pop(ev)) handleEvent(ev, bank);
    }

    // Procesa UN evento (para cuando el hilo principal drena la cola él mismo y
    // enruta los eventos de MODELO 3D a otro pool).
    void handleEvent(const VideoEvent& ev, const InstrumentBank& bank) {
        if (ev.type == VideoEventType::VoiceStarted) {
            const InstrumentSource& s = bank.at(ev.sampleId);
            startVoice(ev, s.videoTrimStart(), s.videoTrimEnd());
        } else if (ev.type == VideoEventType::VoiceStopped) {
            stopVoiceForBicho(ev.bichoIndex);
        }
    }

    // Avanza el cursor de frame de cada voz activa. deltaSeconds es el
    // tiempo real transcurrido desde el último frame de pantalla
    // (GetFrameTime() de Raylib).
    void advanceAll(const InstrumentBank& bank, float deltaSeconds) {
        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            VideoVoice& v = voices[i];
            if (!v.isActive) continue;

            const InstrumentSource& src = bank.at(v.sampleId);
            if (!src.hasVideo()) {
                v.isActive = false;
                continue;
            }

            double framesPerSecond = src.videoFramerate * v.frameRate;
            float step = (float)(framesPerSecond * deltaSeconds) * (v.reverse ? -1.0f : 1.0f);
            v.frameCursor += step;
            v.ageSeconds += deltaSeconds;

            // Loop within the (non-destructive) trim range [start,end).
            int ts = src.videoTrimStart(), te = src.videoTrimEnd();
            if (v.frameCursor >= (float)te || v.frameCursor < (float)ts) {
                v.frameCursor = v.reverse ? (float)(te - 1) : (float)ts;
            }
        }
    }

    // Para que main.cpp dibuje: itera únicamente las voces activas.
    template <typename Fn>
    void forEachActiveVoice(Fn&& fn) const {
        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            if (voices[i].isActive) fn(voices[i]);
        }
    }

    int activeCount() const {
        int count = 0;
        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            if (voices[i].isActive) count++;
        }
        return count;
    }

private:
    VideoVoice voices[MAX_VIDEO_VOICES];

    void startVoice(const VideoEvent& ev, int trimStart, int trimEnd) {
        // Si este bicho ya tenía una voz (no debería, pero por robustez
        // ante el orden de llegada de eventos), la reusamos en vez de
        // duplicar.
        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            if (voices[i].isActive && voices[i].bichoIndex == ev.bichoIndex) {
                // Retrigger de la misma voz: alterna la paridad de espejo,
                // así el efecto FLIP voltea el clip en cada nota.
                voices[i].flipParity = !voices[i].flipParity;
                resetVoice(voices[i], ev, trimStart, trimEnd);
                return;
            }
        }

        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            if (!voices[i].isActive) {
                voices[i].isActive = true;
                voices[i].flipParity = false;
                resetVoice(voices[i], ev, trimStart, trimEnd);
                return;
            }
        }
        // Cupo lleno: este bicho sonará sin representación visual hasta
        // que se libere un slot. No es un error — ver el comentario de
        // MAX_VIDEO_VOICES arriba.
    }

    void resetVoice(VideoVoice& v, const VideoEvent& ev, int trimStart, int trimEnd) {
        v.bichoIndex = ev.bichoIndex;
        v.sampleId = ev.sampleId;
        v.frameRate = ev.playbackRate < 0.0f ? -ev.playbackRate : ev.playbackRate;
        v.reverse = ev.reverse;
        v.ageSeconds = 0.0f;
        // Start at the trim range's edge (reverse -> last frame of the range).
        v.frameCursor = ev.reverse ? (float)(trimEnd > trimStart ? trimEnd - 1 : trimStart)
                                   : (float)trimStart;
    }

    void stopVoiceForBicho(int bichoIndex) {
        for (int i = 0; i < MAX_VIDEO_VOICES; i++) {
            if (voices[i].isActive && voices[i].bichoIndex == bichoIndex) {
                voices[i].isActive = false;
                return;
            }
        }
    }
};
