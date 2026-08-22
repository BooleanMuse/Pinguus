#pragma once
// NtscFX.h
//
// Efecto de VÍDEO ANALÓGICO (NTSC / VHS) aplicado a la MEZCLA FINAL: al
// collage entero, después de componer todas las capas y justo antes de que los
// píxeles salgan hacia ffmpeg (grabación) o hacia la ventana LIVE. Opcional:
// apagado no cuesta ni un paso de render.
//
// POR QUÉ NO SE ENLAZA ntsc-rs DIRECTAMENTE
//   ntsc-rs (https://github.com/valadaptive/ntsc-rs) es la referencia de este
//   tipo de efecto y su licencia es permisiva, así que el problema no es
//   legal: es de construcción. Es una biblioteca de Rust, y meterla aquí
//   obligaría a que CUALQUIERA que compile Pinguus tenga la cadena de Rust
//   instalada, más una capa de C, más compilación cruzada a Windows y a
//   arm64 — cuando ahora mismo basta con un g++ y el script de un clic. Ese
//   es exactamente el tipo de dependencia que este proyecto evita.
//
//   Así que el efecto está reimplementado aquí en GLSL, en un solo paso sobre
//   el fotograma final, con los mismos ingredientes que dan ese aspecto:
//   sangrado de color, retardo de croma, ruido de fase, campanilleo, ruido de
//   compuesto/luma/croma, nieve, ondulación de borde, salto de cabezal,
//   pérdida de croma y líneas de barrido.
//
// LOS PRESETS .json DE ntsc-rs SÍ SE LEEN
//   Su formato es un objeto PLANO de "clave": valor con un "version": 1, así
//   que se puede leer sin arrastrar una biblioteca de JSON. LoadNtscPreset()
//   traduce las claves que tienen equivalente aquí y CUENTA las que no, y esa
//   cuenta se le enseña al usuario. Que quede claro: es una TRADUCCIÓN, no una
//   emulación exacta — son dos implementaciones distintas, así que el ajuste
//   queda en la misma familia de aspecto, no idéntico píxel a píxel.

#include "raylib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Parámetros
//
// Todos van de 0 a 1 para que la interfaz sea una fila de deslizadores iguales
// y para que un preset traducido no pueda meter un valor que reviente el
// shader. Los valores por defecto son un VHS creíble, no el efecto al máximo.
// ---------------------------------------------------------------------------
struct NtscFX {
    bool  enabled = false;
    float noise = 0.10f;          // ruido de la señal compuesta
    float lumaNoise = 0.05f;      // ruido en el brillo
    float chromaNoise = 0.14f;    // ruido en el color
    float snow = 0.02f;           // "nieve": puntos blancos sueltos
    float chromaDelay = 0.35f;    // el color va corrido respecto al brillo
    float phaseNoise = 0.15f;     // el tono del color baila por líneas
    float ringing = 0.35f;        // campanilleo/eco en los bordes verticales
    float sharpen = 0.30f;        // realce de la señal compuesta
    float edgeWave = 0.18f;       // ondulación del borde (cinta floja)
    float edgeWaveSpeed = 0.50f;
    float headSwitch = 0.30f;     // franja rota de abajo (salto de cabezal)
    float chromaLoss = 0.05f;     // líneas que pierden el color del todo
    float scanlines = 0.22f;      // líneas de barrido
    float tapeBlur = 0.35f;       // ancho de banda: SP nítido, EP emborronado
    float seed = 1.0f;

    // Deja los deslizadores en un VHS gastado (el botón "VHS" de la interfaz).
    void presetVHS() {
        noise = 0.18f; lumaNoise = 0.08f; chromaNoise = 0.22f; snow = 0.04f;
        chromaDelay = 0.45f; phaseNoise = 0.22f; ringing = 0.40f; sharpen = 0.25f;
        edgeWave = 0.30f; edgeWaveSpeed = 0.6f; headSwitch = 0.45f; chromaLoss = 0.10f;
        scanlines = 0.20f; tapeBlur = 0.55f;
    }
    // Una emisión NTSC limpia: sangrado de color y campanilleo, poco ruido.
    void presetBroadcast() {
        noise = 0.05f; lumaNoise = 0.03f; chromaNoise = 0.08f; snow = 0.0f;
        chromaDelay = 0.30f; phaseNoise = 0.08f; ringing = 0.45f; sharpen = 0.40f;
        edgeWave = 0.0f; edgeWaveSpeed = 0.5f; headSwitch = 0.0f; chromaLoss = 0.0f;
        scanlines = 0.28f; tapeBlur = 0.15f;
    }
    // Cinta destrozada: para cuando lo que se quiere es que se note.
    void presetRuined() {
        noise = 0.45f; lumaNoise = 0.25f; chromaNoise = 0.50f; snow = 0.18f;
        chromaDelay = 0.70f; phaseNoise = 0.55f; ringing = 0.55f; sharpen = 0.20f;
        edgeWave = 0.65f; edgeWaveSpeed = 1.0f; headSwitch = 0.75f; chromaLoss = 0.35f;
        scanlines = 0.30f; tapeBlur = 0.85f;
    }
};

// ---------------------------------------------------------------------------
// El shader
//
// Un solo paso sobre el fotograma ya compuesto. Trabaja en YIQ (el espacio de
// color que usaba la tele NTSC de verdad) porque ahí es donde estos defectos
// se pueden imitar de forma barata: el brillo y el color van por caminos
// separados, que es precisamente lo que hacía que el color se corriera y
// perdiera nitidez mientras el brillo aguantaba.
// ---------------------------------------------------------------------------
static const char* kNtscFragShader = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  resolution;
uniform float time;
uniform float seed;
uniform float pNoise;
uniform float pLumaNoise;
uniform float pChromaNoise;
uniform float pSnow;
uniform float pChromaDelay;
uniform float pPhaseNoise;
uniform float pRinging;
uniform float pSharpen;
uniform float pEdgeWave;
uniform float pEdgeWaveSpeed;
uniform float pHeadSwitch;
uniform float pChromaLoss;
uniform float pScanlines;
uniform float pTapeBlur;

float hash11(float n) { return fract(sin(n * 12.9898 + seed * 78.233) * 43758.5453); }
float hash21(vec2 p)  { return fract(sin(dot(p, vec2(12.9898, 78.233)) + seed) * 43758.5453); }

vec3 rgb2yiq(vec3 c) {
    return vec3(dot(c, vec3(0.299, 0.587, 0.114)),
                dot(c, vec3(0.596, -0.274, -0.322)),
                dot(c, vec3(0.211, -0.523, 0.312)));
}
vec3 yiq2rgb(vec3 c) {
    return vec3(c.x + 0.956 * c.y + 0.621 * c.z,
                c.x - 0.272 * c.y - 0.647 * c.z,
                c.x - 1.106 * c.y + 1.703 * c.z);
}

void main() {
    vec2 texel = 1.0 / resolution;
    float line = floor(fragTexCoord.y * resolution.y);

    // ---- Desplazamiento horizontal de la línea ----
    // Ondulación de borde: la cinta no avanza perfectamente recta, así que
    // cada línea entra un poco corrida. Va con el tiempo para que tiemble.
    float wobble = (sin(line * 0.31 + time * 6.0 * pEdgeWaveSpeed) * 0.6 +
                    sin(line * 1.73 + time * 2.3 * pEdgeWaveSpeed) * 0.4);
    float off = wobble * pEdgeWave * 0.012;

    // Salto de cabezal: la franja rota de abajo del todo de una cinta VHS.
    // Sólo afecta a las últimas líneas, y cuanto más abajo, más rota.
    float hsBand = pHeadSwitch * 0.06;
    if (hsBand > 0.0 && fragTexCoord.y > 1.0 - hsBand) {
        float t = (fragTexCoord.y - (1.0 - hsBand)) / max(hsBand, 1e-5);
        off += t * t * pHeadSwitch * 0.16 * (0.6 + 0.8 * hash11(line + floor(time * 12.0)));
    }

    vec2 uv = vec2(fragTexCoord.x + off, fragTexCoord.y);

    // ---- Luma: desenfoque horizontal segun el "ancho de banda" de la cinta ----
    // Nueve tomas fijas: el bucle no puede depender de un uniform en GL ES, y
    // aqui tampoco hace falta — lo que cambia con pTapeBlur es la SEPARACION
    // entre tomas, no cuantas hay.
    float lumaSpread = texel.x * (0.4 + pTapeBlur * 5.0);
    float y = 0.0;
    float wsum = 0.0;
    for (int i = -4; i <= 4; i++) {
        float w = 1.0 - abs(float(i)) / 5.0;
        y += rgb2yiq(texture(texture0, vec2(uv.x + float(i) * lumaSpread, uv.y)).rgb).x * w;
        wsum += w;
    }
    y /= wsum;

    // Campanilleo: el eco que deja un filtro brusco en los cantos verticales.
    // Se hace sobre el brillo, que es donde se ve.
    if (pRinging > 0.0) {
        float ringSpread = texel.x * (2.0 + pRinging * 8.0);
        float a = rgb2yiq(texture(texture0, vec2(uv.x - ringSpread, uv.y)).rgb).x;
        float b = rgb2yiq(texture(texture0, vec2(uv.x + ringSpread, uv.y)).rgb).x;
        y += (y - (a + b) * 0.5) * pRinging * 1.6;
    }
    // Realce de la senal compuesta (el "preemphasis" que sube el contraste
    // local y de paso ensucia un poco los bordes).
    if (pSharpen > 0.0) {
        float c0 = rgb2yiq(texture(texture0, uv).rgb).x;
        y += (c0 - y) * pSharpen * 1.5;
    }

    // ---- Croma: mas emborronado que el brillo y ademas corrido ----
    // Esto es la clave del aspecto: en NTSC el color viaja con MUCHO menos
    // ancho de banda que el brillo, asi que se desparrama a los lados y llega
    // tarde. Por eso el rojo "se sale" de las cosas rojas.
    float chromaShift = pChromaDelay * texel.x * 9.0;
    float chromaSpread = texel.x * (2.0 + pChromaDelay * 10.0);
    vec2 iq = vec2(0.0);
    float cw = 0.0;
    for (int i = -4; i <= 4; i++) {
        float w = 1.0 - abs(float(i)) / 5.0;
        vec3 s = rgb2yiq(texture(texture0, vec2(uv.x - chromaShift + float(i) * chromaSpread, uv.y)).rgb);
        iq += s.yz * w;
        cw += w;
    }
    iq /= cw;

    // Ruido de fase: el tono del color baila de una linea a otra. Es un giro
    // del vector (I,Q), que es literalmente lo que le pasaba a la subportadora.
    if (pPhaseNoise > 0.0) {
        float ang = (hash11(line * 3.7 + floor(time * 30.0)) - 0.5) * pPhaseNoise * 2.4;
        float cs = cos(ang), sn = sin(ang);
        iq = vec2(iq.x * cs - iq.y * sn, iq.x * sn + iq.y * cs);
    }
    // Perdida de croma: lineas sueltas que se quedan en blanco y negro.
    if (pChromaLoss > 0.0) {
        if (hash11(line * 7.13 + floor(time * 20.0) * 1.7) < pChromaLoss * 0.5) iq = vec2(0.0);
    }

    // ---- Ruido ----
    vec2 np = vec2(fragTexCoord.x * resolution.x, line) + floor(time * 60.0);
    float n = hash21(np) - 0.5;
    y  += n * pNoise * 0.5;
    y  += (hash21(np * 1.7 + 11.0) - 0.5) * pLumaNoise * 0.6;
    iq += vec2(hash21(np * 2.3 + 5.0) - 0.5, hash21(np * 3.1 + 9.0) - 0.5) * pChromaNoise * 0.5;

    // Nieve: puntos blancos aislados, no un velo. De ahi el umbral tan alto.
    if (pSnow > 0.0) {
        float s = hash21(np * 0.7 + 31.0);
        if (s > 1.0 - pSnow * 0.09) y = mix(y, 1.0, 0.85);
    }

    // Lineas de barrido.
    if (pScanlines > 0.0) {
        float sl = mod(line, 2.0) < 1.0 ? 1.0 : (1.0 - pScanlines * 0.45);
        y *= sl;
    }

    vec3 rgb = yiq2rgb(vec3(y, iq));
    finalColor = vec4(clamp(rgb, 0.0, 1.0), 1.0) * colDiffuse * fragColor;
}
)GLSL";

// Shader cargado + dónde vive cada uniform (buscarlos por nombre en cada
// fotograma sería una llamada a OpenGL por parámetro y por fotograma).
struct NtscShader {
    Shader sh = {0};
    bool   ok = false;
    int    locRes = -1, locTime = -1, locSeed = -1;
    int    locNoise = -1, locLuma = -1, locChroma = -1, locSnow = -1;
    int    locDelay = -1, locPhase = -1, locRing = -1, locSharp = -1;
    int    locWave = -1, locWaveSpd = -1, locHead = -1, locLoss = -1;
    int    locScan = -1, locTape = -1;

    bool load() {
        if (ok) return true;
        sh = LoadShaderFromMemory(0, kNtscFragShader);
        if (sh.id == 0) return false;
        ok = true;
        locRes = GetShaderLocation(sh, "resolution");
        locTime = GetShaderLocation(sh, "time");
        locSeed = GetShaderLocation(sh, "seed");
        locNoise = GetShaderLocation(sh, "pNoise");
        locLuma = GetShaderLocation(sh, "pLumaNoise");
        locChroma = GetShaderLocation(sh, "pChromaNoise");
        locSnow = GetShaderLocation(sh, "pSnow");
        locDelay = GetShaderLocation(sh, "pChromaDelay");
        locPhase = GetShaderLocation(sh, "pPhaseNoise");
        locRing = GetShaderLocation(sh, "pRinging");
        locSharp = GetShaderLocation(sh, "pSharpen");
        locWave = GetShaderLocation(sh, "pEdgeWave");
        locWaveSpd = GetShaderLocation(sh, "pEdgeWaveSpeed");
        locHead = GetShaderLocation(sh, "pHeadSwitch");
        locLoss = GetShaderLocation(sh, "pChromaLoss");
        locScan = GetShaderLocation(sh, "pScanlines");
        locTape = GetShaderLocation(sh, "pTapeBlur");
        return true;
    }

    void unload() {
        if (ok) { UnloadShader(sh); ok = false; sh = {0}; }
    }

    void apply(const NtscFX& fx, int w, int h) const {
        if (!ok) return;
        auto f = [&](int loc, float v) { if (loc >= 0) SetShaderValue(sh, loc, &v, SHADER_UNIFORM_FLOAT); };
        if (locRes >= 0) { Vector2 r = {(float)w, (float)h}; SetShaderValue(sh, locRes, &r, SHADER_UNIFORM_VEC2); }
        f(locTime, (float)GetTime());
        f(locSeed, fx.seed);
        f(locNoise, fx.noise);
        f(locLuma, fx.lumaNoise);
        f(locChroma, fx.chromaNoise);
        f(locSnow, fx.snow);
        f(locDelay, fx.chromaDelay);
        f(locPhase, fx.phaseNoise);
        f(locRing, fx.ringing);
        f(locSharp, fx.sharpen);
        f(locWave, fx.edgeWave);
        f(locWaveSpd, fx.edgeWaveSpeed);
        f(locHead, fx.headSwitch);
        f(locLoss, fx.chromaLoss);
        f(locScan, fx.scanlines);
        f(locTape, fx.tapeBlur);
    }
};

// ---------------------------------------------------------------------------
// Lector de presets .json de ntsc-rs
//
// Su formato es un objeto PLANO: "clave": valor, todo al mismo nivel (incluso
// los hijos de un grupo, que van sueltos junto al booleano del grupo), más un
// "version": 1. Con eso no hace falta una biblioteca de JSON: basta buscar
// cada clave CON SUS COMILLAS —así "composite_noise" no casa dentro de
// "composite_noise_intensity"— y leer el número que va detrás.
// ---------------------------------------------------------------------------
struct NtscPresetResult {
    bool ok = false;
    int  applied = 0;     // claves que sí tienen equivalente aquí
    int  ignored = 0;     // claves del archivo que este efecto no reproduce
    std::string note;
};

// Busca "clave": y devuelve su valor numérico (true=1, false=0).
inline bool NtscJsonGet(const std::string& s, const char* key, double& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return false;
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    if (p >= s.size()) return false;
    if (s.compare(p, 4, "true") == 0)  { out = 1.0; return true; }
    if (s.compare(p, 5, "false") == 0) { out = 0.0; return true; }
    char* end = nullptr;
    double v = strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;   // no había número
    out = v;
    return true;
}

inline NtscPresetResult LoadNtscPreset(const char* path, NtscFX& fx) {
    NtscPresetResult r;
    FILE* f = fopen(path, "rb");
    if (!f) { r.note = "could not open the file"; return r; }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.empty()) { r.note = "the file is empty"; return r; }

    double ver = 0.0;
    if (!NtscJsonGet(text, "version", ver)) {
        r.note = "this does not look like an ntsc-rs preset (no \"version\" key)";
        return r;
    }

    // Cuenta TODAS las claves del archivo para poder decir cuántas se quedaron
    // fuera. Una clave es un "texto" seguido de dos puntos.
    int totalKeys = 0;
    for (size_t i = 0; i + 1 < text.size(); i++) {
        if (text[i] != '"') continue;
        size_t e = text.find('"', i + 1);
        if (e == std::string::npos) break;
        size_t c = e + 1;
        while (c < text.size() && (text[c] == ' ' || text[c] == '\t' || text[c] == '\n' || text[c] == '\r')) c++;
        if (c < text.size() && text[c] == ':') totalKeys++;
        i = e;
    }

    auto clamp01 = [](double v) { return (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)); };
    double v = 0.0, on = 0.0;
    int applied = 0;
    // Lee `key` escalándolo a 0..1 y sólo si su grupo (`gate`, opcional) está
    // encendido; un grupo apagado deja el parámetro en cero, como en ntsc-rs.
    auto take = [&](const char* key, const char* gate, double scale, float& dst) {
        if (!NtscJsonGet(text, key, v)) return;
        applied++;
        if (gate && NtscJsonGet(text, gate, on) && on == 0.0) { dst = 0.0f; return; }
        dst = clamp01(v * scale);
    };

    take("composite_noise_intensity",   "composite_noise", 1.0,  fx.noise);
    take("luma_noise_intensity",        "luma_noise",      1.0,  fx.lumaNoise);
    take("chroma_noise_intensity",      "chroma_noise",    1.0,  fx.chromaNoise);
    take("snow_intensity",              nullptr,           1.0,  fx.snow);
    take("chroma_delay_horizontal",     nullptr,           0.15, fx.chromaDelay);
    take("chroma_phase_noise_intensity",nullptr,           1.0,  fx.phaseNoise);
    take("ringing_power",               "ringing",         0.20, fx.ringing);
    take("composite_preemphasis",       nullptr,           0.25, fx.sharpen);
    take("vhs_edge_wave",               "vhs_settings",    0.20, fx.edgeWave);
    take("vhs_edge_wave_speed",         "vhs_settings",    0.20, fx.edgeWaveSpeed);
    take("head_switching_height",       "head_switching",  0.06, fx.headSwitch);
    take("vhs_chroma_loss",             "vhs_settings",    1.0,  fx.chromaLoss);

    // La velocidad de cinta es una lista (SP / LP / EP), no un número: cuanto
    // más lenta, menos ancho de banda y por tanto más borroso.
    if (NtscJsonGet(text, "vhs_tape_speed", v)) {
        applied++;
        static const float kTape[4] = {0.25f, 0.45f, 0.70f, 0.0f};
        int idx = (int)v;
        fx.tapeBlur = kTape[(idx < 0 || idx > 3) ? 0 : idx];
    }
    if (NtscJsonGet(text, "random_seed", v)) {
        applied++;
        // La semilla de ntsc-rs es un entero grande; aquí sólo hace falta que
        // dos presets distintos den ruidos distintos.
        fx.seed = (float)(fmod(fabs(v), 1000.0) + 1.0);
    }

    r.ok = true;
    r.applied = applied;
    r.ignored = totalKeys - applied - 1;   // -1 por "version"
    if (r.ignored < 0) r.ignored = 0;
    r.note = "translated, not an exact emulation";
    return r;
}
