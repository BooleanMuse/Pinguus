#pragma once
// PitchToNotes.h
//
// DE UN SONIDO A UNA MELODÍA: coge un trozo de audio (lo que acabas de tararear
// al micrófono, un sample, la pista de un clip) y saca la lista de NOTAS que
// contiene — qué tono, cuándo empieza y cuánto dura. Con eso, el resto del
// programa puede pintar esa melodía en el lienzo, en el modo lineal o en el
// tracker, y tocarla con cualquier clip.
//
// La idea: cantar es mucho más rápido que colocar notas a mano, y casi todo el
// mundo sabe cantar la melodía que quiere aunque no sepa dónde cae en un
// teclado.
//
// SIN DEPENDENCIAS Y SIN raylib: son matemáticas sobre un array de float, así
// que compila igual en Linux, en Windows y en arm64, y —lo que importa— se
// puede PROBAR en el escritorio con señales inventadas de tono conocido. Ver
// tests/test_pitch.cpp.
//
// ---------------------------------------------------------------------------
// CÓMO SE DETECTA EL TONO
//
// Con YIN (de Cheveigné y Kawahara, 2002), que es autocorrelación bien hecha.
// La idea de fondo: una señal periódica se parece mucho a sí misma desplazada
// justo un periodo. Así que se mide, para cada desplazamiento tau, cuánto se
// DIFERENCIA la señal de su copia desplazada; el tau donde esa diferencia cae a
// un mínimo es el periodo, y la frecuencia es la tasa de muestreo entre tau.
//
// La autocorrelación a secas se equivoca de octava constantemente (el doble del
// periodo también "se parece"). YIN añade dos cosas que lo arreglan: normaliza
// la diferencia por la media acumulada —lo que penaliza los tau grandes— y
// coge el PRIMER mínimo que baja de un umbral en vez del más profundo. Eso es
// justo lo que evita que un LA acabe detectado como el LA de una octava abajo.
//
// POR QUÉ SE REMUESTREA A 16 kHz
//   El coste es ventana x tau_máximo por cada trozo analizado. A 44100 Hz eso
//   son 2048 x 630 = 1,3 millones de operaciones por trozo, y hay unos cien
//   trozos por segundo de audio: en un teléfono, varios segundos de espera. A
//   16 kHz caben de sobra los 1200 Hz más agudos que se buscan (Nyquist son
//   8000) y la cuenta baja a 1024 x 228, unas seis veces menos. Es la
//   diferencia entre "tarda un momento" y "parece colgado".
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace PitchToNotes {

// Una nota encontrada. `semitone` es RELATIVO AL DO CENTRAL, que es la misma
// convención que usa todo Pinguus (semitono 0 = C4 = nota MIDI 60).
struct Note {
    int   semitone = 0;
    float startSec = 0.0f;
    float lenSec = 0.0f;
    float peak = 1.0f;      // 0..1, lo fuerte que sonaba
};

struct Options {
    // Rango de búsqueda. Por defecto cubre desde una voz masculina grave hasta
    // un silbido: buscar más abajo sólo invita a confundir el zumbido de la red
    // eléctrica con una nota.
    float minHz = 70.0f;
    float maxHz = 1200.0f;
    // Umbral de YIN: cuánto tiene que parecerse la onda a sí misma para dar el
    // tono por bueno. Más BAJO = más exigente, o sea menos notas pero más
    // seguras. 0,20 deja pasar una voz normal grabada con el micrófono del
    // portátil; con 0,15 se perdían tramos enteros de un tarareo perfectamente
    // audible sólo porque la señal no era de laboratorio.
    float threshold = 0.20f;
    // Por debajo de esto es silencio. En amplitud lineal, no en decibelios,
    // para que el mando de la interfaz sea un deslizador normal.
    float silence = 0.010f;
    // Una nota más corta que esto es un chasquido o un tropiezo de la voz, no
    // una nota. Al cuantizar, además, no llegaría ni a una semicorchea.
    float minNoteSec = 0.07f;
    // Cuánto tiene que cambiar el tono para contar como NOTA NUEVA y no como el
    // vibrato o el desliz de la misma. Medio semitono es lo que separa cantar
    // regular de cantar otra nota.
    float newNoteSemis = 0.6f;
    // HUECOS QUE SE PERDONAN dentro de una nota. Al cantar hay microcortes
    // constantes: una consonante, una respiración, un golpe de glotis. Sin esto,
    // cada uno de ellos partía la nota en dos —o la dejaba por debajo del
    // mínimo y desaparecía—, y de un tarareo de cinco segundos salían tres
    // notas. Con 80 ms de perdón, la misma grabación da la melodía entera.
    float maxGapSec = 0.08f;
    int   transpose = 0;
};

struct Result {
    std::vector<Note> notes;
    int   framesAnalysed = 0;
    int   framesVoiced = 0;
    float medianSemitone = 0.0f;   // útil para ofrecer un transporte automático
};

// Una nota ya colocada en la rejilla del secuenciador.
struct StepNote {
    int semitone = 0;
    int step = 0;        // en semicorcheas desde el principio
    int lenSteps = 1;
    float peak = 1.0f;
};

namespace detail {

// Remuestreo a la tasa de trabajo con un promediado de los que caen dentro.
// No es un filtro de libro, pero promediar el bloque que se descarta YA es un
// paso bajo: sin ello, un siseo agudo se plegaría sobre el rango de la voz y
// aparecerían notas donde no hay ninguna.
inline void Downsample(const float* in, size_t n, int srcRate, int dstRate,
                       std::vector<float>& out) {
    if (srcRate <= dstRate) {
        out.assign(in, in + n);
        return;
    }
    const double ratio = (double)srcRate / (double)dstRate;
    const size_t outN = (size_t)((double)n / ratio);
    out.resize(outN);
    for (size_t i = 0; i < outN; i++) {
        const size_t a = (size_t)(i * ratio);
        size_t b = (size_t)((i + 1) * ratio);
        if (b > n) b = n;
        if (b <= a) { out[i] = in[a < n ? a : n - 1]; continue; }
        float acc = 0.0f;
        for (size_t j = a; j < b; j++) acc += in[j];
        out[i] = acc / (float)(b - a);
    }
}

// YIN sobre UNA ventana. Devuelve la frecuencia en Hz, o 0 si no hay tono
// claro. `d` y `cmnd` se pasan de fuera para no reservar memoria por ventana.
inline float YinWindow(const float* x, int W, int sampleRate,
                       int tauMin, int tauMax, float threshold,
                       std::vector<float>& d, std::vector<float>& cmnd) {
    if (tauMax >= W) tauMax = W - 1;
    if (tauMin < 1) tauMin = 1;
    if (tauMin >= tauMax) return 0.0f;

    d.assign(tauMax + 1, 0.0f);
    cmnd.assign(tauMax + 1, 1.0f);

    // 1. Función de diferencia.
    for (int tau = 1; tau <= tauMax; tau++) {
        float sum = 0.0f;
        const int lim = W - tau;
        for (int j = 0; j < lim; j++) {
            const float diff = x[j] - x[j + tau];
            sum += diff * diff;
        }
        d[tau] = sum;
    }

    // 2. Normalización por la media acumulada. Esto es lo que quita el sesgo
    //    hacia los periodos largos, o sea lo que evita el error de octava.
    float running = 0.0f;
    for (int tau = 1; tau <= tauMax; tau++) {
        running += d[tau];
        cmnd[tau] = (running > 0.0f) ? d[tau] * (float)tau / running : 1.0f;
    }

    // 3. El PRIMER mínimo que baja del umbral, no el más profundo: el más
    //    profundo suele estar en el doble del periodo.
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; tau++) {
        if (cmnd[tau] < threshold) {
            while (tau + 1 <= tauMax && cmnd[tau + 1] < cmnd[tau]) tau++;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        // Nada bajó del umbral: se coge el mínimo global y se acepta sólo si
        // es razonablemente bueno. Si no, no hay tono (ruido, silencio, dos
        // notas a la vez).
        float lo = 1e9f;
        for (int tau = tauMin; tau <= tauMax; tau++)
            if (cmnd[tau] < lo) { lo = cmnd[tau]; best = tau; }
        if (best < 0 || lo > threshold * 2.5f) return 0.0f;
    }

    // 4. Interpolación parabólica: el periodo real casi nunca cae en una
    //    muestra exacta, y sin esto el tono se va hasta medio semitono en las
    //    frecuencias altas (donde tau es pequeño y cada muestra pesa mucho).
    float period = (float)best;
    if (best > tauMin && best < tauMax) {
        const float a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
        const float den = 2.0f * (2.0f * b - a - c);
        if (fabsf(den) > 1e-9f) period += (c - a) / den;
    }
    if (period <= 0.0f) return 0.0f;
    return (float)sampleRate / period;
}

inline float Median(std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

} // namespace detail

// ---------------------------------------------------------------------------
// El análisis completo: audio -> lista de notas.
// ---------------------------------------------------------------------------
inline Result Analyse(const float* pcm, size_t frames, int sampleRate, const Options& opt) {
    Result r;
    if (!pcm || frames == 0 || sampleRate <= 0) return r;

    // Tasa de trabajo. 16 kHz llega para los 1200 Hz más agudos que se buscan y
    // hace la cuenta seis veces más barata (ver la cabecera).
    const int WORK_RATE = 16000;
    std::vector<float> sig;
    detail::Downsample(pcm, frames, sampleRate, WORK_RATE, sig);
    if (sig.size() < 512) return r;

    // Ventana de 64 ms y salto de 10 ms: la ventana tiene que abarcar varios
    // periodos de la nota más grave (70 Hz son 14 ms) y el salto marca con qué
    // finura se sitúa el comienzo de cada nota.
    const int W = 1024;                    // 64 ms a 16 kHz
    const int hop = 160;                   // 10 ms
    const int tauMin = (int)(WORK_RATE / opt.maxHz);
    const int tauMax = (int)(WORK_RATE / opt.minHz);
    const float secPerHop = (float)hop / (float)WORK_RATE;

    std::vector<float> midi, rms, dbuf, cbuf;
    for (size_t off = 0; off + W <= sig.size(); off += hop) {
        const float* x = sig.data() + off;

        float acc = 0.0f;
        for (int j = 0; j < W; j++) acc += x[j] * x[j];
        const float level = sqrtf(acc / (float)W);

        float m = 0.0f;                    // 0 = sin nota
        if (level >= opt.silence) {
            const float hz = detail::YinWindow(x, W, WORK_RATE, tauMin, tauMax,
                                               opt.threshold, dbuf, cbuf);
            if (hz >= opt.minHz && hz <= opt.maxHz)
                m = 69.0f + 12.0f * log2f(hz / 440.0f);   // en notas MIDI
        }
        midi.push_back(m);
        rms.push_back(level);
    }
    r.framesAnalysed = (int)midi.size();
    if (midi.empty()) return r;

    // Filtro de mediana de cinco: la voz da tumbos, y un solo trozo detectado
    // una octava arriba metería una nota fantasma. La mediana se los come sin
    // redondear los cambios de nota de verdad, cosa que un promedio sí haría.
    std::vector<float> sm = midi;
    for (size_t i = 0; i < midi.size(); i++) {
        std::vector<float> w;
        for (int k = -2; k <= 2; k++) {
            const long j = (long)i + k;
            if (j < 0 || j >= (long)midi.size()) continue;
            if (midi[j] > 0.0f) w.push_back(midi[j]);
        }
        // Un trozo sin tono se respeta: es el silencio que separa dos notas y
        // rellenarlo por mediana las pegaría en una sola.
        sm[i] = (midi[i] > 0.0f && !w.empty()) ? detail::Median(w) : 0.0f;
    }

    for (float v : sm) if (v > 0.0f) r.framesVoiced++;

    // --- De trozos a NOTAS ---
    //
    // EL EMBORRONADO DE LA VENTANA. Una ventana de 64 ms "ve" la nota en cuanto
    // la roza, así que un sonido de 20 ms enciende unas SEIS ventanas seguidas y
    // parece durar 84 ms. Si no se tiene en cuenta, un chasquido pasa por nota.
    //
    // Se arregla en dos pasos. Primero, cada trozo se fecha por el CENTRO de su
    // ventana y no por su principio: así el comienzo de la nota cae donde toca
    // en vez de medio segundo antes. Y segundo, al medir la duración se
    // descuenta media ventana por cada extremo QUE LINDE CON SILENCIO, que es
    // justo donde el emborronado alarga. Entre dos notas pegadas no se descuenta
    // nada: ahí el corte lo marca el cambio de tono, que sí cae en su sitio.
    const float winSec = (float)W / (float)WORK_RATE;
    std::vector<float> cur;                 // tonos del trozo en curso
    int curStart = -1;
    float curPeak = 0.0f;
    bool startedAfterSilence = true;        // el primer trozo siempre viene de silencio

    auto flush = [&](int endIdx, bool endedBySilence) {
        if (curStart < 0) return;
        const float span = (endIdx - curStart) * secPerHop;
        const float smear = (startedAfterSilence ? winSec * 0.5f : 0.0f) +
                            (endedBySilence      ? winSec * 0.5f : 0.0f);
        const float len = span - smear;
        if (len >= opt.minNoteSec && !cur.empty()) {
            Note n;
            n.semitone = (int)lroundf(detail::Median(cur)) - 60 + opt.transpose;
            // El centro de la primera ventana, más la media ventana que sobra
            // por delante si venía de silencio.
            n.startSec = curStart * secPerHop + winSec * 0.5f +
                         (startedAfterSilence ? winSec * 0.5f : 0.0f);
            n.lenSec = len;
            n.peak = curPeak;
            r.notes.push_back(n);
        }
        curStart = -1;
        cur.clear();
        curPeak = 0.0f;
    };

    // Un hueco corto NO termina la nota: se cuenta cuántos trozos seguidos
    // vienen sin tono y sólo se corta si pasan de opt.maxGapSec. La nota
    // termina en el último trozo CON tono, no donde se acabó de contar, para
    // que el silencio perdonado no se sume a su duración.
    int lastVoiced = -1;
    int silentRun = 0;

    for (size_t i = 0; i < sm.size(); i++) {
        const float m = sm[i];
        if (m <= 0.0f) {
            if (curStart >= 0) {
                silentRun++;
                if ((float)silentRun * secPerHop > opt.maxGapSec) {
                    flush(lastVoiced + 1, true);
                    startedAfterSilence = true;
                    silentRun = 0;
                }
            }
            continue;
        }
        silentRun = 0;

        if (curStart < 0) {
            curStart = (int)i;
            cur.clear();
            curPeak = 0.0f;
        } else {
            // ¿Sigue siendo la misma nota? Se compara con la MEDIANA de lo que
            // lleva, no con el trozo anterior: así un vibrato ancho no parte la
            // nota en trocitos, pero un salto de verdad sí la corta.
            std::vector<float> tmp = cur;
            if (fabsf(m - detail::Median(tmp)) > opt.newNoteSemis) {
                flush(lastVoiced + 1, false);   // corta el tono, no el silencio
                startedAfterSilence = false;
                curStart = (int)i;
            }
        }
        cur.push_back(m);
        if (rms[i] > curPeak) curPeak = rms[i];
        lastVoiced = (int)i;
    }
    flush(lastVoiced + 1, true);

    // El volumen se normaliza contra la nota más fuerte: lo que importa es el
    // relieve entre notas, no cuánto se acercó el micrófono a la boca.
    float loudest = 0.0f;
    for (const Note& n : r.notes) if (n.peak > loudest) loudest = n.peak;
    if (loudest > 0.0f)
        for (Note& n : r.notes) n.peak = n.peak / loudest;

    std::vector<float> all;
    for (const Note& n : r.notes) all.push_back((float)n.semitone);
    r.medianSemitone = detail::Median(all);
    return r;
}

// ---------------------------------------------------------------------------
// De notas en segundos a notas en PASOS del secuenciador.
//
// `stepsPerBeat` = 4 son semicorcheas, que es la rejilla de Pinguus. Con
// `quantize` en falso se sigue devolviendo un paso (la rejilla es lo único que
// hay), pero sin redondear al más cercano: se trunca, así lo que caiga justo
// después de un paso no se adelanta al anterior.
// ---------------------------------------------------------------------------
inline std::vector<StepNote> ToSteps(const Result& r, float bpm, int stepsPerBeat,
                                     bool quantize, int maxSteps, bool trimLeading = true) {
    std::vector<StepNote> out;
    if (bpm <= 0.0f || stepsPerBeat <= 0) return out;
    const float stepSec = 60.0f / (bpm * (float)stepsPerBeat);

    // El silencio DE DELANTE se recorta. Una melodía del banco es una pieza que
    // se estampa donde se pinche, así que dónde empezaste a cantar dentro de la
    // grabación no significa nada — y dejarlo dentro gastaba media rejilla en
    // casillas vacías antes de la primera nota.
    float offset = 0.0f;
    if (trimLeading && !r.notes.empty()) offset = r.notes.front().startSec;

    int lastStep = -1;
    for (const Note& n : r.notes) {
        StepNote s;
        const float raw = (n.startSec - offset) / stepSec;
        s.step = quantize ? (int)lroundf(raw) : (int)raw;
        s.lenSteps = (int)lroundf(n.lenSec / stepSec);
        if (s.lenSteps < 1) s.lenSteps = 1;
        s.semitone = n.semitone;
        s.peak = n.peak;
        if (s.step < 0) s.step = 0;
        // Dos notas en el mismo paso: al cuantizar pasa con notas muy seguidas.
        // Se empuja la segunda al paso siguiente en vez de perderla — perder
        // una nota sin decir nada es peor que moverla una semicorchea.
        if (s.step <= lastStep) s.step = lastStep + 1;
        if (maxSteps > 0 && s.step >= maxSteps) break;
        lastStep = s.step;
        out.push_back(s);
    }
    return out;
}

} // namespace PitchToNotes
