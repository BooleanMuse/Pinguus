#pragma once
// PdEffects.h
//
// Per-slot audio effects powered by libpd (Pure Data). Each CLIP/SAMPLE slot
// can have a .pd patch loaded as an insert effect: the slot's dry audio is
// run through the patch (adc~ -> ... -> dac~) before hitting the master.
//
// The same .pd format is written by both Pure Data (vanilla) and PlugData,
// so patches from either work here.
//
// THREADING: loadPatch()/clearPatch() build/tear down a Pd DSP graph (file
// I/O + allocation) and MUST be called from the main thread with the audio
// device STOPPED. process() is the only method safe to call from the audio
// callback — it just runs libpd_process_float on a prepared instance.
//
// Each slot gets its own libpd instance (multi-instance build: PDINSTANCE),
// so patches are isolated and can't cross-talk.

// PINGUUS_NO_PD compila el motor SIN libpd, dejando esta clase como un cascarón
// que nunca tiene patch. Existe porque el puerto a Android no lleva Pure Data
// (ni libpd cruzado para ARM, ni sitio en una interfaz táctil para un editor de
// efectos), y sin esto el motor entero es imposible de compilar allí: este era
// el ÚNICO archivo del motor que arrastraba una dependencia externa.
#ifdef PINGUUS_NO_PD

#define PD_MAX_SLOTS 128

class PdEffects {
public:
    void init(int) {}
    bool hasPatch(int) const { return false; }
    bool loadPatch(int, const char*) { return false; }
    void clearPatch(int) {}
    void process(int, float*, unsigned int) {}
};

#else

extern "C" {
#include "z_libpd.h"
}

#include <cstring>
#include <cstdlib>
#include <string>

#define PD_MAX_SLOTS 128

class PdEffects {
public:
    void init(int sampleRate) {
        if (initialized) return;
        libpd_init();          // global, once
        sr = sampleRate;
        initialized = true;
    }

    bool hasPatch(int slot) const {
        return slot >= 0 && slot < PD_MAX_SLOTS && inst[slot] != nullptr;
    }

    // Loads a .pd patch as slot's effect. Main thread, audio stopped.
    bool loadPatch(int slot, const char* fullPath) {
        if (!initialized || slot < 0 || slot >= PD_MAX_SLOTS) return false;
        clearPatch(slot);

        t_pdinstance* p = libpd_new_instance();
        if (p == nullptr) return false;
        libpd_set_instance(p);
        libpd_init_audio(1, 1, sr);           // mono in / mono out
        // Turn DSP on: [; pd dsp 1(
        libpd_start_message(1);
        libpd_add_float(1.0f);
        libpd_finish_message("pd", "dsp");

        // Split path into dir + filename for libpd_openfile.
        std::string full(fullPath);
        std::string dir = ".", file = full;
        size_t s = full.find_last_of("/\\");
        if (s != std::string::npos) { dir = full.substr(0, s); file = full.substr(s + 1); }

        void* handle = libpd_openfile(file.c_str(), dir.c_str());
        if (handle == nullptr) {
            libpd_free_instance(p);
            return false;
        }
        inst[slot] = p;
        patch[slot] = handle;
        return true;
    }

    void clearPatch(int slot) {
        if (slot < 0 || slot >= PD_MAX_SLOTS || inst[slot] == nullptr) return;
        libpd_set_instance((t_pdinstance*)inst[slot]);
        if (patch[slot]) libpd_closefile(patch[slot]);
        libpd_free_instance((t_pdinstance*)inst[slot]);
        inst[slot] = nullptr;
        patch[slot] = nullptr;
    }

    // Runs a mono buffer of `frames` samples in place through slot's patch.
    // Audio-thread safe. Processes whole 64-sample blocks; any tail (only if
    // the device period isn't a multiple of 64) is left dry.
    void process(int slot, float* buf, unsigned int frames) {
        if (slot < 0 || slot >= PD_MAX_SLOTS || inst[slot] == nullptr) return;
        const int bs = libpd_blocksize(); // 64
        int ticks = (int)frames / bs;
        if (ticks <= 0) return;
        libpd_set_instance((t_pdinstance*)inst[slot]);
        libpd_process_float(ticks, buf, buf); // in == out is fine
    }

    ~PdEffects() {
        for (int i = 0; i < PD_MAX_SLOTS; i++) clearPatch(i);
    }

private:
    bool initialized = false;
    int sr = 44100;
    void* inst[PD_MAX_SLOTS] = {nullptr};
    void* patch[PD_MAX_SLOTS] = {nullptr};
};

#endif // PINGUUS_NO_PD
