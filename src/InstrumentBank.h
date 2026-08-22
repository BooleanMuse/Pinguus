#pragma once

// <cstdio> va ANTES de pl_mpeg.h a propósito: pl_mpeg declara funciones que
// toman FILE* sin incluir stdio él mismo. Estando debajo, esta cabecera sólo
// compilaba de rebote, porque main.cpp ya había incluido <cstdio> antes; al
// incluirla desde cualquier otro sitio (el motor solo, el puerto a Android)
// fallaba con "'FILE' was not declared in this scope".
#include <cstdio>

// Esta cabecera USA la API de miniaudio (ma_decoder) y close(), pero no los
// incluía: funcionaba de rebote porque main.cpp incluye miniaudio.h antes (y
// miniaudio arrastra <unistd.h>). Incluirlo aquí hace que el motor se sostenga
// solo, que es justo lo que necesita el puerto a Android. Cuando main.cpp ya lo
// incluyó con MA_IMPLEMENTATION, esto es un no-op por la guarda de la cabecera.
#include "miniaudio.h"

// close() en MakeTempMpgPath. Sólo llegaba aquí cuando miniaudio se compilaba
// con MA_IMPLEMENTATION (que sí incluye <unistd.h>); pidiéndolo explícitamente
// la cabecera vale también para quien sólo quiere las declaraciones.
#if !defined(_WIN32)
  #include <unistd.h>
#endif

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cctype>

#define MAX_INSTRUMENTS 128 // 64 clips de video + 64 samples de audio

// InstrumentBank.h
//
// Generaliza el viejo "SharedSample" (solo audio) a un InstrumentSource que
// puede tener audio, video, o ambos — pre-decodificados por completo a RAM
// al momento de cargar, igual de espíritu a como LoadSampleIntoBank() ya
// decodifica WAV/MP3/FLAC con miniaudio.
//
// POR QUÉ TODO SE PRE-DECODIFICA (nada de streaming en vivo):
// El requisito es que un Bicho empiece a sonar/mostrarse de forma
// INSTANTÁNEA al pasar por una celda pintada, sin ningún arranque en frío.
// Decodificar un .mpg con pl_mpeg en tiempo real, cuadro por cuadro, mientras
// el bicho ya está sonando, metería latencia variable justo en el momento
// más sensible. La única forma de garantizar "instantáneo" es que, para
// cuando el usuario pinta una celda con este instrumento, sus frames de
// video YA estén en un array contiguo en RAM, listos para copiarse a una
// textura con un simple memcpy/UpdateTexture.
//
// COSTO DE MEMORIA — IMPORTANTE LEER ANTES DE CARGAR VIDEOS LARGOS:
// Un video de 10s a 480x270 sin comprimir en RGB pesa
// 480*270*3 bytes/frame * 30fps * 10s ~= 116 MB. Por eso este banco impone
// un límite de duración por defecto (ver kMaxVideoSeconds) y por qué el
// LEEME del proyecto recomienda clips cortos, igual que ya recomendaba para
// los samples de audio.
//
// FORMATOS DE ARCHIVO DE VIDEO SOPORTADOS:
// pl_mpeg solo decodifica MPEG1 Video + MP2 Audio dentro de un contenedor
// MPEG-PS (.mpg/.mpeg). Sin embargo, LoadVideo() acepta CUALQUIER formato
// que FFmpeg pueda leer (.mp4, .mov, .webm, .avi, etc.) — si el archivo
// NO es ya .mpg/.mpeg, TranscodeToMpg() lo convierte automáticamente a
// una ruta temporal con ffmpeg antes de que pl_mpeg lo decodifique. La
// ruta temporal se borra una vez terminada la decodificación. El usuario
// nunca necesita hacer la conversión a mano.
//
// Requisito: el ejecutable `ffmpeg` debe estar disponible en el PATH del
// sistema. En la mayoría de Linux: sudo apt install ffmpeg. En macOS:
// brew install ffmpeg. En Windows: descargarlo de ffmpeg.org y agregarlo
// al PATH. Si ffmpeg no está disponible, LoadVideo() devuelve false con
// un mensaje de error claro, pero el resto del programa sigue
// funcionando (el slot queda vacío).

// -------------------------------------------------------------------------
// Transcodificación automática a .mpg (MPEG1 Video + MP2 Audio)
// -------------------------------------------------------------------------

// Lado mayor máximo (px) al que se reescala un video al cargarlo. Bajarlo
// comprime más (menos RAM por clip → caben más videos); solo afecta a los
// videos cargados DESPUÉS de cambiarlo. La UI lo expone como botón "RES".
static int g_transcodeMaxSide = 480;

// CON QUÉ se invoca ffmpeg. Por defecto el del PATH, que es lo que había
// siempre; se puede apuntar a una copia PROPIA (ruta absoluta) para no
// depender de que el usuario tenga uno instalado.
//
// Existe por el módulo de VCV Rack: ahí el usuario no es alguien que vaya a
// tocar su PATH, así que el plugin se descarga su ffmpeg junto al .so y apunta
// esto a esa copia. Ver vcv/src/Ffmpeg.hpp.
//
// Se guarda ya escapado para la shell (lleva sus comillas si hacen falta),
// porque el único sitio donde se usa es dentro de un system().
static std::string g_ffmpegBinary = "ffmpeg";

// Duración máxima aceptada por clip (por seguridad de RAM). Los archivos
// más largos NO se rechazan: la transcodificación corta a los primeros
// kMaxVideoSeconds (el editor de recorte sirve para afinar después).
static const double kMaxVideoSeconds = 30.0;

// Devuelve true si la extensión del archivo ya es .mpg o .mpeg (case-
// insensitive), en cuyo caso no hace falta convertir nada.
static bool IsMpgFile(const char* filePath) {
    std::string path(filePath);
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
    }
    return ext == ".mpg" || ext == ".mpeg";
}

// Genera una ruta de archivo temporal única con extensión .mpg.
// Usa mkstemp en POSIX (Linux/macOS) y GetTempPath/GetTempFileName en Windows.
static std::string MakeTempMpgPath() {
#if defined(_WIN32)
    char tempDir[MAX_PATH];
    char tempFile[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    GetTempFileNameA(tempDir, "pinguus_", 0, tempFile);
    // GetTempFileNameA crea el archivo con una extensión vacía; renombramos.
    std::string result(tempFile);
    result += ".mpg";
    return result;
#else
    char tmpl[] = "/tmp/pinguus_XXXXXX.mpg";
    int fd = mkstemps(tmpl, 4); // 4 = longitud de ".mpg"
    if (fd >= 0) close(fd);
    return std::string(tmpl);
#endif
}

// Convierte cualquier video que FFmpeg pueda leer al formato MPEG1 Video
// + MP2 Audio que pl_mpeg necesita. Escribe el resultado en outTempPath
// (ruta temporal generada por MakeTempMpgPath) y devuelve true si la
// conversión tuvo éxito.
//
// La calidad del video transcodeado se fija en 2500 kbps (suficiente
// para clips cortos de YTPMV a resoluciones típicas 480x270 – 720x405)
// con framerate máximo de 30fps. Ajustar estos valores cambia el tamaño
// del archivo temporal y el tiempo de transcodificación, pero NO cambia
// el consumo de RAM final (que depende solo de resolución × fotogramas ×
// 3 bytes/pixel, sin importar la tasa de bits del .mpg intermedio).
// MPEG-1 sólo admite OCHO tasas de fotogramas. No es una recomendación: está en
// el formato, son 3 bits en la cabecera de secuencia, y pedirle otra a ffmpeg no
// se aproxima — FALLA, con "MPEG-1/2 does not support 15/1 fps", sin escribir un
// solo byte.
//
// Costó un fallo real: el módulo de VCV Rack pedía 15 fps para ahorrar RAM, así
// que TODA carga de mp4 fallaba... y como este camino sólo sabe devolver
// true/false, el usuario recibía "necesitas ffmpeg en el PATH" aunque lo tuviera
// instalado y funcionando. El mensaje mandaba a arreglar lo que no estaba roto.
//
// Por eso el ajuste se hace AQUÍ y no en quien llama: cualquiera puede pedir la
// tasa que quiera y sale una válida, en vez de un archivo vacío.
static int SnapToMpeg1Fps(float requested) {
    static const int kLegal[] = {24, 25, 30, 50, 60};
    const int n = (int)(sizeof(kLegal) / sizeof(kLegal[0]));
    if (requested <= (float)kLegal[0]) return kLegal[0];   // 24 es el mínimo que existe
    int best = kLegal[0], bestDiff = -1;
    for (int i = 0; i < n; i++) {
        const int d = (int)(requested > kLegal[i] ? requested - kLegal[i] : kLegal[i] - requested);
        if (bestDiff < 0 || d < bestDiff) { bestDiff = d; best = kLegal[i]; }
    }
    return best;
}

static bool TranscodeToMpg(const char* inputPath, const std::string& outTempPath, float maxFps) {
    // Construimos el comando de ffmpeg como string. En producción esto
    // podría reemplazarse por una llamada directa a libavcodec para evitar
    // la dependencia del ejecutable de sistema, pero para el caso de uso
    // real (cargar unos pocos clips al arrancar el programa) la penalización
    // de una llamada a un subproceso es completamente aceptable.
    const int fps = SnapToMpeg1Fps(maxFps);
    std::string cmd;

    // Filtro de escala: limita el lado mayor a g_transcodeMaxSide px
    // (manteniendo aspecto y dimensiones pares, requisito de mpeg1video).
    // Esto controla la RAM del pre-decodificado (que crece con resolución x
    // fotogramas, no con el bitrate) y hace que clips 1080p no exploten la
    // memoria. La UI puede bajar el valor (RES 480/360/240) para meter más
    // videos en máquinas con poca RAM.
    char scaleFilter[160];
    snprintf(scaleFilter, sizeof(scaleFilter),
             "scale=w='min(iw,%d)':h='min(ih,%d)':"
             "force_original_aspect_ratio=decrease:force_divisible_by=2",
             g_transcodeMaxSide, g_transcodeMaxSide);

#if defined(_WIN32)
    // En Windows escapamos las rutas con comillas dobles. Si la ruta tiene
    // comillas incrustadas (caso extremamente raro), esta construcción simple
    // no es segura, pero para rutas de archivo normales funciona correctamente.
    cmd = g_ffmpegBinary + " -y -i \"" + std::string(inputPath) + "\""
          " -t " + std::to_string((int)kMaxVideoSeconds) +
          " -vf \"" + scaleFilter + "\""
          " -c:v mpeg1video -q:v 3"
          " -r " + std::to_string(fps) +
          " -c:a mp2 -ar 44100 -ac 2 -b:a 224k"
          " -muxpreload 0 -muxdelay 0 -f mpeg"
          " \"" + outTempPath + "\""
          " -loglevel quiet 2>&1";
#else
    // POSIX: usamos comillas simples + escapes para rutas con espacios.
    // Reemplaza las comillas simples en la ruta por '\''.
    auto escapeForShell = [](const std::string& s) {
        std::string result;
        result.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '\'') result += "'\\''";
            else result += c;
        }
        return "'" + result + "'";
    };

    cmd = g_ffmpegBinary + " -y -i " + escapeForShell(inputPath)
          + " -t " + std::to_string((int)kMaxVideoSeconds)
          + " -vf " + escapeForShell(scaleFilter)
          + " -c:v mpeg1video -q:v 3"
          + " -r " + std::to_string(fps)
          + " -c:a mp2 -ar 44100 -ac 2 -b:a 224k"
          + " -muxpreload 0 -muxdelay 0 -f mpeg "
          + escapeForShell(outTempPath)
          + " -loglevel quiet 2>&1";
#endif

    printf("InstrumentBank: transcodificando '%s' -> .mpg temporal...\n", inputPath);
    int ret = system(cmd.c_str());
    if (ret != 0) {
        printf("InstrumentBank: ffmpeg no disponible o falló (código %d). "
               "Asegurate de tener ffmpeg instalado y en el PATH.\n"
               "  Comando: %s\n", ret, cmd.c_str());
        return false;
    }

    // Verificar que el archivo temporal realmente se creó y tiene contenido.
    FILE* f = fopen(outTempPath.c_str(), "rb");
    if (f == nullptr) {
        printf("InstrumentBank: ffmpeg no creó el archivo temporal: %s\n",
               outTempPath.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    if (size < 1024) {
        printf("InstrumentBank: el archivo temporal está vacío o dañado: %s\n",
               outTempPath.c_str());
        remove(outTempPath.c_str());
        return false;
    }

    printf("InstrumentBank: transcodificación OK (%.1f KB)\n", size / 1024.0f);
    return true;
}

struct SharedSample {
    float* pPCM = nullptr;
    unsigned long long totalFrames = 0;
};

// Un frame de video ya decodificado a RGB24 plano, listo para subir a una
// textura. width/height se repiten desde el InstrumentSource por comodidad,
// pero el dato real es solo el buffer de bytes.
struct VideoFrameRGB {
    uint8_t* rgb = nullptr; // width * height * 3 bytes
};

struct InstrumentSource {
    // --- Audio (opcional) ---
    SharedSample audio;

    // --- Video (opcional) ---
    std::vector<VideoFrameRGB> videoFrames;
    int videoWidth = 0;
    int videoHeight = 0;
    int videoChannels = 3;       // 3 = RGB (video), 4 = RGBA (image/GIF with alpha)
    double videoFramerate = 0.0; // frames del clip ORIGINAL por segundo

    // NON-DESTRUCTIVE TRIM: the full media stays in RAM; playback just uses
    // this range. 0 end = "to the end". So you can re-open the editor and
    // widen/move the trim without re-importing the file.
    int vTrimStart = 0, vTrimEnd = 0;                 // video, in frames
    unsigned long long aTrimStart = 0, aTrimEnd = 0;  // audio, in samples

    int videoTrimStart() const { return vTrimStart; }
    int videoTrimEnd() const { return vTrimEnd > 0 ? vTrimEnd : (int)videoFrames.size(); }
    unsigned long long audioTrimStart() const { return aTrimStart; }
    unsigned long long audioTrimEnd() const { return aTrimEnd > 0 ? aTrimEnd : audio.totalFrames; }

    bool hasAudio() const { return audio.pPCM != nullptr && audio.totalFrames > 0; }
    bool hasVideo() const { return !videoFrames.empty(); }
};

// Libera toda la memoria de un slot (audio + cada frame de video) sin dejar
// el slot en un estado parcialmente liberado.
inline void FreeInstrumentSource(InstrumentSource& src) {
    if (src.audio.pPCM != nullptr) {
        free(src.audio.pPCM);
        src.audio.pPCM = nullptr;
        src.audio.totalFrames = 0;
    }
    for (auto& frame : src.videoFrames) {
        free(frame.rgb);
    }
    src.videoFrames.clear();
    src.videoWidth = 0;
    src.videoHeight = 0;
    src.videoFramerate = 0.0;
}

// Decodifica un .wav/.mp3/.flac/.ogg por completo a un SharedSample, igual
// que la función LoadSampleIntoBank() original de main.cpp (se deja aquí
// para que InstrumentBank sea autocontenido; main.cpp puede seguir llamando
// a su propia versión si prefiere, son equivalentes).
bool DecodeAudioFile(const char* filePath, SharedSample* outSample) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 44100);
    ma_decoder decoder;

    if (ma_decoder_init_file(filePath, &config, &decoder) != MA_SUCCESS) {
        printf("InstrumentBank: no se pudo abrir el audio: %s\n", filePath);
        return false;
    }

    ma_uint64 totalFrames;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) != MA_SUCCESS || totalFrames == 0) {
        printf("InstrumentBank: no se pudo determinar la duracion del audio: %s\n", filePath);
        ma_decoder_uninit(&decoder);
        return false;
    }

    float* pcm = (float*)malloc((size_t)totalFrames * sizeof(float));
    if (pcm == nullptr) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    ma_uint64 framesRead = 0;
    ma_decoder_read_pcm_frames(&decoder, pcm, totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);

    outSample->pPCM = pcm;
    outSample->totalFrames = framesRead;
    return true;
}

// Decodifica un archivo de video por completo a memoria:
// todos los frames de video como RGB24 planos, y todo el audio como PCM
// float intercalado-a-mono (promedio de canales, igual de formato que
// SharedSample espera en el resto del motor).
//
// Acepta CUALQUIER formato que FFmpeg pueda leer (.mp4, .mov, .webm,
// .avi, etc.). Si el archivo no es ya .mpg/.mpeg, lo convierte
// automáticamente con `ffmpeg` a una ruta temporal antes de
// decodificarlo con pl_mpeg. El archivo temporal se borra al terminar.
//
// loadAudio: si es false, se ignora la pista de audio del video aunque
// exista (para el caso "solo video, mudo" del diseño).
//
// maxFps: framerate máximo de transcodificación. Videos con framerate
// original mayor se submuestrean. 30.0f es un buen valor por defecto.
bool DecodeVideoFile(const char* filePath, InstrumentSource* outSource,
                     bool loadAudio, float maxFps = 30.0f) {
    // Determinar si necesitamos transcodificar primero.
    std::string mpgPath;
    bool didTranscode = false;

    if (IsMpgFile(filePath)) {
        // Ya es .mpg/.mpeg — pl_mpeg puede leerlo directamente.
        mpgPath = std::string(filePath);
    } else {
        // Cualquier otro formato: transcodificar con ffmpeg a .mpg temporal.
        mpgPath = MakeTempMpgPath();
        if (!TranscodeToMpg(filePath, mpgPath, maxFps)) {
            return false; // TranscodeToMpg ya imprimió el error
        }
        didTranscode = true;
    }

    plm_t* plm = plm_create_with_filename(mpgPath.c_str());

    // Si transcodificamos, borramos el temporal ahora que pl_mpeg ya lo
    // abrió (tiene un file handle propio; borrar en Linux/macOS no
    // invalida el handle hasta que se cierre). En Windows, hay que
    // esperar a que pl_mpeg cierre el archivo primero — usamos un flag
    // para borrarlo después de plm_destroy().
    bool deleteTempAfterDestroy = false;
    if (didTranscode) {
#if defined(_WIN32)
        deleteTempAfterDestroy = true; // Windows no permite borrar un archivo abierto
#else
        remove(mpgPath.c_str()); // POSIX: el inode sigue vivo mientras haya handles
#endif
    }

    if (plm == nullptr) {
        printf("InstrumentBank: no se pudo abrir el video: %s\n", mpgPath.c_str());
        if (didTranscode) remove(mpgPath.c_str());
        return false;
    }

    if (!plm_has_headers(plm)) {
        printf("InstrumentBank: archivo sin cabeceras MPEG válidas: %s\n", mpgPath.c_str());
        plm_destroy(plm);
        if (didTranscode) remove(mpgPath.c_str());
        return false;
    }

    double duration = plm_get_duration(plm);
    // +1.0 de tolerancia: el corte a -t segundos de ffmpeg puede pasarse una
    // fracción por los límites de GOP. Solo los .mpg directos (sin
    // transcodificar) pueden llegar aquí realmente largos.
    if (duration > kMaxVideoSeconds + 1.0) {
        printf(
            "InstrumentBank: '%s' dura %.1fs, por encima del límite de %.1fs "
            "(kMaxVideoSeconds). Usa un clip más corto o sube el límite a "
            "propósito si tienes RAM de sobra.\n",
            filePath, duration, kMaxVideoSeconds
        );
        plm_destroy(plm);
        if (didTranscode) remove(mpgPath.c_str());
        return false;
    }

    plm_set_video_enabled(plm, TRUE);
    plm_set_audio_enabled(plm, loadAudio ? TRUE : FALSE);
    if (loadAudio) {
        plm_set_audio_stream(plm, 0);
    }

    outSource->videoChannels = 3; // pl_mpeg decodes to RGB (no alpha)
    outSource->videoWidth = plm_get_width(plm);
    outSource->videoHeight = plm_get_height(plm);
    outSource->videoFramerate = plm_get_framerate(plm);


    if (outSource->videoWidth <= 0 || outSource->videoHeight <= 0) {
        printf("InstrumentBank: dimensiones de video inválidas en: %s\n", filePath);
        plm_destroy(plm);
        return false;
    }

    const size_t frameBytes = (size_t)outSource->videoWidth * (size_t)outSource->videoHeight * 3;

    // Decodifica frame por frame hasta agotar el archivo. Esto SÍ recorre
    // todo el video de una sola pasada, pero ocurre una sola vez durante la
    // carga — nunca durante la reproducción en tiempo real.
    std::vector<float> pcmAccumulator;
    if (loadAudio) {
        pcmAccumulator.reserve((size_t)(duration * 44100.0) + PLM_AUDIO_SAMPLES_PER_FRAME);
    }

    while (true) {
        plm_frame_t* frame = plm_decode_video(plm);
        if (frame == nullptr) {
            // No hay más frames de video; si todavía queda audio pendiente,
            // lo seguimos drenando antes de salir del todo.
            if (!loadAudio) break;
            plm_samples_t* samples = plm_decode_audio(plm);
            if (samples == nullptr) break;
            for (int i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME; i++) {
                float l = samples->interleaved[i * 2];
                float r = samples->interleaved[i * 2 + 1];
                pcmAccumulator.push_back((l + r) * 0.5f);
            }
            continue;
        }

        uint8_t* rgb = (uint8_t*)malloc(frameBytes);
        if (rgb == nullptr) {
            printf("InstrumentBank: sin memoria decodificando frames de: %s\n", filePath);
            FreeInstrumentSource(*outSource);
            plm_destroy(plm);
            return false;
        }
        plm_frame_to_rgb(frame, rgb, outSource->videoWidth * 3);
        outSource->videoFrames.push_back({rgb});

        if (loadAudio) {
            // Drena todo el audio que ya esté disponible para este punto del
            // demuxer, para no acumular un desfasaje entre pistas.
            plm_samples_t* samples;
            while ((samples = plm_decode_audio(plm)) != nullptr) {
                for (int i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME; i++) {
                    float l = samples->interleaved[i * 2];
                    float r = samples->interleaved[i * 2 + 1];
                    pcmAccumulator.push_back((l + r) * 0.5f);
                }
            }
        }
    }

    plm_destroy(plm);

    // En Windows esperamos a cerrar el handle antes de borrar el temporal.
    if (deleteTempAfterDestroy) {
        remove(mpgPath.c_str());
    }

    if (outSource->videoFrames.empty()) {
        printf("InstrumentBank: '%s' no produjo ningún frame de video decodificable.\n", filePath);
        return false;
    }

    if (loadAudio && !pcmAccumulator.empty()) {
        float* pcm = (float*)malloc(pcmAccumulator.size() * sizeof(float));
        if (pcm != nullptr) {
            memcpy(pcm, pcmAccumulator.data(), pcmAccumulator.size() * sizeof(float));
            outSource->audio.pPCM = pcm;
            outSource->audio.totalFrames = pcmAccumulator.size();
        }
    }

    return true;
}

class InstrumentBank {
public:
    // A prueba de índices fuera de rango (los "slots virtuales" de modelos 3D
    // usan ids >= 1000): devuelven una fuente vacía compartida en vez de leer
    // fuera del arreglo. La fuente vacía no tiene audio ni video, así que el
    // motor la ignora (no suena) sin caso especial.
    InstrumentSource& at(int slotId) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return emptySource;
        return sources[slotId];
    }

    const InstrumentSource& at(int slotId) const {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return emptySource;
        return sources[slotId];
    }

    // Reemplaza el contenido de un slot por un instrumento de SOLO AUDIO,
    // liberando lo que hubiera antes. Pensado para kicks, snares, o cualquier
    // sample tradicional que el usuario quiera usar sin video asociado.
    bool LoadAudioOnly(int slotId, const char* audioPath) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;

        SharedSample newAudio;
        if (!DecodeAudioFile(audioPath, &newAudio)) return false;

        FreeInstrumentSource(sources[slotId]);
        sources[slotId].audio = newAudio;
        return true;
    }

    // Carga PCM YA DECODIFICADO (mono, float, 44100 Hz) en el audio de un slot.
    //
    // Es el equivalente de LoadVisualFrames para el sonido, y existe porque en
    // Android no hay ffmpeg ni ma_decoder que valga para AAC: el audio sale de
    // AMediaCodec como muestras en memoria, y sin esta puerta habría que
    // escribirlo a un .wav temporal sólo para volver a leerlo.
    //
    // keepVideo=true deja intactos los fotogramas del slot, para cargar por
    // separado la imagen y el sonido de un mismo clip.
    bool LoadAudioFromPCM(int slotId, const float* pcm, unsigned long long frames,
                          bool keepVideo = false) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;
        if (pcm == nullptr || frames == 0) return false;

        float* copy = (float*)malloc((size_t)frames * sizeof(float));
        if (copy == nullptr) return false;
        memcpy(copy, pcm, (size_t)frames * sizeof(float));

        InstrumentSource& s = sources[slotId];
        if (keepVideo) {
            free(s.audio.pPCM);          // sólo el audio; los fotogramas se quedan
        } else {
            FreeInstrumentSource(s);
        }
        s.audio.pPCM = copy;
        s.audio.totalFrames = frames;
        s.aTrimStart = 0;
        s.aTrimEnd = 0;
        return true;
    }

    // Reemplaza el contenido de un slot por un instrumento de VIDEO,
    // Reemplaza el contenido de un slot por un instrumento de VIDEO,
    // opcionalmente usando también su pista de audio como sample.
    //
    // Acepta CUALQUIER formato de video que FFmpeg pueda leer:
    //   .mp4, .mov, .webm, .avi, .mkv, .mpg, .mpeg, etc.
    // Si el archivo no es ya .mpg/.mpeg, se convierte automáticamente
    // con `ffmpeg` a una ruta temporal. Requiere que el ejecutable
    // `ffmpeg` esté disponible en el PATH del sistema.
    //
    // maxFps: framerate máximo del video pre-decodificado. Videos con
    // framerate original mayor se submuestrean a este valor durante la
    // transcodificación. 30.0f es el valor por defecto razonable.
    bool LoadVideo(int slotId, const char* videoPath,
                   bool useVideoAudio, float maxFps = 30.0f) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;

        InstrumentSource newSource;
        if (!DecodeVideoFile(videoPath, &newSource, useVideoAudio, maxFps)) return false;

        FreeInstrumentSource(sources[slotId]);
        sources[slotId] = newSource;
        return true;
    }

    // Combina un slot ya cargado por video con un audio externo distinto al
    // de la pista del propio clip (caso: "quiero este video pero con ESTE
    // kick"). Reemplaza solo la parte de audio del slot, deja el video tal
    // cual estaba.
    bool OverrideAudio(int slotId, const char* audioPath) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;

        SharedSample newAudio;
        if (!DecodeAudioFile(audioPath, &newAudio)) return false;

        InstrumentSource& src = sources[slotId];
        if (src.audio.pPCM != nullptr) free(src.audio.pPCM);
        src.audio = newAudio;
        return true;
    }

    // NON-DESTRUCTIVE trim: keep all frames/samples, just set the playback
    // range. Safe to call on the audio thread's data (it only reads); no
    // realloc, so no need to stop the device. Re-openable/adjustable.
    void SetVideoTrim(int slotId, int startFrame, int endFrame) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return;
        InstrumentSource& src = sources[slotId];
        int total = (int)src.videoFrames.size();
        if (startFrame < 0) startFrame = 0;
        if (endFrame <= 0 || endFrame > total) endFrame = total;
        if (endFrame <= startFrame) return;
        src.vTrimStart = startFrame;
        src.vTrimEnd = endFrame;
        // Mirror the range onto the audio track so both stay in sync.
        if (src.videoFramerate > 0.0 && src.audio.totalFrames > 0) {
            src.aTrimStart = (unsigned long long)(startFrame / src.videoFramerate * 44100.0);
            src.aTrimEnd = (unsigned long long)(endFrame / src.videoFramerate * 44100.0);
            if (src.aTrimEnd > src.audio.totalFrames) src.aTrimEnd = src.audio.totalFrames;
        }
    }

    void SetAudioTrim(int slotId, unsigned long long a0, unsigned long long a1) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return;
        InstrumentSource& src = sources[slotId];
        if (a1 == 0 || a1 > src.audio.totalFrames) a1 = src.audio.totalFrames;
        if (a1 <= a0) return;
        src.aTrimStart = a0;
        src.aTrimEnd = a1;
    }

    void ClearTrim(int slotId) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return;
        InstrumentSource& s = sources[slotId];
        s.vTrimStart = s.vTrimEnd = 0;
        s.aTrimStart = s.aTrimEnd = 0;
    }

    // (Legacy DESTRUCTIVE trim — no longer used by the UI, kept for reference.)
    bool TrimVideo(int slotId, int startFrame, int endFrame) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;
        InstrumentSource& src = sources[slotId];
        if (!src.hasVideo()) return false;

        int total = (int)src.videoFrames.size();
        if (startFrame < 0) startFrame = 0;
        if (endFrame > total) endFrame = total;
        if (endFrame - startFrame < 1) return false;
        if (startFrame == 0 && endFrame == total) return true; // nada que recortar

        for (int i = 0; i < startFrame; i++) free(src.videoFrames[i].rgb);
        for (int i = endFrame; i < total; i++) free(src.videoFrames[i].rgb);
        src.videoFrames.erase(src.videoFrames.begin() + endFrame, src.videoFrames.end());
        src.videoFrames.erase(src.videoFrames.begin(), src.videoFrames.begin() + startFrame);

        // Audio: mismo rango pero en muestras (el PCM del banco está a 44100
        // Hz mono, ver DecodeVideoFile/DecodeAudioFile).
        if (src.audio.pPCM != nullptr && src.videoFramerate > 0.0) {
            unsigned long long a0 = (unsigned long long)(startFrame / src.videoFramerate * 44100.0);
            unsigned long long a1 = (unsigned long long)(endFrame / src.videoFramerate * 44100.0);
            if (a0 > src.audio.totalFrames) a0 = src.audio.totalFrames;
            if (a1 > src.audio.totalFrames) a1 = src.audio.totalFrames;

            if (a1 > a0) {
                unsigned long long newLen = a1 - a0;
                float* pcm = (float*)malloc((size_t)newLen * sizeof(float));
                if (pcm != nullptr) {
                    memcpy(pcm, src.audio.pPCM + a0, (size_t)newLen * sizeof(float));
                    free(src.audio.pPCM);
                    src.audio.pPCM = pcm;
                    src.audio.totalFrames = newLen;
                }
            } else {
                free(src.audio.pPCM);
                src.audio.pPCM = nullptr;
                src.audio.totalFrames = 0;
            }
        }
        return true;
    }

    // Loads pre-decoded RGB24 frames (from a still image or an animated GIF,
    // decoded by the caller via raylib) into a slot as VISUAL content, with
    // no audio. A still image is just one frame; a GIF is many. This lets
    // clips be images/GIFs, a classic YTPMV move (cut the video to a still
    // or a looping GIF). Frames are copied into the bank's own buffers.
    bool LoadVisualFrames(int slot, const uint8_t* rgbFrames, int frameCount,
                          int w, int h, double framerate, int channels = 3) {
        if (slot < 0 || slot >= MAX_INSTRUMENTS) return false;
        if (rgbFrames == nullptr || frameCount < 1 || w <= 0 || h <= 0) return false;
        if (channels != 3 && channels != 4) channels = 3;

        FreeInstrumentSource(sources[slot]);
        const size_t frameBytes = (size_t)w * (size_t)h * channels;
        for (int i = 0; i < frameCount; i++) {
            uint8_t* rgb = (uint8_t*)malloc(frameBytes);
            if (rgb == nullptr) { FreeInstrumentSource(sources[slot]); return false; }
            memcpy(rgb, rgbFrames + (size_t)i * frameBytes, frameBytes);
            sources[slot].videoFrames.push_back({rgb});
        }
        sources[slot].videoWidth = w;
        sources[slot].videoHeight = h;
        sources[slot].videoChannels = channels;
        sources[slot].videoFramerate = framerate > 0.0 ? framerate : 12.0;
        return true;
    }

    // Replaces ONLY the visual frames of a slot (keeps its audio). Used to
    // swap a clip's video for an image/GIF while keeping the original sound,
    // or to give an audio sample a still/GIF visual. Classic YTPMV move.
    bool ReplaceVisualFrames(int slot, const uint8_t* rgbFrames, int frameCount,
                             int w, int h, double framerate, int channels = 3) {
        if (slot < 0 || slot >= MAX_INSTRUMENTS) return false;
        if (rgbFrames == nullptr || frameCount < 1 || w <= 0 || h <= 0) return false;
        if (channels != 3 && channels != 4) channels = 3;

        InstrumentSource& s = sources[slot];
        for (auto& f : s.videoFrames) free(f.rgb);
        s.videoFrames.clear();

        const size_t frameBytes = (size_t)w * (size_t)h * channels;
        for (int i = 0; i < frameCount; i++) {
            uint8_t* rgb = (uint8_t*)malloc(frameBytes);
            if (rgb == nullptr) { for (auto& f : s.videoFrames) free(f.rgb); s.videoFrames.clear(); return false; }
            memcpy(rgb, rgbFrames + (size_t)i * frameBytes, frameBytes);
            s.videoFrames.push_back({rgb});
        }
        s.videoWidth = w;
        s.videoHeight = h;
        s.videoChannels = channels;
        s.videoFramerate = framerate > 0.0 ? framerate : 12.0;
        return true; // audio left untouched
    }

    // Drops a slot's visual frames, keeping its audio (undo a visual override
    // on an audio-only sample).
    void ClearVisual(int slot) {
        if (slot < 0 || slot >= MAX_INSTRUMENTS) return;
        InstrumentSource& s = sources[slot];
        for (auto& f : s.videoFrames) free(f.rgb);
        s.videoFrames.clear();
        s.videoWidth = 0;
        s.videoHeight = 0;
        s.videoFramerate = 0.0;
    }

    // Recorta el AUDIO de un slot al rango [a0, a1) en muestras PCM (44100
    // Hz mono). Igual de destructivo que TrimVideo y con la misma regla:
    // llamar con el dispositivo de audio detenido.
    bool TrimAudio(int slotId, unsigned long long a0, unsigned long long a1) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return false;
        SharedSample& audio = sources[slotId].audio;
        if (audio.pPCM == nullptr || audio.totalFrames == 0) return false;

        if (a1 > audio.totalFrames) a1 = audio.totalFrames;
        if (a0 >= a1) return false;
        if (a0 == 0 && a1 == audio.totalFrames) return true;

        unsigned long long newLen = a1 - a0;
        float* pcm = (float*)malloc((size_t)newLen * sizeof(float));
        if (pcm == nullptr) return false;
        memcpy(pcm, audio.pPCM + a0, (size_t)newLen * sizeof(float));
        free(audio.pPCM);
        audio.pPCM = pcm;
        audio.totalFrames = newLen;
        return true;
    }

    void Clear(int slotId) {
        if (slotId < 0 || slotId >= MAX_INSTRUMENTS) return;
        FreeInstrumentSource(sources[slotId]);
    }

    ~InstrumentBank() {
        for (int i = 0; i < MAX_INSTRUMENTS; i++) {
            FreeInstrumentSource(sources[i]);
        }
    }

private:
    InstrumentSource sources[MAX_INSTRUMENTS];
    InstrumentSource emptySource; // devuelta para ids fuera de rango (modelos 3D)
};
