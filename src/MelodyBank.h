#pragma once
// MelodyBank.h
//
// EL BANCO DE MELODÍAS: ocho melodías guardadas, cada una una lista de notas ya
// colocadas en pasos de semicorchea. Se comporta como los demás valores de la
// paleta (el acorde del arpegio, la espera del HOLD, el efecto del FX): eliges
// la celda MELODY, eliges cuál de las ocho, y al pincharla en el lienzo se
// estampa esa melodía hacia la derecha con el clip o la muestra que tengas
// seleccionados.
//
// POR QUÉ UN BANCO Y NO UNA SOLA
//   Una melodía sola obligaría a volver a analizar cada vez que quisieras la
//   otra. Con ocho a mano se puede tener el estribillo, la respuesta y el bajo
//   analizados a la vez y ponerlos donde haga falta, que es como se trabaja de
//   verdad. Ocho es el número de la paleta: entran en una fila de botones sin
//   que ninguno baje del tamaño mínimo táctil en el móvil.
//
// POR QUÉ SE GUARDAN EN PASOS Y NO EN SEGUNDOS
//   Porque una melodía del banco es una PIEZA QUE SE ESTAMPA, no una grabación.
//   Al analizar ya se decidió el tempo y la cuantización; a partir de ahí lo
//   que importa es en qué casilla cae cada nota. Guardarlo en segundos
//   obligaría a re-cuantizar en cada estampado y la misma melodía saldría
//   distinta si se cambia el BPM entre una y otra.

#include "PitchToNotes.h"

#include <string>
#include <vector>

#define MELODY_BANK_SIZE 8

struct MelodyClip {
    std::vector<PitchToNotes::StepNote> notes;
    std::string name;

    bool empty() const { return notes.empty(); }

    // Cuántas casillas ocupa al estamparla (el último paso, más uno).
    int widthSteps() const {
        int w = 0;
        for (const auto& n : notes) if (n.step + 1 > w) w = n.step + 1;
        return w;
    }

    // Rango de tonos, para dibujar la vista previa a escala.
    void range(int& lo, int& hi) const {
        lo = 127; hi = -127;
        for (const auto& n : notes) {
            if (n.semitone < lo) lo = n.semitone;
            if (n.semitone > hi) hi = n.semitone;
        }
        if (notes.empty()) { lo = 0; hi = 0; }
    }

    void clear() { notes.clear(); name.clear(); }

    // Etiqueta corta para el botón de la paleta: el nombre si lo tiene, y si no
    // el número de notas, que es lo mínimo para saber si hay algo dentro.
    std::string label(int idx) const {
        if (empty()) return std::string("M") + (char)('1' + idx) + ":-";
        if (!name.empty()) return name;
        return std::string("M") + (char)('1' + idx) + ":" + std::to_string((int)notes.size());
    }
};

// Nombre por defecto al analizar: el hueco de donde salió, para reconocerla.
inline std::string DefaultMelodyName(int idx, int noteCount) {
    return std::string("M") + (char)('1' + idx) + " (" + std::to_string(noteCount) + ")";
}
