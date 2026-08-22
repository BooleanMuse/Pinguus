// ============================================================================
// BvhAnim.h — Read a .BVH motion capture file (hierarchy + motion) into plain
// per-joint rotation tracks.
//
// WHY BVH TOO, IF .VRMA ALREADY WORKS
//   .vrma is the VRM world's animation format and it retargets perfectly, but
//   almost nothing PRODUCES it. BVH is the opposite: it is the lingua franca of
//   motion capture — Mixamo, CMU's free library, Rokoko, Perception Neuron,
//   Blender, iPhone-based mocap apps and every "free dance mocap" pack on the
//   internet export it. It is also a plain TEXT format, so reading it needs no
//   library at all. Between the two, an avatar in Pinguus can dance to almost
//   any motion a user can find or record.
//
// WHAT THIS FILE IS AND IS NOT
//   It is a parser: it turns the file into joints and per-frame rotations, and
//   nothing else. It does NOT know about VRM, humanoid roles, or raylib — the
//   caller maps joint NAMES to humanoid bones and retargets, exactly as it
//   already does for .vrma. Keeping it that way means the risky part (naming
//   conventions differ wildly between mocap tools) lives next to the code that
//   already handles VRoid/Unity/Mixamo/Koikatsu naming.
//
// WHAT WE USE AND WHAT WE DROP
//   Only ROTATIONS are kept. BVH also stores the root's TRANSLATION channels
//   (where the hips travel), but Pinguus draws the avatar at a fixed spot in
//   the collage and its humanoid rig is rotation-only, so root motion would
//   just make the character slide off the frame. OFFSETs (bone lengths) are
//   parsed for completeness but not used: the TARGET skeleton's proportions
//   win, which is the whole point of retargeting.
//
// FORMAT NOTES THAT MATTER
//   * Rotation channels come in a per-joint ORDER (usually Z X Y). The order is
//     not decoration: the value list follows it, and the rotations compose in
//     that order (R = Rfirst * Rsecond * Rthird). Assuming XYZ is the classic
//     "why is the elbow inside out" bug.
//   * "End Site" blocks are leaves with an OFFSET and NO channels; they must be
//     skipped without consuming motion values or every later joint reads the
//     wrong column.
//   * Files are whitespace-formatted, not line-formatted: tokenise, don't
//     parse lines.
// ============================================================================
#ifndef PINGUUS_BVHANIM_H
#define PINGUUS_BVHANIM_H

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>

// Cuaternión mínimo propio: este archivo no depende de raylib (quien lo usa
// convierte al suyo, que tiene exactamente el mismo orden x,y,z,w).
struct BvhQuat { float x = 0, y = 0, z = 0, w = 1; };

struct BvhJoint {
    std::string name;
    int parent = -1;
    float offset[3] = {0, 0, 0};
    // Rotación por frame (ya compuesta en el orden de canales del archivo).
    std::vector<BvhQuat> rot;
};

struct BvhClip {
    std::string name;
    std::vector<BvhJoint> joints;
    int frameCount = 0;
    float frameTime = 1.0f / 30.0f;
    float duration() const { return frameCount > 0 ? frameCount * frameTime : 1.0f; }
};

// --- utilidades de cuaterniones (locales, para no arrastrar dependencias) ---
inline BvhQuat BvhQuatMul(const BvhQuat& a, const BvhQuat& b) {
    BvhQuat r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}
// Rotación de `deg` grados sobre el eje 0=X, 1=Y, 2=Z.
inline BvhQuat BvhQuatAxis(int axis, float deg) {
    float h = deg * 0.017453292519943295f * 0.5f;
    float s = sinf(h);
    BvhQuat q;
    q.w = cosf(h);
    if (axis == 0) q.x = s;
    else if (axis == 1) q.y = s;
    else q.z = s;
    return q;
}

// ---------------------------------------------------------------------------
// Tokenizador: el formato es texto separado por espacios/saltos, y las llaves
// son tokens por su cuenta aunque vengan pegadas.
// ---------------------------------------------------------------------------
class BvhTokenizer {
public:
    explicit BvhTokenizer(const std::string& text) : t_(text) {}
    bool next(std::string& out) {
        while (i_ < t_.size() && isspace((unsigned char)t_[i_])) i_++;
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{' || c == '}') { out.assign(1, c); i_++; return true; }
        size_t s = i_;
        while (i_ < t_.size() && !isspace((unsigned char)t_[i_]) && t_[i_] != '{' && t_[i_] != '}') i_++;
        out = t_.substr(s, i_ - s);
        return true;
    }
    // Igual que next(), pero exige que el token sea `expect` (sin distinguir
    // mayúsculas): así un archivo corrupto falla donde toca en vez de más lejos.
    bool expect(const char* word) {
        std::string tok;
        if (!next(tok)) return false;
        return equalsNoCase(tok, word);
    }
    static bool equalsNoCase(const std::string& a, const char* b) {
        size_t n = strlen(b);
        if (a.size() != n) return false;
        for (size_t i = 0; i < n; i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    }
private:
    const std::string& t_;
    size_t i_ = 0;
};

// Canales de un joint: cuáles de los 6 valores por frame le tocan y en qué
// orden están sus rotaciones.
struct BvhChannelSet {
    int count = 0;
    // Para cada canal del joint: 0..2 = posición X/Y/Z, 3..5 = rotación X/Y/Z,
    // -1 = desconocido (se lee y se descarta, para no descuadrar las columnas).
    std::vector<int> kind;
};

// Carga un .bvh. Devuelve false si no es un BVH legible.
inline bool LoadBvhClip(const char* path, BvhClip& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (200L << 20)) { fclose(f); return false; }   // 200 MB de tope
    std::string text((size_t)sz, '\0');
    size_t got = fread(&text[0], 1, (size_t)sz, f);
    fclose(f);
    text.resize(got);

    BvhTokenizer tk(text);
    std::string tok;
    if (!tk.next(tok) || !BvhTokenizer::equalsNoCase(tok, "HIERARCHY")) return false;

    out = BvhClip();
    std::vector<BvhChannelSet> chans;
    std::vector<int> stack;          // pila de joints abiertos ({ ... })
    int current = -1;
    bool inEndSite = false;

    while (tk.next(tok)) {
        if (BvhTokenizer::equalsNoCase(tok, "ROOT") || BvhTokenizer::equalsNoCase(tok, "JOINT")) {
            std::string nm;
            if (!tk.next(nm)) return false;
            BvhJoint j;
            j.name = nm;
            j.parent = current;
            out.joints.push_back(j);
            chans.push_back(BvhChannelSet());
            current = (int)out.joints.size() - 1;
            inEndSite = false;
        } else if (BvhTokenizer::equalsNoCase(tok, "End")) {
            // "End Site": una hoja sin canales. Se traga entera al llegar su
            // llave; marcarla evita que su OFFSET se asigne al joint padre.
            std::string site;
            tk.next(site);
            inEndSite = true;
        } else if (tok == "{") {
            if (!inEndSite) stack.push_back(current);
        } else if (tok == "}") {
            if (inEndSite) { inEndSite = false; }
            else {
                if (!stack.empty()) stack.pop_back();
                current = stack.empty() ? -1 : stack.back();
            }
        } else if (BvhTokenizer::equalsNoCase(tok, "OFFSET")) {
            std::string a, b, c;
            if (!tk.next(a) || !tk.next(b) || !tk.next(c)) return false;
            if (!inEndSite && current >= 0) {
                out.joints[current].offset[0] = (float)atof(a.c_str());
                out.joints[current].offset[1] = (float)atof(b.c_str());
                out.joints[current].offset[2] = (float)atof(c.c_str());
            }
        } else if (BvhTokenizer::equalsNoCase(tok, "CHANNELS")) {
            std::string n;
            if (!tk.next(n)) return false;
            int cnt = atoi(n.c_str());
            if (cnt < 0 || cnt > 16) return false;
            BvhChannelSet cs;
            cs.count = cnt;
            for (int i = 0; i < cnt; i++) {
                std::string cn;
                if (!tk.next(cn)) return false;
                int kind = -1;
                if (BvhTokenizer::equalsNoCase(cn, "Xposition")) kind = 0;
                else if (BvhTokenizer::equalsNoCase(cn, "Yposition")) kind = 1;
                else if (BvhTokenizer::equalsNoCase(cn, "Zposition")) kind = 2;
                else if (BvhTokenizer::equalsNoCase(cn, "Xrotation")) kind = 3;
                else if (BvhTokenizer::equalsNoCase(cn, "Yrotation")) kind = 4;
                else if (BvhTokenizer::equalsNoCase(cn, "Zrotation")) kind = 5;
                cs.kind.push_back(kind);
            }
            if (current >= 0) chans[current] = cs;
        } else if (BvhTokenizer::equalsNoCase(tok, "MOTION")) {
            break;
        }
    }
    if (out.joints.empty()) return false;

    // ---- MOTION ----
    // "Frames: N" y "Frame Time: t" (algunos exportadores escriben "Frames:"
    // pegado al número o "Frame" "Time:" por separado; se toleran ambos).
    int frames = 0;
    float frameTime = 1.0f / 30.0f;
    while (tk.next(tok)) {
        if (BvhTokenizer::equalsNoCase(tok, "Frames:")) {
            std::string n;
            if (!tk.next(n)) return false;
            frames = atoi(n.c_str());
        } else if (BvhTokenizer::equalsNoCase(tok, "Frame")) {
            std::string t1;
            if (!tk.next(t1)) return false;                 // "Time:"
            if (BvhTokenizer::equalsNoCase(t1, "Time:")) {
                std::string v;
                if (!tk.next(v)) return false;
                frameTime = (float)atof(v.c_str());
                break;
            }
        } else if (frames > 0) {
            // Ya empezaron los números: este token es el primer valor.
            break;
        }
    }
    if (frames <= 0) return false;
    if (frameTime <= 0.0001f) frameTime = 1.0f / 30.0f;

    int totalChannels = 0;
    for (const BvhChannelSet& cs : chans) totalChannels += cs.count;
    if (totalChannels <= 0) return false;

    for (size_t j = 0; j < out.joints.size(); j++) out.joints[j].rot.reserve(frames);

    // Los valores vienen en una sola tirada: frames × totalChannels. Se leen
    // en orden y se reparten por joint (los de posición se descartan).
    std::vector<float> vals((size_t)totalChannels);
    int framesRead = 0;
    for (int fr = 0; fr < frames; fr++) {
        bool ok = true;
        for (int c = 0; c < totalChannels; c++) {
            std::string v;
            if (!tk.next(v)) { ok = false; break; }
            vals[(size_t)c] = (float)atof(v.c_str());
        }
        if (!ok) break;                    // archivo truncado: nos quedamos con lo leído
        int base = 0;
        for (size_t j = 0; j < out.joints.size(); j++) {
            const BvhChannelSet& cs = chans[j];
            BvhQuat q;                     // identidad
            for (int c = 0; c < cs.count; c++) {
                int kind = cs.kind[(size_t)c];
                if (kind >= 3) {
                    // R = Rprimero * Rsegundo * Rtercero, en el orden del archivo.
                    q = BvhQuatMul(q, BvhQuatAxis(kind - 3, vals[(size_t)(base + c)]));
                }
            }
            out.joints[j].rot.push_back(q);
            base += cs.count;
        }
        framesRead++;
    }
    if (framesRead <= 0) return false;

    out.frameCount = framesRead;
    out.frameTime = frameTime;
    return true;
}

#endif // PINGUUS_BVHANIM_H
