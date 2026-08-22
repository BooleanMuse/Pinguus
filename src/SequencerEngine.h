#pragma once
// SequencerEngine.h
//
// Todo lo que antes vivía en UpdateSimulationStep() ahora vive aquí, y se
// llama EXCLUSIVAMENTE desde dentro del callback de audio (AudioCallback),
// nunca desde el hilo principal. El grid y el pool de bichos son ahora
// propiedad exclusiva de esta clase.
//
// La diferencia clave respecto a la versión anterior: el "paso" del
// secuenciador ya no se mide en segundos de reloj de pared (deltaTime de
// Raylib), sino en NÚMERO DE MUESTRAS DE AUDIO procesadas. A una tasa de
// muestreo fija (ej. 44100 Hz), un BPM determinado corresponde a un número
// exacto de muestras por paso — sin redondeos acumulados, sin depender del
// framerate de la ventana, sin jitter del scheduler de vídeo.
//
// NUEVO EN ESTA VERSIÓN (integración SimTunes + YTPMV):
// soundBank[MAX_INSTRUMENTS] (un arreglo plano de SharedSample) se
// reemplaza por un InstrumentBank, donde cada slot puede tener audio,
// video, o ambos — ver InstrumentBank.h para el porqué de pre-decodificar
// todo a RAM de antemano. El motor de audio sigue sin tocar video para
// nada: solo reporta, vía VideoEventQueue, "qué bicho con qué instrumento
// empezó/dejó de sonar y a qué velocidad" — el hilo principal decide qué
// hacer con esa información (animar el panel derecho).

#include "CommandQueue.h"
#include "VisualSnapshot.h"
#include "InstrumentBank.h"
#include "VideoEventQueue.h"
#include "PdEffects.h"
#include <cmath>
#include <cstring>

#define MAX_BICHOS_POOL 1024
#define TRACKER_CHANNELS 4
#define TRACKER_ROWS 32

// --- Modo lineal (horizontal, estilo Mario Paint Composer) ---
// Un lienzo horizontal: COLUMNAS de tiempo (16avos, igual que el tracker) x
// FILAS/carriles. Cada fila es como un notey propio: tiene UNA voz que se
// re-dispara al tocar una nota nueva en esa fila (ver LinearVoice /
// fireLinearColumn). El tono de cada celda se guarda en la celda (viene del
// color elegido, no de la fila).
#define LINEAR_COLS 64
#define LINEAR_ROWS 25
#define LINEAR_CENTER_ROW 12
// Espacio de ids ESTABLE para los eventos de video de las voces lineales.
// Muy por encima de MAX_BICHOS_POOL para no colisionar con los ids de bichos
// que también viajan por VideoEventQueue (ver publishVideoEvents).
#define LINEAR_ID_BASE 100000

// Voces "en vivo": disparadas por un teclado/controlador MIDI (o el gamepad)
// para tocar el slot activo al tono de la tecla, como un instrumento. Pool
// pequeño round-robin; su id de video vive en otro rango estable.
#define LIVE_VOICES 8
#define LIVE_ID_BASE 200000
#define CTRL_ID (LIVE_ID_BASE + LIVE_VOICES) // id de video estable del Notey Controlable

// Modelos 3D (.glb/.vrm): son "slots virtuales" con id >= MODEL_SLOT_BASE. El
// motor los trata como SILENCIOSOS (sin audio) pero EMITE evento de video para
// que el hilo principal renderice el modelo y dispare su animación. El id
// codifica el modelo y la animación: MODEL_SLOT_BASE + modelo*MAX_MODEL_ANIMS + anim.
#define MODEL_SLOT_BASE 1000
#define MAX_MODELS 5
#define MAX_MODEL_ANIMS 32
static inline bool isModelSlot(int id) { return id >= MODEL_SLOT_BASE; }

// Fila -> semitono absoluto. Solo para compatibilidad al cargar el formato
// viejo de `lin` (sin pitch); la UI actual guarda el tono en la celda.
static inline int linearRowToSemitone(int row) { return LINEAR_CENTER_ROW - row; }

// A cell's colorId encodes an absolute semitone: semitone = colorId - PITCH_BASE.
// colorId 0 still means "no note". PITCH_BASE is high so many octaves (both
// up and down) fit in the unsigned-char colorId range 1..255.
#define PITCH_BASE 64

enum class ModifierType : int {
    None = 0,
    Teleport,
    DirectionChange,
    Sustain,
    Reverse,
    Bitcrush,
    MidiOut,
    Silence,  // corta en seco la nota (y el video) del bicho que la pisa
    EchoFx,   // la nota disparada aquí pasa por el bus de eco (delay)
    ReverbFx, // la nota disparada aquí pasa por el bus de reverb
    ChorusFx  // la nota disparada aquí pasa por el bus de chorus
};

// Fase 3: una celda COMPUESTA encadena varias primitivas. Cada acción es una
// de estas, aplicadas EN ORDEN cuando un bicho pisa la celda (nativo, por-paso).
enum CellActionType : unsigned char {
    CA_NONE = 0, CA_TURN, CA_HOLD, CA_MUTE, CA_TELEPORT, CA_FX, CA_NOTE
};
struct CellAction {
    unsigned char type = CA_NONE;
    int a = 0, b = 0;   // TURN: dx,dy | TELEPORT: tx,ty(abs) | FX: code(1..4) | NOTE: colorId,slot
    float f = 0.0f;     // HOLD: segundos
};
#define MAX_CELL_ACTIONS 4

struct GridCell {
    unsigned char colorId = 0;
    ModifierType modifier = ModifierType::None;
    int targetX = 0, targetY = 0;
    int nextDx = 0, nextDy = 0;
    unsigned char midiNote = 0;
    float sustainSeconds = 1.5f; // cuánto se detiene un bicho en una celda Sustain
    int sampleId = 0; // qué slot de InstrumentBank usa un bicho al pasar por aquí

    // Atributos SUPERPUESTOS. Son independientes de `modifier`, así que una
    // misma celda puede llevar a la vez un FX, una espera, un volumen y una
    // velocidad. Los tres son neutros por defecto.
    //
    // holdSeconds > 0 detiene al bicho AUNQUE la celda no tenga nota
    // (colorId == 0): eso es justamente un SILENCIO de la duración elegida.
    float holdSeconds = 0.0f;
    float volMul = 1.0f;  // ganancia de la nota disparada aquí (0..1)
    float timeMul = 1.0f; // velocidad de la nota: <1 cámara lenta, >1 rápida
    // Fase 3: si actionCount>0, la celda es COMPUESTA y se aplican estas acciones.
    CellAction actions[MAX_CELL_ACTIONS];
    int actionCount = 0;
};

struct Bicho {
    bool isActive = false;
    int x = 0, y = 0;
    int dx = 1, dy = 0;
    int sampleId = 0;

    // Tempo PROPIO: multiplicador sobre el BPM global (x0.25 = cuatro veces
    // más lento, x4 = cuatro veces más rápido). Cada bicho lleva su propio
    // acumulador de muestras, así bichos distintos avanzan a ritmos
    // distintos dentro del mismo grid.
    float tempoMul = 1.0f;
    unsigned long long stepAccum = 0;
    float volume = 1.0f; // ganancia propia del notey (0..1.5)

    // Ganancia de la CELDA que disparó la nota actual. Se queda pegada a la
    // nota hasta el próximo disparo, igual que hasEcho/hasReverb: si no, una
    // celda de volumen sólo se oiría durante el paso en que se pisa.
    float cellVol = 1.0f;

    bool isPlaying = false;
    float playbackCursor = 0.0f;
    float playbackRate = 1.0f;

    bool isSustained = false;
    long long sustainRemainingSamples = 0;
    bool hasBitcrush = false;
    bool muted = false; // se mueve normal, pero no dispara clips/samples
    bool stopped = false; // PARADO: no avanza ni suena (play/stop individual)
    bool hasEcho = false;    // la nota actual va al bus de eco
    bool hasReverb = false;  // la nota actual va al bus de reverb
    bool hasChorus = false;  // la nota actual va al bus de chorus

    // Refleja el isPlaying del callback ANTERIOR, exclusivamente para que
    // publishVideoEvents() pueda detectar la transición false->true o
    // true->false sin necesitar su propio estado paralelo. No lo toca
    // nadie más que publishVideoEvents().
    bool wasPlayingLastCallback = false;

    // Contador de disparos de nota: stepSequencer lo incrementa CADA vez que
    // el bicho pisa una celda pintada (aunque ya estuviera sonando). Así
    // publishVideoEvents puede reemitir VoiceStarted por cada nota nueva y
    // el video se REINICIA con el pitch de la celda — el efecto "picado"
    // clásico de YTPMV. Sin esto, un bicho que encadena celdas pintadas
    // dejaba el video corriendo con el rate viejo.
    unsigned int noteSeq = 0;
    unsigned int lastPublishedNoteSeq = 0; // solo lo toca publishVideoEvents
};

// Una celda del tracker: qué sample dispara, con qué tono y qué efecto.
struct TrackerCell {
    int sampleId = -1;         // -1 = celda vacía
    unsigned char colorId = 0; // mismo mapeo de tono que el grid (colorId-6 semitonos)
    unsigned char fx = 0;      // 0=nada, 1=reverb, 2=echo, 3=reverse
};

// Voz de un canal del tracker: como un Bicho pero estacionaria — solo
// reproduce el sample disparado en su canal.
struct TrackerVoice {
    bool isPlaying = false;
    int sampleId = 0;
    float cursor = 0.0f;
    float rate = 1.0f;
    bool hasEcho = false;
    bool hasReverb = false;
    bool hasChorus = false;
};

// Una celda del piano-roll lineal: idéntica a TrackerCell (qué sample, con qué
// tono, qué efecto). El tono se guarda como colorId (semitono + PITCH_BASE),
// derivado de la fila al colocar la nota.
struct LinearCell {
    int sampleId = -1;         // -1 = celda vacía
    unsigned char colorId = 0; // mismo mapeo de tono que el grid/tracker
    unsigned char fx = 0;      // 0=nada, 1=reverb, 2=echo, 3=reverse, 4=chorus
};

// Voz del modo lineal: como TrackerVoice, pero además lleva la info necesaria
// para emitir eventos de VIDEO (igual que un Bicho) — así una nota lineal con
// un CLIP de video también anima el collage, pitcheado/acelerado por su tono.
struct LinearVoice {
    bool isPlaying = false;
    int sampleId = 0;
    float cursor = 0.0f;
    float rate = 1.0f;         // magnitud+signo aplicados al cursor de audio
    bool hasEcho = false;
    bool hasReverb = false;
    bool hasChorus = false;
    float playbackRate = 1.0f; // igual que rate, expuesto al evento de video
    unsigned int noteSeq = 0;            // ++ en cada (re)disparo -> re-emite video
    unsigned int lastPublishedNoteSeq = 0;
    bool wasPlayingLastCallback = false; // solo lo toca publishVideoEvents
};

// Voz "en vivo" (teclado MIDI / gamepad): igual que una LinearVoice pero de un
// pool round-robin, no ligada a filas. Lleva la info de video para animar el
// collage con el clip tocado.
struct LiveVoice {
    bool isPlaying = false;
    int sampleId = 0;
    float cursor = 0.0f;
    float rate = 1.0f;
    bool hasEcho = false, hasReverb = false, hasChorus = false;
    float playbackRate = 1.0f;
    unsigned int noteSeq = 0, lastPublishedNoteSeq = 0;
    bool wasPlayingLastCallback = false;
};

// ===========================================================================
// PADS (modo BEATBOX / sampleadora, estilo Roland SP-555)
//
// 4 bancos x 16 pads = 64 pads. Cada pad apunta a un slot del InstrumentBank
// (el MISMO banco que usan el lienzo, el tracker y el modo lineal: un pad no
// copia audio, solo referencia un slot), y lleva su propio tono, volumen,
// modo de disparo y grupo de corte.
//
// UNA VOZ POR PAD (indexada por pad GLOBAL, no round-robin). Esto es lo que
// hace una sampleadora de pads y no un teclado: volver a golpear el mismo pad
// RE-DISPARA su voz desde el principio en vez de apilar copias, y como el id
// de video es estable (PAD_ID_BASE + pad) el clip del collage se reinicia en
// vez de multiplicarse. Cambiar de banco no corta lo que está sonando: las
// voces viven en el espacio global de 64 pads.
// ===========================================================================
#define PAD_COLS 4
#define PAD_ROWS 4
#define PAD_PER_BANK (PAD_COLS * PAD_ROWS)
#define PAD_BANKS 4
#define PAD_TOTAL (PAD_PER_BANK * PAD_BANKS)   // 64: cabe en una máscara de 64 bits
#define PAD_ID_BASE 300000

// Modo de disparo de un pad.
enum PadMode : unsigned char {
    PAD_ONESHOT = 0,  // golpea y suena entero, sueltes cuando sueltes
    PAD_GATE,         // suena mientras se mantiene pulsado; al soltar, corta
    PAD_LOOP          // primer golpe arranca en bucle, el segundo lo para
};

struct PadConfig {
    int slot = -1;                 // slot del InstrumentBank; -1 = pad vacío
    signed char pitch = 0;         // semitonos respecto al original
    float vol = 1.0f;              // 0..1
    unsigned char mode = PAD_ONESHOT;
    unsigned char choke = 0;       // 0 = ninguno; 1..4 = grupo de corte (charles)
    unsigned char fx = 0;          // 0 none, 1 reverb, 2 echo, 3 reverse, 4 chorus
    bool empty() const { return slot < 0; }
};

struct PadVoice {
    bool isPlaying = false;
    int sampleId = 0;
    float cursor = 0.0f;
    float rate = 1.0f;
    float vol = 1.0f;              // volumen del pad x velocidad del golpe
    bool loop = false;             // se reengancha al llegar al final
    bool held = false;             // GATE: el dedo/tecla sigue abajo
    bool hasEcho = false, hasReverb = false, hasChorus = false;
    float playbackRate = 1.0f;
    unsigned int noteSeq = 0, lastPublishedNoteSeq = 0;
    bool wasPlayingLastCallback = false;
};

// --- Secuenciador de patrones de pads ---
//
// El patrón se guarda como una MÁSCARA DE BITS por tick: el bit N dice "en
// este tick suena el pad global N". 64 pads = un unsigned long long exacto,
// así que grabar un golpe es un OR y borrarlo un AND — sin listas, sin
// asignaciones, seguro dentro del callback de audio.
//
// La rejilla interna corre a PAD_PAT_RES ticks por semicorchea. Grabar cae en
// el tick más cercano de esa rejilla fina (prácticamente sin cuantizar), y la
// cuantización opcional redondea a la semicorchea o a la corchea al vuelo.
#define PAD_PAT_RES 4                                   // ticks por semicorchea
#define PAD_PAT_MAX_STEPS 64                            // 4 compases de 4/4
#define PAD_PAT_TICKS (PAD_PAT_MAX_STEPS * PAD_PAT_RES) // 256 ticks

// La foto que lee la interfaz lleva el patrón entero; si alguien cambia el
// largo aquí y no allí, el memcpy de publishSnapshot se saldría del buffer.
static_assert(PAD_PAT_TICKS == SNAP_PAD_TICKS,
              "PAD_PAT_TICKS y SNAP_PAD_TICKS (VisualSnapshot.h) deben coincidir");
static_assert(PAD_TOTAL <= 64, "la mascara del patron es de 64 bits: PAD_TOTAL no puede pasar de 64");

using MidiNoteCallback = void(*)(unsigned char note, unsigned char velocity, void* userData);

// ---------------------------------------------------------------------------
// Efecto MAESTRO en tiempo real (la sección de FX de la SP-555)
//
// Va al FINAL de la mezcla, después de todo lo demás, y se maneja con DOS
// parámetros (x, y) en 0..1 — que es exactamente lo que se puede tocar de una
// vez con un dedo en una pantalla táctil, con el stick derecho de un mando o
// con dos potenciómetros de un controlador MIDI.
// ---------------------------------------------------------------------------
enum MasterFxType : int {
    MFX_OFF = 0, MFX_FILTER, MFX_DELAY, MFX_CRUSH, MFX_REVERB, MFX_FLANGER, MFX_SLICER,
    MFX_COUNT
};
static const char* kMasterFxNames[MFX_COUNT] = {
    "OFF", "FILTER", "DELAY", "CRUSH", "REVERB", "FLANGER", "SLICER"
};
// Qué significan los dos ejes en cada efecto, para poder etiquetar el pad XY.
static const char* kMasterFxAxisX[MFX_COUNT] = {
    "-", "CUTOFF", "TIME", "BITS", "SIZE", "RATE", "RATE"
};
static const char* kMasterFxAxisY[MFX_COUNT] = {
    "-", "RESO", "FEEDBACK", "RATE", "MIX", "DEPTH", "DEPTH"
};

class SequencerEngine {
public:
    SequencerEngine() {
        resetGrid();
    }

    void setGridSize(int w, int h) {
        width = (w > MAX_GRID_WIDTH) ? MAX_GRID_WIDTH : w;
        height = (h > MAX_GRID_HEIGHT) ? MAX_GRID_HEIGHT : h;
    }

    int getWidth() const { return width; }
    int getHeight() const { return height; }

    void setSampleRate(int rate) {
        sampleRate = rate;
        recomputeStepDuration();
    }

    void setMidiCallback(MidiNoteCallback cb, void* userData) {
        midiCallback = cb;
        midiUserData = userData;
    }

    InstrumentBank& getInstrumentBank() { return instrumentBank; }
    PdEffects& getPd() { return pd; }

    template <size_t Capacity>
    void drainCommands(CommandQueue<Capacity>& queue) {
        Command cmd;
        while (queue.pop(cmd)) {
            applyCommand(cmd);
        }
    }

    void advance(unsigned int frameCount) {
        // El patrón de pads tiene su PROPIO transporte: una sampleadora se
        // toca con el bucle de ritmo corriendo aunque el lienzo esté parado
        // (y al revés). Por eso va antes del `return` de la pausa global.
        if (padPatPlaying) {
            unsigned long long tickLen = samplesPerStep / PAD_PAT_RES;
            if (tickLen < 1) tickLen = 1;
            padPatAccum += frameCount;
            while (padPatAccum >= tickLen) {
                padPatAccum -= tickLen;
                padPatternTick();
            }
        }

        if (paused) return; // en pausa el secuenciador no avanza ni un paso

        // Tracker: avanza una fila por paso global (16avos al BPM actual).
        trackerAccum += frameCount;
        while (trackerAccum >= samplesPerStep) {
            trackerAccum -= samplesPerStep;
            trackerStep();
        }

        // Modo lineal: el playhead avanza una columna por paso global, igual
        // que el tracker (mismo largo de paso = 16avos al BPM actual).
        linearAccum += frameCount;
        while (linearAccum >= samplesPerStep) {
            linearAccum -= samplesPerStep;
            linearStep();
        }

        // Cada bicho avanza con su PROPIO acumulador y su propio largo de
        // paso (BPM global / tempoMul del bicho): tempos independientes.
        for (int i = 0; i < MAX_BICHOS_POOL; i++) {
            Bicho& b = bichos[i];
            if (!b.isActive) continue;
            if (b.stopped) continue; // parado: congelado en su sitio, sin sonar

            float mul = (b.tempoMul > 0.01f) ? b.tempoMul : 1.0f;
            unsigned long long stepLen = (unsigned long long)((double)samplesPerStep / mul);
            if (stepLen < 1) stepLen = 1;

            b.stepAccum += frameCount;
            while (b.stepAccum >= stepLen) {
                b.stepAccum -= stepLen;
                stepBicho(b, stepLen);
            }
        }
    }

    void renderAudio(float* pOutput, unsigned int frameCount, unsigned int channels) {
        memset(pOutput, 0, frameCount * channels * sizeof(float));

        // Buses de efectos: los bichos, las notas y los pads marcados suman su
        // señal aquí además de a la salida seca; al final del callback se
        // procesa el eco (delay con feedback), la reverb (4 combs paralelos) y
        // el chorus. Todo son buffers miembros de tamaño fijo — nada de malloc
        // en el callback.
        //
        // Se vacían AQUÍ ARRIBA, antes de la pausa, porque los pads escriben en
        // ellos aunque el secuenciador esté parado: una sampleadora se toca con
        // el resto en silencio.
        unsigned int busFrames = frameCount < MAX_RENDER_FRAMES ? frameCount : MAX_RENDER_FRAMES;
        memset(busEcho, 0, busFrames * sizeof(float));
        memset(busReverb, 0, busFrames * sizeof(float));
        memset(busChorus, 0, busFrames * sizeof(float));

        // Reproducción de PREVIEW (editor de trim): suena SIEMPRE, incluso con
        // el secuenciador en pausa, para poder escuchar el clip/sample que se
        // está recortando. Reproduce a tono original en bucle dentro de [a0,a1).
        if (preview.active) {
            const SharedSample& s = instrumentBank.at(preview.slot).audio;
            if (s.pPCM != nullptr && preview.a1 > preview.a0 && preview.a1 <= s.totalFrames) {
                for (unsigned int frame = 0; frame < frameCount; frame++) {
                    if (preview.cursor >= preview.a1 || preview.cursor < preview.a0) preview.cursor = preview.a0;
                    float v = s.pPCM[preview.cursor] * 0.6f;
                    for (unsigned int ch = 0; ch < channels; ch++) pOutput[frame * channels + ch] += v;
                    preview.cursor++;
                }
            } else {
                preview.active = false;
            }
        }

        // Voces EN VIVO (teclado MIDI / gamepad): suenan SIEMPRE, incluso en
        // pausa, para poder tocar como instrumento con el secuenciador parado.
        // Se mezclan en seco (sin buses de FX); el "reverse" sí funciona porque
        // vive en el signo del cursor, no en un bus.
        for (int t = 0; t <= LIVE_VOICES; t++) {
            LiveVoice& v = (t < LIVE_VOICES) ? liveVoices[t] : ctrlVoice; // +1 = voz del Notey Controlable
            if (!v.isPlaying) continue;
            const InstrumentSource& srcLv = instrumentBank.at(v.sampleId);
            const SharedSample& sample = srcLv.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) { v.isPlaying = false; continue; }
            float lo = (float)srcLv.audioTrimStart(), hi = (float)srcLv.audioTrimEnd();
            for (unsigned int frame = 0; frame < frameCount; frame++) {
                if (v.cursor >= hi || v.cursor < lo) { v.isPlaying = false; break; }
                int il = (int)v.cursor, ir = il + 1;
                if ((unsigned long long)ir >= sample.totalFrames) ir = il;
                float frac = v.cursor - il;
                float contrib = (sample.pPCM[il] + frac * (sample.pPCM[ir] - sample.pPCM[il])) * 0.4f;
                for (unsigned int ch = 0; ch < channels; ch++) pOutput[frame * channels + ch] += contrib;
                v.cursor += v.rate;
            }
        }

        // Voces de PAD (modo BEATBOX): como las voces en vivo, suenan siempre,
        // en pausa o no. Lo que añaden es el volumen por pad (x velocidad del
        // golpe), el bucle, y los envíos a los buses de efectos.
        for (int p = 0; p < PAD_TOTAL; p++) {
            PadVoice& v = padVoices[p];
            if (!v.isPlaying) continue;
            const InstrumentSource& srcPad = instrumentBank.at(v.sampleId);
            const SharedSample& sample = srcPad.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) { v.isPlaying = false; continue; }
            float lo = (float)srcPad.audioTrimStart(), hi = (float)srcPad.audioTrimEnd();
            if (hi <= lo) { v.isPlaying = false; continue; }

            for (unsigned int frame = 0; frame < frameCount; frame++) {
                if (v.cursor >= hi || v.cursor < lo) {
                    // Un pad en bucle se reengancha por el otro extremo (y por
                    // el de arriba si va del revés) en vez de callarse.
                    if (v.loop) v.cursor = (v.rate < 0.0f) ? hi - 1.0f : lo;
                    else { v.isPlaying = false; break; }
                }
                int il = (int)v.cursor, ir = il + 1;
                if ((unsigned long long)ir >= sample.totalFrames) ir = il;
                float frac = v.cursor - il;
                float contrib = (sample.pPCM[il] + frac * (sample.pPCM[ir] - sample.pPCM[il])) * 0.4f * v.vol;
                for (unsigned int ch = 0; ch < channels; ch++) pOutput[frame * channels + ch] += contrib;
                if (frame < busFrames) {
                    if (v.hasEcho) busEcho[frame] += contrib;
                    if (v.hasReverb) busReverb[frame] += contrib;
                    if (v.hasChorus) busChorus[frame] += contrib;
                }
                v.cursor += v.rate;
            }
        }

        // En pausa: el secuenciador calla (salvo el preview, las voces en vivo
        // y los pads de arriba) y los cursores de reproducción quedan
        // congelados donde estaban. Los buses y el FX maestro sí siguen
        // procesándose al final: si no, la cola del eco se cortaría en seco al
        // pausar, y el filtro de la sección de FX dejaría de afectar a los pads.
        if (!paused) {

        for (int i = 0; i < MAX_BICHOS_POOL; i++) {
            Bicho& b = bichos[i];
            if (!b.isActive || !b.isPlaying) continue;
            if (pd.hasPatch(b.sampleId)) continue; // routed through Pd below

            const InstrumentSource& srcB = instrumentBank.at(b.sampleId);
            const SharedSample& sample = srcB.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) continue;
            float aLo = (float)srcB.audioTrimStart(), aHi = (float)srcB.audioTrimEnd();

            for (unsigned int frame = 0; frame < frameCount; frame++) {
                if (b.playbackCursor >= aHi || b.playbackCursor < aLo) {
                    b.isPlaying = false;
                    break;
                }

                int idxLeft = (int)b.playbackCursor;
                int idxRight = idxLeft + 1;
                if ((unsigned long long)idxRight >= sample.totalFrames) idxRight = idxLeft;

                float frac = b.playbackCursor - idxLeft;
                float s0 = sample.pPCM[idxLeft];
                float s1 = sample.pPCM[idxRight];
                float finalSample = s0 + frac * (s1 - s0);

                if (b.hasBitcrush) {
                    const float quantizationSteps = 16.0f;
                    finalSample = floorf(finalSample * quantizationSteps) / quantizationSteps;
                }

                float contrib = finalSample * 0.4f * b.volume * b.cellVol;
                for (unsigned int ch = 0; ch < channels; ch++) {
                    pOutput[frame * channels + ch] += contrib;
                }
                if (frame < busFrames) {
                    if (b.hasEcho) busEcho[frame] += contrib;
                    if (b.hasReverb) busReverb[frame] += contrib;
                    if (b.hasChorus) busChorus[frame] += contrib;
                }

                b.playbackCursor += b.playbackRate;
            }
        }

        // Voces del tracker: mismo mezclado que los bichos (con sus buses).
        for (int t = 0; t < TRACKER_CHANNELS; t++) {
            TrackerVoice& v = trackVoices[t];
            if (!v.isPlaying) continue;
            if (pd.hasPatch(v.sampleId)) continue; // routed through Pd below

            const InstrumentSource& srcTvR = instrumentBank.at(v.sampleId);
            const SharedSample& sample = srcTvR.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) { v.isPlaying = false; continue; }
            float tvLo = (float)srcTvR.audioTrimStart(), tvHi = (float)srcTvR.audioTrimEnd();

            for (unsigned int frame = 0; frame < frameCount; frame++) {
                if (v.cursor >= tvHi || v.cursor < tvLo) {
                    v.isPlaying = false;
                    break;
                }

                int idxLeft = (int)v.cursor;
                int idxRight = idxLeft + 1;
                if ((unsigned long long)idxRight >= sample.totalFrames) idxRight = idxLeft;

                float frac = v.cursor - idxLeft;
                float s0 = sample.pPCM[idxLeft];
                float s1 = sample.pPCM[idxRight];
                float contrib = (s0 + frac * (s1 - s0)) * 0.4f;

                for (unsigned int ch = 0; ch < channels; ch++) {
                    pOutput[frame * channels + ch] += contrib;
                }
                if (frame < busFrames) {
                    if (v.hasEcho) busEcho[frame] += contrib;
                    if (v.hasReverb) busReverb[frame] += contrib;
                    if (v.hasChorus) busChorus[frame] += contrib;
                }

                v.cursor += v.rate;
            }
        }

        // Voces del modo lineal: mismo mezclado que los bichos y el tracker.
        for (int t = 0; t < LINEAR_ROWS; t++) {
            LinearVoice& v = linearVoices[t];
            if (!v.isPlaying) continue;
            if (pd.hasPatch(v.sampleId)) continue; // routed through Pd below

            const InstrumentSource& srcLvR = instrumentBank.at(v.sampleId);
            const SharedSample& sample = srcLvR.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) { v.isPlaying = false; continue; }
            float lvLo = (float)srcLvR.audioTrimStart(), lvHi = (float)srcLvR.audioTrimEnd();

            for (unsigned int frame = 0; frame < frameCount; frame++) {
                if (v.cursor >= lvHi || v.cursor < lvLo) {
                    v.isPlaying = false;
                    break;
                }

                int idxLeft = (int)v.cursor;
                int idxRight = idxLeft + 1;
                if ((unsigned long long)idxRight >= sample.totalFrames) idxRight = idxLeft;

                float frac = v.cursor - idxLeft;
                float s0 = sample.pPCM[idxLeft];
                float s1 = sample.pPCM[idxRight];
                float contrib = (s0 + frac * (s1 - s0)) * 0.4f;

                for (unsigned int ch = 0; ch < channels; ch++) {
                    pOutput[frame * channels + ch] += contrib;
                }
                if (frame < busFrames) {
                    if (v.hasEcho) busEcho[frame] += contrib;
                    if (v.hasReverb) busReverb[frame] += contrib;
                    if (v.hasChorus) busChorus[frame] += contrib;
                }

                v.cursor += v.rate;
            }
        }

        } // fin de if (!paused): a partir de aquí se procesa siempre

        // Procesado de los buses (una sola vez por callback, mono -> ambos canales).
        for (unsigned int frame = 0; frame < busFrames; frame++) {
            // Eco: línea de retardo de 0.25s con feedback.
            float e = echoBuf[echoPos];
            echoBuf[echoPos] = busEcho[frame] + e * 0.45f;
            echoPos++;
            if (echoPos >= kEchoLen) echoPos = 0;
            float wet = e * 0.85f;

            // Reverb: 4 filtros comb paralelos con longitudes coprimas.
            float rIn = busReverb[frame];
            float rWet = 0.0f;
            for (int c = 0; c < 4; c++) {
                float v = combBuf[c][combPos[c]];
                combBuf[c][combPos[c]] = rIn + v * 0.75f;
                combPos[c]++;
                if (combPos[c] >= kCombLen[c]) combPos[c] = 0;
                rWet += v;
            }
            wet += rWet * 0.3f;

            // Chorus: delay corto modulado por un LFO (voz "engrosada").
            chorusBuf[chorusPos] = busChorus[frame];
            chorusLfo += kChorusLfoInc;
            if (chorusLfo >= 6.2831853f) chorusLfo -= 6.2831853f;
            float delaySamp = kChorusBase + kChorusDepth * sinf(chorusLfo); // ~5..15ms
            float readPos = (float)chorusPos - delaySamp;
            while (readPos < 0.0f) readPos += kChorusLen;
            int r0 = (int)readPos;
            int r1 = (r0 + 1) % kChorusLen;
            float cf = readPos - r0;
            float cWet = chorusBuf[r0] + cf * (chorusBuf[r1] - chorusBuf[r0]);
            chorusPos++;
            if (chorusPos >= kChorusLen) chorusPos = 0;
            wet += (busChorus[frame] * 0.5f + cWet * 0.5f); // dry+wet blend

            for (unsigned int ch = 0; ch < channels; ch++) {
                pOutput[frame * channels + ch] += wet;
            }
        }

        // --- Pure Data insert effects (per patched slot) ---
        // Slots with a .pd patch were skipped above; mix each such slot's
        // notes into a mono scratch, run it through the patch, add to master.
        // Las voces de PAD no pasan por aquí a propósito: el modo BEATBOX tiene
        // su propia sección de FX (el maestro de abajo), y Android compila sin
        // libpd, así que un pad tiene que sonar igual en las dos plataformas.
        unsigned int pf = frameCount < MAX_RENDER_FRAMES ? frameCount : MAX_RENDER_FRAMES;
        if (paused) pf = 0;   // en pausa los cursores de esas voces no avanzan
        for (int slot = 0; slot < MAX_INSTRUMENTS && pf > 0; slot++) {
            if (!pd.hasPatch(slot)) continue;
            const InstrumentSource& srcP = instrumentBank.at(slot);
            const SharedSample& sample = srcP.audio;
            if (sample.pPCM == nullptr || sample.totalFrames == 0) continue;
            float pLo = (float)srcP.audioTrimStart(), pHi = (float)srcP.audioTrimEnd();

            memset(pdScratch, 0, pf * sizeof(float));
            bool any = false;

            for (int i = 0; i < MAX_BICHOS_POOL; i++) {
                Bicho& b = bichos[i];
                if (!b.isActive || !b.isPlaying || b.sampleId != slot) continue;
                for (unsigned int frame = 0; frame < pf; frame++) {
                    if (b.playbackCursor >= pHi || b.playbackCursor < pLo) { b.isPlaying = false; break; }
                    int il = (int)b.playbackCursor, ir = il + 1;
                    if ((unsigned long long)ir >= sample.totalFrames) ir = il;
                    float frac = b.playbackCursor - il;
                    float v = sample.pPCM[il] + frac * (sample.pPCM[ir] - sample.pPCM[il]);
                    if (b.hasBitcrush) { const float q = 16.0f; v = floorf(v * q) / q; }
                    pdScratch[frame] += v * 0.4f * b.volume * b.cellVol;
                    b.playbackCursor += b.playbackRate;
                }
                any = true;
            }
            for (int t = 0; t < TRACKER_CHANNELS; t++) {
                TrackerVoice& tv = trackVoices[t];
                if (!tv.isPlaying || tv.sampleId != slot) continue;
                for (unsigned int frame = 0; frame < pf; frame++) {
                    if (tv.cursor >= pHi || tv.cursor < pLo) { tv.isPlaying = false; break; }
                    int il = (int)tv.cursor, ir = il + 1;
                    if ((unsigned long long)ir >= sample.totalFrames) ir = il;
                    float frac = tv.cursor - il;
                    pdScratch[frame] += (sample.pPCM[il] + frac * (sample.pPCM[ir] - sample.pPCM[il])) * 0.4f;
                    tv.cursor += tv.rate;
                }
                any = true;
            }
            for (int t = 0; t < LINEAR_ROWS; t++) {
                LinearVoice& lv = linearVoices[t];
                if (!lv.isPlaying || lv.sampleId != slot) continue;
                for (unsigned int frame = 0; frame < pf; frame++) {
                    if (lv.cursor >= pHi || lv.cursor < pLo) { lv.isPlaying = false; break; }
                    int il = (int)lv.cursor, ir = il + 1;
                    if ((unsigned long long)ir >= sample.totalFrames) ir = il;
                    float frac = lv.cursor - il;
                    pdScratch[frame] += (sample.pPCM[il] + frac * (sample.pPCM[ir] - sample.pPCM[il])) * 0.4f;
                    lv.cursor += lv.rate;
                }
                any = true;
            }

            if (any) {
                pd.process(slot, pdScratch, pf);
                for (unsigned int frame = 0; frame < pf; frame++)
                    for (unsigned int ch = 0; ch < channels; ch++)
                        pOutput[frame * channels + ch] += pdScratch[frame];
            }
        }

        // --- FX MAESTRO (la sección de efectos del modo BEATBOX) ---
        // Va al final del todo, sobre la mezcla ya hecha. Todos los canales
        // llevan la misma señal (el motor mezcla en mono), así que se procesa
        // el canal 0 y el resultado se copia al resto.
        renderMasterFx(pOutput, frameCount, channels);
    }

    void publishSnapshot(VisualSnapshotPublisher& publisher) {
        SnapshotBuffer& buf = publisher.beginWrite();
        buf.count = 0;
        buf.trackerRow = trackerRow;
        buf.linearCol = linearCol;
        buf.previewActive = preview.active;
        buf.previewCursor = (long long)preview.cursor;

        // Modo BEATBOX: qué pads suenan, dónde va el patrón y el patrón entero.
        buf.padPlaying = 0ULL;
        buf.padLooping = 0ULL;
        for (int p = 0; p < PAD_TOTAL; p++) {
            if (!padVoices[p].isPlaying) continue;
            buf.padPlaying |= (1ULL << p);
            if (padVoices[p].loop) buf.padLooping |= (1ULL << p);
        }
        buf.padTick = padPatPlaying ? padPatTick : -1;
        buf.padSteps = padPatSteps;
        buf.padPatPlaying = padPatPlaying;
        buf.padRecording = padRecording;
        buf.padPatVersion = padPatVersion;
        memcpy(buf.padPattern, padPattern, sizeof(padPattern));
        for (int i = 0; i < MAX_BICHOS_POOL && buf.count < MAX_BICHOS_SNAPSHOT; i++) {
            if (!bichos[i].isActive) continue;
            BichoVisual& v = buf.bichos[buf.count++];
            v.isActive = true;
            v.x = bichos[i].x;
            v.y = bichos[i].y;
            v.isPlaying = bichos[i].isPlaying;
            v.poolIndex = i;
            v.sampleId = bichos[i].sampleId;
            v.muted = bichos[i].muted;
            v.stopped = bichos[i].stopped;
        }
        publisher.commitWrite();
    }

    // Llamado SOLO desde el hilo de audio, igual que publishSnapshot — se
    // recorre el pool UNA vez por callback (no por muestra) y se compara
    // isPlaying actual contra wasPlayingLastCallback para emitir como
    // máximo un VoiceStarted y un VoiceStopped por transición real, en vez
    // de un evento por cada una de las (potencialmente miles de) muestras
    // procesadas en renderAudio().
    template <size_t Capacity>
    void publishVideoEvents(VideoEventQueue<Capacity>& queue) {
        for (int i = 0; i < MAX_BICHOS_POOL; i++) {
            Bicho& b = bichos[i];

            // Un bicho desactivado a mitad de reproducción (RemoveBicho)
            // también cuenta como "dejó de sonar" para quien esté
            // mostrando su video en el panel derecho.
            bool effectivelyPlaying = b.isActive && b.isPlaying;

            bool emits = isModelSlot(b.sampleId) || instrumentBank.at(b.sampleId).hasVideo();
            if (effectivelyPlaying && (!b.wasPlayingLastCallback || b.noteSeq != b.lastPublishedNoteSeq)) {
                b.lastPublishedNoteSeq = b.noteSeq;
                if (emits) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStarted;
                    ev.bichoIndex = i;
                    ev.sampleId = b.sampleId;
                    ev.playbackRate = b.playbackRate;
                    ev.reverse = b.playbackRate < 0.0f;
                    queue.push(ev);
                }
            } else if (!effectivelyPlaying && b.wasPlayingLastCallback) {
                if (emits) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStopped;
                    ev.bichoIndex = i;
                    ev.sampleId = b.sampleId;
                    queue.push(ev);
                }
            }

            b.wasPlayingLastCallback = effectivelyPlaying;
        }

        // Voces del modo lineal: misma lógica de transición que los bichos,
        // pero con un id ESTABLE en el espacio LINEAR_ID_BASE+i para no chocar
        // con los ids de bichos. Así una nota lineal con video anima el collage.
        for (int i = 0; i < LINEAR_ROWS; i++) {
            LinearVoice& v = linearVoices[i];
            if (v.isPlaying && (!v.wasPlayingLastCallback || v.noteSeq != v.lastPublishedNoteSeq)) {
                v.lastPublishedNoteSeq = v.noteSeq;
                if (instrumentBank.at(v.sampleId).hasVideo()) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStarted;
                    ev.bichoIndex = LINEAR_ID_BASE + i;
                    ev.sampleId = v.sampleId;
                    ev.playbackRate = v.playbackRate;
                    ev.reverse = v.playbackRate < 0.0f;
                    queue.push(ev);
                }
            } else if (!v.isPlaying && v.wasPlayingLastCallback) {
                if (instrumentBank.at(v.sampleId).hasVideo()) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStopped;
                    ev.bichoIndex = LINEAR_ID_BASE + i;
                    ev.sampleId = v.sampleId;
                    queue.push(ev);
                }
            }
            v.wasPlayingLastCallback = v.isPlaying;
        }

        // Voces EN VIVO: misma lógica, id estable en el espacio LIVE_ID_BASE+i.
        // La voz extra (i == LIVE_VOICES) es el Notey Controlable, con id CTRL_ID.
        for (int i = 0; i <= LIVE_VOICES; i++) {
            LiveVoice& v = (i < LIVE_VOICES) ? liveVoices[i] : ctrlVoice;
            int vid = (i < LIVE_VOICES) ? (LIVE_ID_BASE + i) : CTRL_ID;
            if (v.isPlaying && (!v.wasPlayingLastCallback || v.noteSeq != v.lastPublishedNoteSeq)) {
                v.lastPublishedNoteSeq = v.noteSeq;
                if (instrumentBank.at(v.sampleId).hasVideo()) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStarted;
                    ev.bichoIndex = vid;
                    ev.sampleId = v.sampleId;
                    ev.playbackRate = v.playbackRate;
                    ev.reverse = v.playbackRate < 0.0f;
                    queue.push(ev);
                }
            } else if (!v.isPlaying && v.wasPlayingLastCallback) {
                if (instrumentBank.at(v.sampleId).hasVideo()) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStopped;
                    ev.bichoIndex = vid;
                    ev.sampleId = v.sampleId;
                    queue.push(ev);
                }
            }
            v.wasPlayingLastCallback = v.isPlaying;
        }

        // Voces de PAD: id estable PAD_ID_BASE + pad, así un pad con un CLIP de
        // vídeo anima el collage y al re-golpearlo su vídeo VUELVE A EMPEZAR en
        // vez de aparecer una copia nueva. Es lo que hace que grabar la salida
        // en el móvil dé un vídeo que va al ritmo de lo que se toca.
        for (int p = 0; p < PAD_TOTAL; p++) {
            PadVoice& v = padVoices[p];
            bool emits = instrumentBank.at(v.sampleId).hasVideo();
            if (v.isPlaying && (!v.wasPlayingLastCallback || v.noteSeq != v.lastPublishedNoteSeq)) {
                v.lastPublishedNoteSeq = v.noteSeq;
                if (emits) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStarted;
                    ev.bichoIndex = PAD_ID_BASE + p;
                    ev.sampleId = v.sampleId;
                    ev.playbackRate = v.playbackRate;
                    ev.reverse = v.playbackRate < 0.0f;
                    queue.push(ev);
                }
            } else if (!v.isPlaying && v.wasPlayingLastCallback) {
                if (emits) {
                    VideoEvent ev;
                    ev.type = VideoEventType::VoiceStopped;
                    ev.bichoIndex = PAD_ID_BASE + p;
                    ev.sampleId = v.sampleId;
                    queue.push(ev);
                }
            }
            v.wasPlayingLastCallback = v.isPlaying;
        }
    }

private:
    static const int MAX_GRID_WIDTH = 128;
    static const int MAX_GRID_HEIGHT = 128;

    GridCell cells[MAX_GRID_WIDTH * MAX_GRID_HEIGHT];
    int width = 40;
    int height = 30;

    Bicho bichos[MAX_BICHOS_POOL];
    InstrumentBank instrumentBank;
    PdEffects pd;                       // per-slot Pure Data insert effects

    // --- Tracker (4 canales x 32 filas, loopea) ---
    TrackerCell trackerCells[TRACKER_CHANNELS][TRACKER_ROWS];
    TrackerVoice trackVoices[TRACKER_CHANNELS];
    int trackerRow = -1;
    unsigned long long trackerAccum = 0;

    // --- Modo lineal (horizontal, estilo canvas) ---
    // UNA voz por FILA/carril: cada fila se comporta como un notey propio, así
    // una nota nueva en esa fila RE-DISPARA la misma voz (y su mismo video) en
    // vez de acumular voces/videos. Por eso hay LINEAR_ROWS voces, indexadas
    // por fila, y el id de video estable es LINEAR_ID_BASE + fila.
    LinearCell linearCells[LINEAR_COLS][LINEAR_ROWS];
    LinearVoice linearVoices[LINEAR_ROWS];
    int linearCol = -1;                 // columna actual (playhead); -1 = pre-inicio
    unsigned long long linearAccum = 0;
    int linearLength = 32;              // columnas del loop (1..LINEAR_COLS)
    bool linearLoop = true;             // true = loopea; false = una sola pasada

    // --- Voces en vivo (teclado MIDI / gamepad) ---
    LiveVoice liveVoices[LIVE_VOICES];
    int liveVoiceRR = 0;                // puntero round-robin para asignar voces
    // Voz DEDICADA del "Notey Controlable": una sola voz con id de video estable,
    // así al pasar por celdas RE-DISPARA (retrigger) su mismo video sin multiplicarlo.
    LiveVoice ctrlVoice;

    // --- Pads (modo BEATBOX) ---
    // Una voz por pad GLOBAL: golpear otra vez el mismo pad lo re-dispara en
    // vez de apilar una segunda copia (ver el comentario de PadVoice).
    PadConfig padCfg[PAD_TOTAL];
    PadVoice  padVoices[PAD_TOTAL];

    unsigned long long padPattern[PAD_PAT_TICKS] = {0};
    int  padPatSteps = 16;              // largo del bucle, en semicorcheas
    int  padPatTick = -1;               // tick actual; -1 = aún no ha empezado
    unsigned long long padPatAccum = 0;
    bool padPatPlaying = false;
    bool padRecording = false;
    int  padQuantize = 0;               // 0 off, 1 = 1/16, 2 = 1/8, 3 = 1/4, 4 = 1/32
    unsigned int padPatVersion = 0;     // ++ en cada cambio, para que la UI se entere

    // Escribe un golpe en el patrón, redondeando al tick que toque según la
    // cuantización elegida. Sin cuantizar cae en la rejilla fina (1/64), que a
    // efectos prácticos es "donde lo tocaste".
    void recordPadHit(int pad) {
        if (!padPatPlaying) return;
        int ticks = padPatSteps * PAD_PAT_RES;
        int t = padPatTick < 0 ? 0 : padPatTick;
        int grid = 1;
        switch (padQuantize) {
            case 1: grid = PAD_PAT_RES; break;         // semicorchea
            case 2: grid = PAD_PAT_RES * 2; break;     // corchea
            case 3: grid = PAD_PAT_RES * 4; break;     // negra
            case 4: grid = PAD_PAT_RES / 2; break;     // fusa
            default: grid = 1; break;
        }
        if (grid < 1) grid = 1;
        if (grid > 1) t = ((t + grid / 2) / grid) * grid;  // al más cercano, no al anterior
        if (t >= ticks) t -= ticks;                        // el redondeo puede dar la vuelta
        if (t < 0) t = 0;
        padPattern[t] |= (1ULL << pad);
        padPatVersion++;
    }

    // -----------------------------------------------------------------------
    // FX MAESTRO
    //
    // Siete efectos con DOS mandos cada uno, porque dos es lo que se puede
    // mover a la vez con un dedo en una pantalla, con el stick derecho de un
    // mando o con dos ruedas de un controlador MIDI. Todos son baratos: nada
    // aquí reserva memoria ni recorre tablas grandes, porque esto corre dentro
    // del callback de audio en cada muestra.
    // -----------------------------------------------------------------------
    void renderMasterFx(float* pOutput, unsigned int frameCount, unsigned int channels) {
        if (!mfxOn || mfxType == MFX_OFF) return;

        const float sr = (float)(sampleRate > 0 ? sampleRate : 44100);

        // Parámetros que no dependen de la muestra, calculados UNA vez.
        float fCut = 0.0f, fQ = 0.0f;                    // FILTER
        int   dlyLen = 1; float dlyFb = 0.0f;            // DELAY
        float crushStep = 1.0f; int crushHold = 1;       // CRUSH
        float revFb = 0.0f, revMix = 0.0f;               // REVERB
        float flLfoInc = 0.0f, flDepth = 0.0f;           // FLANGER
        unsigned long long slicePeriod = 1; float sliceDuty = 0.5f; // SLICER

        switch (mfxType) {
            case MFX_FILTER: {
                // Corte exponencial 80 Hz .. 12 kHz: en lineal, la mitad de
                // arriba del recorrido no se oiría como que hace nada.
                float hz = 80.0f * powf(150.0f, mfxX);
                if (hz > sr * 0.45f) hz = sr * 0.45f;
                fCut = 2.0f * sinf(3.14159265f * hz / sr);
                if (fCut > 0.99f) fCut = 0.99f;
                fQ = 1.0f - mfxY * 0.92f;                // menos amortiguación = más pico
                if (fQ < 0.06f) fQ = 0.06f;
                break;
            }
            case MFX_DELAY: {
                dlyLen = (int)(sr * (0.03f + mfxX * 0.57f));   // 30 ms .. 600 ms
                if (dlyLen < 1) dlyLen = 1;
                if (dlyLen >= kMfxDelayLen) dlyLen = kMfxDelayLen - 1;
                dlyFb = mfxY * 0.85f;
                break;
            }
            case MFX_CRUSH: {
                float bits = 1.0f + mfxX * 14.0f;             // 1 .. 15 bits
                crushStep = powf(2.0f, bits);
                crushHold = 1 + (int)(mfxY * 40.0f);          // diezmado
                break;
            }
            case MFX_REVERB: {
                revFb = 0.55f + mfxX * 0.38f;                 // "tamaño" de la sala
                revMix = mfxY;
                break;
            }
            case MFX_FLANGER: {
                float rate = 0.05f + mfxX * 4.0f;             // Hz
                flLfoInc = 6.2831853f * rate / sr;
                flDepth = mfxY;
                break;
            }
            case MFX_SLICER: {
                // Sincronizado al tempo: el corte cae en divisiones del paso
                // (semicorchea) del propio secuenciador, no en un Hz suelto,
                // que es lo que lo hace sonar "a ritmo" y no a zumbido.
                static const float kDiv[4] = {4.0f, 2.0f, 1.0f, 0.5f}; // negra..fusa
                int di = (int)(mfxX * 3.999f);
                if (di < 0) di = 0;
                if (di > 3) di = 3;
                unsigned long long per = (unsigned long long)((double)samplesPerStep * kDiv[di]);
                slicePeriod = per < 32 ? 32 : per;
                sliceDuty = 0.1f + (1.0f - mfxY) * 0.85f;     // arriba = más picado
                break;
            }
            default: break;
        }

        for (unsigned int frame = 0; frame < frameCount; frame++) {
            float in = pOutput[frame * channels];
            float out = in;

            switch (mfxType) {
                case MFX_FILTER: {
                    // Filtro de variables de estado (Chamberlin): un paso-bajo
                    // resonante en cuatro sumas, que es el filtro de barrido
                    // clásico de una caja de ritmos.
                    mfxLp1 += fCut * mfxBp;
                    float hp = in - mfxLp1 - fQ * mfxBp;
                    mfxBp += fCut * hp;
                    out = mfxLp1;
                    // Con resonancia alta el filtro se puede disparar; recortar
                    // aquí es más barato que dejar que llegue a infinito.
                    if (out > 4.0f) { out = 4.0f; mfxLp1 = 4.0f; }
                    if (out < -4.0f) { out = -4.0f; mfxLp1 = -4.0f; }
                    if (mfxBp > 4.0f) mfxBp = 4.0f;
                    if (mfxBp < -4.0f) mfxBp = -4.0f;
                    break;
                }
                case MFX_DELAY: {
                    int rd = mfxDelayPos - dlyLen;
                    while (rd < 0) rd += kMfxDelayLen;
                    float d = mfxDelayBuf[rd];
                    mfxDelayBuf[mfxDelayPos] = in + d * dlyFb;
                    mfxDelayPos++;
                    if (mfxDelayPos >= kMfxDelayLen) mfxDelayPos = 0;
                    out = in + d * 0.7f;
                    break;
                }
                case MFX_CRUSH: {
                    if (mfxHoldCnt <= 0) {
                        mfxHold = floorf(in * crushStep) / crushStep;
                        mfxHoldCnt = crushHold;
                    }
                    mfxHoldCnt--;
                    out = mfxHold;
                    break;
                }
                case MFX_REVERB: {
                    float wet = 0.0f;
                    for (int c = 0; c < 4; c++) {
                        float v = mfxCombBuf[c][mfxCombPos[c]];
                        mfxCombBuf[c][mfxCombPos[c]] = in + v * revFb;
                        mfxCombPos[c]++;
                        if (mfxCombPos[c] >= kMfxCombLen[c]) mfxCombPos[c] = 0;
                        wet += v;
                    }
                    out = in * (1.0f - revMix * 0.5f) + wet * 0.22f * revMix;
                    break;
                }
                case MFX_FLANGER: {
                    mfxFlangeBuf[mfxFlangePos] = in;
                    mfxPhase += flLfoInc;
                    if (mfxPhase >= 6.2831853f) mfxPhase -= 6.2831853f;
                    // 0,5 ms .. 9 ms de retardo, barridos por el LFO.
                    float dly = 22.0f + (0.5f + 0.5f * sinf(mfxPhase)) * flDepth * 380.0f;
                    float rp = (float)mfxFlangePos - dly;
                    while (rp < 0.0f) rp += kMfxFlangeLen;
                    int r0 = (int)rp, r1 = (r0 + 1) % kMfxFlangeLen;
                    float cf = rp - r0;
                    float d = mfxFlangeBuf[r0] + cf * (mfxFlangeBuf[r1] - mfxFlangeBuf[r0]);
                    mfxFlangePos++;
                    if (mfxFlangePos >= kMfxFlangeLen) mfxFlangePos = 0;
                    out = in * 0.6f + d * 0.6f;
                    break;
                }
                case MFX_SLICER: {
                    mfxSlicePos++;
                    if (mfxSlicePos >= slicePeriod) mfxSlicePos = 0;
                    float pos = (float)mfxSlicePos / (float)slicePeriod;
                    float target = (pos < sliceDuty) ? 1.0f : 0.0f;
                    // Rampa de ~1 ms: un corte a pelo suena a chasquido, no a corte.
                    float step = 1.0f / (sr * 0.001f);
                    if (mfxSliceGain < target) { mfxSliceGain += step; if (mfxSliceGain > target) mfxSliceGain = target; }
                    else if (mfxSliceGain > target) { mfxSliceGain -= step; if (mfxSliceGain < target) mfxSliceGain = target; }
                    out = in * mfxSliceGain;
                    break;
                }
                default: break;
            }

            for (unsigned int ch = 0; ch < channels; ch++) pOutput[frame * channels + ch] = out;
        }
    }

    // Un tick del patrón: dispara los pads cuyo bit esté puesto.
    void padPatternTick() {
        int ticks = padPatSteps * PAD_PAT_RES;
        padPatTick = (padPatTick + 1) % ticks;
        unsigned long long mask = padPattern[padPatTick];
        if (mask == 0ULL) return;
        for (int pad = 0; pad < PAD_TOTAL; pad++) {
            if (mask & (1ULL << pad)) triggerPad(pad, 1.0f, true);
        }
    }

    // Voz de preview del editor de trim (una sola, a tono original).
    struct PreviewVoice {
        bool active = false;
        int slot = 0;
        unsigned long long a0 = 0, a1 = 0, cursor = 0;
    } preview;

    void trackerStep() {
        trackerRow = (trackerRow + 1) % TRACKER_ROWS;
        for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
            const TrackerCell& c = trackerCells[ch][trackerRow];
            if (c.sampleId < 0 || c.colorId == 0) continue;

            TrackerVoice& v = trackVoices[ch];
            v.sampleId = c.sampleId;
            float sign = (c.fx == 3) ? -1.0f : 1.0f;
            v.rate = powf(2.0f, (c.colorId - PITCH_BASE) / 12.0f) * sign;
            const InstrumentSource& srcTv = instrumentBank.at(c.sampleId);
            const SharedSample& a = srcTv.audio;
            unsigned long long ta0 = srcTv.audioTrimStart(), ta1 = srcTv.audioTrimEnd();
            v.cursor = sign < 0.0f ? (float)(ta1 > ta0 ? ta1 - 1 : ta0) : (float)ta0;
            v.isPlaying = (a.pPCM != nullptr && a.totalFrames > 0);
            v.hasReverb = (c.fx == 1);
            v.hasEcho = (c.fx == 2);
            v.hasChorus = (c.fx == 4);
            // c.fx == 3 (reverse) is handled by 'sign' above.
        }
    }

    int linearClampLen() const {
        int len = linearLength;
        if (len < 1) len = 1;
        if (len > LINEAR_COLS) len = LINEAR_COLS;
        return len;
    }

    // Un paso del playhead lineal: avanza una columna y dispara sus notas.
    void linearStep() {
        int len = linearClampLen();
        int next = linearCol + 1;
        if (next >= len) {
            if (linearLoop) next = 0;
            else return; // una sola pasada: se queda parado en la última columna
        }
        linearCol = next;
        fireLinearColumn(linearCol);
    }

    // Dispara las notas de una columna: cada fila re-dispara SU voz (la de esa
    // fila), como un notey que vuelve a tocar. Nunca crea voces nuevas, así el
    // video de esa fila se reinicia en vez de multiplicarse.
    void fireLinearColumn(int col) {
        if (col < 0 || col >= LINEAR_COLS) return;
        for (int row = 0; row < LINEAR_ROWS; row++) {
            const LinearCell& c = linearCells[col][row];
            if (c.sampleId < 0 || c.colorId == 0) continue;

            LinearVoice& v = linearVoices[row];
            v.sampleId = c.sampleId;
            float sign = (c.fx == 3) ? -1.0f : 1.0f;
            v.rate = powf(2.0f, (c.colorId - PITCH_BASE) / 12.0f) * sign;
            v.playbackRate = v.rate;
            const InstrumentSource& src = instrumentBank.at(c.sampleId);
            const SharedSample& a = src.audio;
            unsigned long long a0 = src.audioTrimStart(), a1 = src.audioTrimEnd();
            v.cursor = sign < 0.0f ? (float)(a1 > a0 ? a1 - 1 : a0) : (float)a0;
            v.isPlaying = (a.pPCM != nullptr && a.totalFrames > 0);
            v.hasReverb = (c.fx == 1);
            v.hasEcho = (c.fx == 2);
            v.hasChorus = (c.fx == 4);
            v.noteSeq++; // fuerza re-emisión del evento de video (efecto picado)
        }
    }

    // Dispara una nota "en vivo" (teclado MIDI / gamepad): toca el slot al tono
    // dado en una voz round-robin. Suena aunque el secuenciador esté en pausa.
    void triggerLive(int sampleId, unsigned char colorId, unsigned char fx) {
        if (colorId == 0) return;
        LiveVoice* chosen = nullptr;
        for (int i = 0; i < LIVE_VOICES; i++) {
            int idx = (liveVoiceRR + i) % LIVE_VOICES;
            if (!liveVoices[idx].isPlaying) { chosen = &liveVoices[idx]; liveVoiceRR = (idx + 1) % LIVE_VOICES; break; }
        }
        if (!chosen) { chosen = &liveVoices[liveVoiceRR]; liveVoiceRR = (liveVoiceRR + 1) % LIVE_VOICES; }

        LiveVoice& v = *chosen;
        v.sampleId = sampleId;
        float sign = (fx == 3) ? -1.0f : 1.0f;
        v.rate = powf(2.0f, (colorId - PITCH_BASE) / 12.0f) * sign;
        v.playbackRate = v.rate;
        const InstrumentSource& src = instrumentBank.at(sampleId);
        const SharedSample& a = src.audio;
        unsigned long long a0 = src.audioTrimStart(), a1 = src.audioTrimEnd();
        v.cursor = sign < 0.0f ? (float)(a1 > a0 ? a1 - 1 : a0) : (float)a0;
        v.isPlaying = (a.pPCM != nullptr && a.totalFrames > 0);
        v.hasReverb = (fx == 1);
        v.hasEcho = (fx == 2);
        v.hasChorus = (fx == 4);
        v.noteSeq++;
    }

    // Igual que triggerLive pero SIEMPRE en la voz dedicada ctrlVoice: al
    // re-disparar reusa el mismo id de video -> retrigger, sin multiplicar.
    void triggerControllable(int sampleId, unsigned char colorId, unsigned char fx) {
        if (colorId == 0) return;
        LiveVoice& v = ctrlVoice;
        v.sampleId = sampleId;
        float sign = (fx == 3) ? -1.0f : 1.0f;
        v.rate = powf(2.0f, (colorId - PITCH_BASE) / 12.0f) * sign;
        v.playbackRate = v.rate;
        const InstrumentSource& src = instrumentBank.at(sampleId);
        const SharedSample& a = src.audio;
        unsigned long long a0 = src.audioTrimStart(), a1 = src.audioTrimEnd();
        v.cursor = sign < 0.0f ? (float)(a1 > a0 ? a1 - 1 : a0) : (float)a0;
        v.isPlaying = (a.pPCM != nullptr && a.totalFrames > 0);
        v.hasReverb = (fx == 1);
        v.hasEcho = (fx == 2);
        v.hasChorus = (fx == 4);
        v.noteSeq++;
    }

public:
    // =======================================================================
    // PADS (modo BEATBOX)
    //
    // Esta parte es PÚBLICA aunque esté aquí abajo, entre lo privado: la
    // interfaz (escritorio y Android) necesita leer la configuración de un pad
    // para dibujarlo, y las pruebas disparan pads directamente. Los comandos
    // de la cola siguen siendo el camino normal desde el hilo principal.
    // =======================================================================

    // Golpea un pad. `velocity` en 0..1 (127 de MIDI ya dividido, o 1.0 desde
    // una tecla o un dedo). Suena SIEMPRE, esté el secuenciador en pausa o no:
    // una sampleadora tiene que sonar al tocarla aunque no haya nada corriendo.
    void triggerPad(int pad, float velocity, bool fromPattern = false) {
        if (pad < 0 || pad >= PAD_TOTAL) return;
        const PadConfig& pc = padCfg[pad];
        if (pc.empty()) return;

        // Grupo de corte: un pad con grupo silencia a los demás del MISMO
        // grupo antes de sonar. Es lo que hace que un charles abierto se calle
        // en cuanto entra el cerrado, en vez de sonar los dos a la vez.
        if (pc.choke != 0) {
            for (int i = 0; i < PAD_TOTAL; i++) {
                if (i == pad || !padVoices[i].isPlaying) continue;
                if (padCfg[i].choke == pc.choke) padVoices[i].isPlaying = false;
            }
        }

        // LOOP es un interruptor: si el pad ya está sonando en bucle, el
        // segundo golpe lo para. (Desde el patrón NO: allí un golpe siempre
        // re-dispara, o un bucle grabado se apagaría solo en la vuelta siguiente.)
        if (pc.mode == PAD_LOOP && !fromPattern && padVoices[pad].isPlaying && padVoices[pad].loop) {
            padVoices[pad].isPlaying = false;
            return;
        }

        PadVoice& v = padVoices[pad];
        const InstrumentSource& src = instrumentBank.at(pc.slot);
        const SharedSample& a = src.audio;
        float sign = (pc.fx == 3) ? -1.0f : 1.0f;
        v.sampleId = pc.slot;
        v.rate = powf(2.0f, pc.pitch / 12.0f) * sign;
        v.playbackRate = v.rate;
        unsigned long long a0 = src.audioTrimStart(), a1 = src.audioTrimEnd();
        v.cursor = sign < 0.0f ? (float)(a1 > a0 ? a1 - 1 : a0) : (float)a0;
        v.vol = pc.vol * (velocity < 0.0f ? 0.0f : (velocity > 1.0f ? 1.0f : velocity));
        v.loop = (pc.mode == PAD_LOOP);
        v.held = (pc.mode == PAD_GATE);
        v.hasReverb = (pc.fx == 1);
        v.hasEcho = (pc.fx == 2);
        v.hasChorus = (pc.fx == 4);
        v.isPlaying = (a.pPCM != nullptr && a.totalFrames > 0);
        v.noteSeq++;

        // Grabación del patrón: el golpe se escribe en el tick que suena AHORA.
        // Lo hace el hilo de audio, con su propio reloj — no el de dibujo — así
        // que lo que se graba es lo que se oyó, sin el desfase de un fotograma.
        if (padRecording && !fromPattern) recordPadHit(pad);
    }

    // Suelta un pad: solo importa en modo GATE (corta la nota).
    void releasePad(int pad) {
        if (pad < 0 || pad >= PAD_TOTAL) return;
        PadVoice& v = padVoices[pad];
        if (!v.isPlaying || !v.held) return;
        v.held = false;
        if (padCfg[pad].mode == PAD_GATE) v.isPlaying = false;
    }

    void stopAllPads() {
        for (int i = 0; i < PAD_TOTAL; i++) { padVoices[i].isPlaying = false; padVoices[i].held = false; }
    }

    void setPadConfig(int pad, const PadConfig& cfg) {
        if (pad < 0 || pad >= PAD_TOTAL) return;
        padCfg[pad] = cfg;
        // Un pad que deja de ser bucle no puede quedarse sonando para siempre.
        if (cfg.mode != PAD_LOOP) padVoices[pad].loop = false;
        if (cfg.empty()) padVoices[pad].isPlaying = false;
    }
    const PadConfig& getPadConfig(int pad) const {
        return padCfg[(pad < 0 || pad >= PAD_TOTAL) ? 0 : pad];
    }

    // --- Patrón ---
    void setPadPatternPlaying(bool on) {
        padPatPlaying = on;
        if (!on) { padPatTick = -1; padPatAccum = 0; padRecording = false; }
    }
    void setPadRecording(bool on) {
        padRecording = on;
        if (on) padPatPlaying = true; // grabar sin que el bucle corra no tiene sentido
    }
    void setPadQuantize(int q) { padQuantize = q < 0 ? 0 : (q > 4 ? 4 : q); }
    void setPadPatternLength(int steps) {
        if (steps < 4) steps = 4;
        if (steps > PAD_PAT_MAX_STEPS) steps = PAD_PAT_MAX_STEPS;
        padPatSteps = steps;
    }
    void clearPadPattern() {
        for (int t = 0; t < PAD_PAT_TICKS; t++) padPattern[t] = 0ULL;
        padPatVersion++;
    }
    // Borra solo las notas de UN pad: rectificar un golpe mal puesto sin
    // perder el resto del ritmo (el "erase" con el pad pulsado de una MPC).
    void clearPadTrack(int pad) {
        if (pad < 0 || pad >= PAD_TOTAL) return;
        unsigned long long keep = ~(1ULL << pad);
        for (int t = 0; t < PAD_PAT_TICKS; t++) padPattern[t] &= keep;
        padPatVersion++;
    }
    void setPadPatternStep(int tick, int pad, bool on) {
        if (tick < 0 || tick >= PAD_PAT_TICKS || pad < 0 || pad >= PAD_TOTAL) return;
        if (on) padPattern[tick] |= (1ULL << pad);
        else    padPattern[tick] &= ~(1ULL << pad);
        padPatVersion++;
    }

    // --- FX maestro ---
    void setMasterFx(int type, float x, float y, bool on) {
        if (type < 0 || type >= MFX_COUNT) type = MFX_OFF;
        mfxType = type;
        mfxX = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        mfxY = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
        mfxOn = on;
    }

private:
    int sampleRate = 44100;
    float bpm = 120.0f;
    bool paused = false;

    // Buffers de los buses de efectos (ver renderAudio). Miembros del
    // objeto global -> quedan inicializados a cero al arrancar.
    static const unsigned int MAX_RENDER_FRAMES = 8192;
    static const int kEchoLen = 11025; // 0.25s a 44100 Hz
    float busEcho[MAX_RENDER_FRAMES];
    float busReverb[MAX_RENDER_FRAMES];
    float busChorus[MAX_RENDER_FRAMES];
    float pdScratch[MAX_RENDER_FRAMES]; // mono mix bus for a patched slot
    float echoBuf[kEchoLen];
    int echoPos = 0;
    static const int kCombMax = 1617;
    const int kCombLen[4] = {1557, 1617, 1491, 1422};
    float combBuf[4][kCombMax];
    int combPos[4] = {0, 0, 0, 0};
    // Chorus: modulated delay line.
    static const int kChorusLen = 2048;
    float chorusBuf[kChorusLen];
    int chorusPos = 0;
    float chorusLfo = 0.0f;
    static constexpr float kChorusBase = 441.0f;   // ~10ms @ 44100
    static constexpr float kChorusDepth = 220.0f;  // ~5ms sweep
    static constexpr float kChorusLfoInc = 6.2831853f * 1.2f / 44100.0f; // ~1.2 Hz
    unsigned long long samplesPerStep = 0;

    // --- Estado del FX maestro (modo BEATBOX) ---
    // Todos los buffers son miembros de tamaño fijo, como el resto: el
    // callback de audio no reserva memoria ni cambia de efecto por su cuenta.
    int   mfxType = MFX_OFF;
    float mfxX = 0.5f, mfxY = 0.3f;
    bool  mfxOn = false;
    static const int kMfxDelayLen = 44100;     // hasta 1 s de retardo
    float mfxDelayBuf[kMfxDelayLen];
    int   mfxDelayPos = 0;
    float mfxLp1 = 0.0f, mfxBp = 0.0f;   // estado del filtro de variables de estado
    float mfxHold = 0.0f;                       // crush: muestra retenida
    int   mfxHoldCnt = 0;                       // crush: cuántas muestras lleva retenida
    float mfxPhase = 0.0f;                      // flanger: fase del LFO
    unsigned long long mfxSlicePos = 0;         // slicer: posición dentro del ciclo
    float mfxSliceGain = 1.0f;                  // slicer: ganancia suavizada (anti-clic)
    static const int kMfxFlangeLen = 1024;
    float mfxFlangeBuf[kMfxFlangeLen];
    int   mfxFlangePos = 0;
    static const int kMfxCombMax = 1801;
    const int kMfxCombLen[4] = {1687, 1801, 1601, 1523};
    float mfxCombBuf[4][kMfxCombMax];
    int   mfxCombPos[4] = {0, 0, 0, 0};

    MidiNoteCallback midiCallback = nullptr;
    void* midiUserData = nullptr;

    void recomputeStepDuration() {
        float stepSeconds = 60.0f / (bpm * 4.0f);
        samplesPerStep = (unsigned long long)(stepSeconds * (float)sampleRate);
        if (samplesPerStep < 1) samplesPerStep = 1;
    }

    GridCell& cellAt(int x, int y) {
        return cells[y * width + x];
    }

    void resetGrid() {
        for (int i = 0; i < MAX_GRID_WIDTH * MAX_GRID_HEIGHT; i++) {
            cells[i] = GridCell();
        }
    }

    void applyCommand(const Command& cmd) {
        switch (cmd.type) {
            case CommandType::PaintCell: {
                if (cmd.cellX < 0 || cmd.cellX >= width || cmd.cellY < 0 || cmd.cellY >= height) break;
                GridCell& cell = cellAt(cmd.cellX, cmd.cellY);
                cell.colorId = cmd.colorId;
                cell.modifier = (ModifierType)cmd.modifier;
                cell.targetX = cmd.targetX;
                cell.targetY = cmd.targetY;
                cell.nextDx = cmd.nextDx;
                cell.nextDy = cmd.nextDy;
                cell.midiNote = cmd.midiNote;
                cell.sustainSeconds = cmd.sustainSeconds;
                cell.holdSeconds = cmd.holdSeconds;
                cell.volMul = cmd.cellVolMul;
                cell.timeMul = cmd.cellTimeMul;
                cell.sampleId = cmd.sampleId;
                break;
            }
            case CommandType::PaintCompound: {
                if (cmd.cellX < 0 || cmd.cellX >= width || cmd.cellY < 0 || cmd.cellY >= height) break;
                GridCell& cell = cellAt(cmd.cellX, cmd.cellY);
                cell = GridCell();
                int n = cmd.compCount; if (n > MAX_CELL_ACTIONS) n = MAX_CELL_ACTIONS; if (n < 0) n = 0;
                cell.actionCount = n;
                for (int i = 0; i < n; i++) {
                    cell.actions[i].type = cmd.compType[i];
                    cell.actions[i].a = cmd.compA[i];
                    cell.actions[i].b = cmd.compB[i];
                    cell.actions[i].f = cmd.compF[i];
                }
                break;
            }
            case CommandType::SpawnBicho: {
                spawnBichoInternal(cmd.spawnX, cmd.spawnY, cmd.spawnDx, cmd.spawnDy,
                                   cmd.sampleId, cmd.spawnTempoMul, cmd.mutedFlag != 0, cmd.volumeVal,
                                   cmd.stoppedFlag != 0);
                break;
            }
            case CommandType::SetBichoMuted: {
                if (cmd.bichoIndex >= 0 && cmd.bichoIndex < MAX_BICHOS_POOL) {
                    bichos[cmd.bichoIndex].muted = (cmd.mutedFlag != 0);
                    if (bichos[cmd.bichoIndex].muted) bichos[cmd.bichoIndex].isPlaying = false;
                }
                break;
            }
            case CommandType::SetBichoStopped: {
                if (cmd.bichoIndex >= 0 && cmd.bichoIndex < MAX_BICHOS_POOL) {
                    bichos[cmd.bichoIndex].stopped = (cmd.stoppedFlag != 0);
                    if (bichos[cmd.bichoIndex].stopped) bichos[cmd.bichoIndex].isPlaying = false;
                }
                break;
            }
            case CommandType::SetBichoVolume: {
                if (cmd.bichoIndex >= 0 && cmd.bichoIndex < MAX_BICHOS_POOL)
                    bichos[cmd.bichoIndex].volume = cmd.volumeVal < 0.0f ? 0.0f : cmd.volumeVal;
                break;
            }
            case CommandType::SetTempo: {
                bpm = cmd.newBpm;
                recomputeStepDuration();
                break;
            }
            case CommandType::RemoveBicho: {
                if (cmd.bichoIndex >= 0 && cmd.bichoIndex < MAX_BICHOS_POOL) {
                    bichos[cmd.bichoIndex].isActive = false;
                }
                break;
            }
            case CommandType::ClearGrid: {
                resetGrid();
                break;
            }
            case CommandType::ClearBichos: {
                for (int i = 0; i < MAX_BICHOS_POOL; i++) {
                    bichos[i].isActive = false;
                }
                break;
            }
            case CommandType::SetPaused: {
                paused = (cmd.pausedState != 0);
                break;
            }
            case CommandType::SetGridSize: {
                setGridSize(cmd.newGridW, cmd.newGridH);
                resetGrid();
                break;
            }
            case CommandType::SetTrackerCell: {
                if (cmd.trkChannel < 0 || cmd.trkChannel >= TRACKER_CHANNELS) break;
                if (cmd.trkRow < 0 || cmd.trkRow >= TRACKER_ROWS) break;
                TrackerCell& c = trackerCells[cmd.trkChannel][cmd.trkRow];
                c.sampleId = cmd.sampleId;
                c.colorId = cmd.colorId;
                c.fx = (unsigned char)cmd.modifier;
                break;
            }
            case CommandType::ClearTracker: {
                for (int ch = 0; ch < TRACKER_CHANNELS; ch++) {
                    for (int r = 0; r < TRACKER_ROWS; r++) trackerCells[ch][r] = TrackerCell();
                }
                break;
            }
            case CommandType::ResetPlayhead: {
                trackerRow = -1;   // el próximo paso cae en la fila 0
                trackerAccum = 0;
                for (int t = 0; t < TRACKER_CHANNELS; t++) trackVoices[t].isPlaying = false;
                linearCol = -1;    // el próximo paso cae en la columna 0
                linearAccum = 0;
                for (int t = 0; t < LINEAR_ROWS; t++) linearVoices[t].isPlaying = false;
                break;
            }
            case CommandType::SetLinearCell: {
                if (cmd.linCol < 0 || cmd.linCol >= LINEAR_COLS) break;
                if (cmd.linRow < 0 || cmd.linRow >= LINEAR_ROWS) break;
                LinearCell& c = linearCells[cmd.linCol][cmd.linRow];
                c.sampleId = cmd.sampleId;
                c.colorId = cmd.colorId;
                c.fx = (unsigned char)cmd.modifier;
                break;
            }
            case CommandType::ClearLinear: {
                for (int col = 0; col < LINEAR_COLS; col++)
                    for (int row = 0; row < LINEAR_ROWS; row++) linearCells[col][row] = LinearCell();
                break;
            }
            case CommandType::SetLinearParams: {
                int len = cmd.linLen;
                if (len < 1) len = 1;
                if (len > LINEAR_COLS) len = LINEAR_COLS;
                linearLength = len;
                linearLoop = (cmd.linLoop != 0);
                break;
            }
            case CommandType::TriggerLive: {
                triggerLive(cmd.sampleId, cmd.colorId, (unsigned char)cmd.modifier);
                break;
            }
            case CommandType::TriggerControllable: {
                triggerControllable(cmd.sampleId, cmd.colorId, (unsigned char)cmd.modifier);
                break;
            }
            // --- Modo BEATBOX ---
            case CommandType::PadTrigger: {
                triggerPad(cmd.padIndex, cmd.padVelocity);
                break;
            }
            case CommandType::PadRelease: {
                releasePad(cmd.padIndex);
                break;
            }
            case CommandType::PadStopAll: {
                stopAllPads();
                break;
            }
            case CommandType::SetPadConfig: {
                PadConfig pc;
                pc.slot = cmd.sampleId;
                pc.pitch = (signed char)cmd.padPitch;
                pc.vol = cmd.padVol;
                pc.mode = (unsigned char)(cmd.padMode < 0 ? 0 : (cmd.padMode > PAD_LOOP ? 0 : cmd.padMode));
                pc.choke = (unsigned char)(cmd.padChoke < 0 ? 0 : (cmd.padChoke > 4 ? 0 : cmd.padChoke));
                pc.fx = (unsigned char)(cmd.padFx < 0 ? 0 : (cmd.padFx > 4 ? 0 : cmd.padFx));
                setPadConfig(cmd.padIndex, pc);
                break;
            }
            case CommandType::SetPadTransport: {
                setPadPatternLength(cmd.padSteps);
                setPadQuantize(cmd.padQuant);
                // Grabar implica reproducir; parar implica dejar de grabar.
                bool play = cmd.padPlayFlag != 0 || cmd.padRecFlag != 0;
                setPadPatternPlaying(play);
                padRecording = play && cmd.padRecFlag != 0;
                break;
            }
            case CommandType::ClearPadPattern: {
                // padIndex < 0 = el patrón entero; si no, solo esa pista.
                if (cmd.padIndex < 0) clearPadPattern();
                else                  clearPadTrack(cmd.padIndex);
                break;
            }
            case CommandType::SetPadStep: {
                setPadPatternStep(cmd.padTick, cmd.padIndex, cmd.padOn != 0);
                break;
            }
            case CommandType::SetMasterFx: {
                setMasterFx(cmd.mfxType, cmd.mfxX, cmd.mfxY, cmd.mfxOn != 0);
                break;
            }
            case CommandType::PreviewPlay: {
                unsigned long long a0 = (unsigned long long)(cmd.previewA0 < 0 ? 0 : cmd.previewA0);
                unsigned long long a1 = (unsigned long long)(cmd.previewA1 < 0 ? 0 : cmd.previewA1);
                if (preview.active && preview.slot == cmd.sampleId) {
                    // Ya sonando este slot: solo actualizar el rango.
                    preview.a0 = a0;
                    preview.a1 = a1;
                    if (preview.cursor < a0 || preview.cursor >= a1) preview.cursor = a0;
                } else {
                    preview.active = true;
                    preview.slot = cmd.sampleId;
                    preview.a0 = a0;
                    preview.a1 = a1;
                    preview.cursor = a0;
                }
                break;
            }
            case CommandType::PreviewStop: {
                preview.active = false;
                break;
            }
            default:
                break;
        }
    }

    void spawnBichoInternal(int x, int y, int dx, int dy, int sampleId, float tempoMul, bool muted, float spawnVolume, bool stopped = false) {
        for (int i = 0; i < MAX_BICHOS_POOL; i++) {
            if (!bichos[i].isActive) {
                Bicho& b = bichos[i];
                b = Bicho();
                b.isActive = true;
                b.x = x;
                b.y = y;
                b.dx = dx;
                b.dy = dy;
                b.sampleId = sampleId;
                b.tempoMul = tempoMul;
                b.muted = muted;
                b.stopped = stopped;
                b.volume = spawnVolume;
                break;
            }
        }
    }

    // Un paso de UN bicho. stepLen es el largo de paso PROPIO de este bicho
    // (ya dividido por su tempoMul), necesario para descontar el sustain a
    // su propio ritmo.
    void stepBicho(Bicho& b, unsigned long long stepLen) {
        if (b.isSustained) {
            b.sustainRemainingSamples -= (long long)stepLen;
            if (b.sustainRemainingSamples <= 0) b.isSustained = false;
            return;
        }

        int nextX = b.x + b.dx;
        int nextY = b.y + b.dy;

        if (nextX < 0 || nextX >= width) { b.dx *= -1; nextX = b.x; }
        if (nextY < 0 || nextY >= height) { b.dy *= -1; nextY = b.y; }

        b.x = nextX;
        b.y = nextY;

        GridCell& cell = cellAt(b.x, b.y);

        // Fase 3: celda COMPUESTA -> aplica su lista de acciones y termina.
        if (cell.actionCount > 0) { applyCompound(b, cell); return; }

        if (cell.modifier == ModifierType::Teleport) {
            b.x = cell.targetX;
            b.y = cell.targetY;
        } else if (cell.modifier == ModifierType::DirectionChange) {
            b.dx = cell.nextDx;
            b.dy = cell.nextDy;
        } else if (cell.modifier == ModifierType::Sustain) {
            // Formato antiguo: la espera venía como modificador. Se conserva
            // para los proyectos ya guardados; lo nuevo usa cell.holdSeconds.
            b.isSustained = true;
            b.sustainRemainingSamples = (long long)(cell.sustainSeconds * sampleRate);
        } else if (cell.modifier == ModifierType::Silence) {
            // Celda de silencio: corta la nota en seco. publishVideoEvents
            // verá la transición y también apagará el video del bicho.
            b.isPlaying = false;
        } else if (cell.modifier == ModifierType::MidiOut && midiCallback != nullptr) {
            midiCallback(cell.midiNote, 100, midiUserData);
        }

        // Espera SUPERPUESTA: es independiente del modificador, así que una
        // celda puede llevar a la vez un FX y una espera. Y como no exige que
        // la celda tenga nota, una celda de espera sola es un SILENCIO: el
        // bicho se para ahí el tiempo elegido sin disparar nada.
        if (cell.holdSeconds > 0.0f) {
            b.isSustained = true;
            b.sustainRemainingSamples = (long long)(cell.holdSeconds * sampleRate);
        }

        b.hasBitcrush = (cell.modifier == ModifierType::Bitcrush);
        float directionSign = (cell.modifier == ModifierType::Reverse) ? -1.0f : 1.0f;

        if (cell.colorId > 0 && !b.muted) {
            int semiTones = (cell.colorId - PITCH_BASE);
            // timeMul es cámara lenta/rápida "de cinta": mueve el cursor más
            // despacio o más deprisa, y el vídeo sigue a playbackRate, así que
            // imagen y sonido se estiran juntos.
            b.playbackRate = powf(2.0f, semiTones / 12.0f) * directionSign * cell.timeMul;
            b.cellVol = cell.volMul;
            b.sampleId = cell.sampleId;
            // El efecto de la celda queda pegado a ESTA nota hasta el
            // próximo disparo (no se limpia al pisar celdas vacías).
            b.hasEcho = (cell.modifier == ModifierType::EchoFx);
            b.hasReverb = (cell.modifier == ModifierType::ReverbFx);
            b.hasChorus = (cell.modifier == ModifierType::ChorusFx);

            const InstrumentSource& srcT = instrumentBank.at(b.sampleId);
            unsigned long long a0 = srcT.audioTrimStart(), a1 = srcT.audioTrimEnd();
            if (directionSign < 0.0f) {
                b.playbackCursor = (float)(a1 > a0 ? a1 - 1 : a0);
            } else {
                b.playbackCursor = (float)a0;
            }
            b.isPlaying = true;
            b.noteSeq++;
        }
    }

    // Fase 3: aplica la lista de acciones de una celda compuesta, en orden. Las
    // acciones FX preceden a un NOTE para pegarle el efecto; TELEPORT mueve el
    // bicho; HOLD lo detiene; TURN cambia su rumbo; MUTE corta la nota.
    void applyCompound(Bicho& b, const GridCell& c) {
        bool wantNote = false; int noteColor = 0, noteSlot = 0;
        bool rev = false, fxE = false, fxR = false, fxC = false;
        int n = c.actionCount > MAX_CELL_ACTIONS ? MAX_CELL_ACTIONS : c.actionCount;
        for (int i = 0; i < n; i++) {
            const CellAction& a = c.actions[i];
            switch (a.type) {
                case CA_TURN:     b.dx = a.a; b.dy = a.b; break;
                case CA_HOLD:     b.isSustained = true; b.sustainRemainingSamples = (long long)(a.f * sampleRate); break;
                case CA_MUTE:     b.isPlaying = false; break;
                case CA_TELEPORT: b.x = a.a; b.y = a.b; break;
                case CA_FX:
                    if (a.a == 1) fxR = true;
                    else if (a.a == 2) fxE = true;
                    else if (a.a == 3) rev = true;
                    else if (a.a == 4) fxC = true;
                    break;
                case CA_NOTE:     wantNote = true; noteColor = a.a; noteSlot = a.b; break;
                default: break;
            }
        }
        if (wantNote && !b.muted && noteColor > 0) {
            float sign = rev ? -1.0f : 1.0f;
            b.playbackRate = powf(2.0f, (noteColor - PITCH_BASE) / 12.0f) * sign;
            b.sampleId = noteSlot;
            b.hasEcho = fxE; b.hasReverb = fxR; b.hasChorus = fxC;
            b.cellVol = 1.0f;  // las celdas de mod no llevan volumen propio
            const InstrumentSource& s = instrumentBank.at(noteSlot);
            unsigned long long a0 = s.audioTrimStart(), a1 = s.audioTrimEnd();
            b.playbackCursor = sign < 0.0f ? (float)(a1 > a0 ? a1 - 1 : a0) : (float)a0;
            b.isPlaying = true;
            b.noteSeq++;
        }
    }
};
