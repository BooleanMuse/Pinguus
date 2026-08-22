#pragma once
// BlendFX.h
//
// Modos de fusión de capa (los "blend modes" de un editor de imagen) para el
// collage, compartidos por el escritorio y por Android.
//
// POR QUÉ SÓLO CINCO Y NO VEINTE
//   Estos cinco salen EXACTOS con los factores de mezcla que ya tiene la
//   tarjeta gráfica: una llamada de estado y a dibujar, sin un shader por capa
//   y sin leer el fotograma de fondo. Los demás modos de Photoshop (overlay,
//   luz suave, tono, saturación...) necesitan la fórmula completa con el fondo
//   como entrada, lo que obliga a un paso de render extra POR CAPA. En un
//   teléfono eso se nota, y aquí el collage puede tener ocho capas a la vez.
//
//   Además, todo lo de aquí funciona igual en OpenGL de escritorio y en
//   OpenGL ES 2 (el de Android): no se usa glBlendEquation con MIN/MAX, que en
//   ES 2 depende de una extensión y habría dejado dos programas distintos.
//
// LA OPACIDAD
//   En NORMAL, MULTIPLY, ADD y SUBTRACT el alfa del tinte ya atenúa la capa,
//   porque el factor de origen lleva SRC_ALPHA (o el destino lleva
//   1-SRC_ALPHA). SCREEN es la excepción: su factor de origen es
//   1-DST_COLOR y ahí el alfa no entra en la cuenta, así que la opacidad se
//   aplica atenuando el COLOR del tinte. De eso se encarga BlendTintFX, para
//   que quien dibuja no tenga que acordarse de la excepción.

#include "raylib.h"
#include "rlgl.h"

enum BlendModeFX : int {
    BLEND_FX_NORMAL = 0, BLEND_FX_MULTIPLY, BLEND_FX_SCREEN, BLEND_FX_ADD, BLEND_FX_SUBTRACT,
    BLEND_FX_COUNT
};

static const char* kBlendNames[BLEND_FX_COUNT] = {
    "NORMAL", "MULTIPLY", "SCREEN", "ADD", "SUBTRACT"
};

// Qué hace cada uno, para la ayuda de la interfaz.
static const char* kBlendHelp[BLEND_FX_COUNT] = {
    "the layer simply covers what is below",
    "darkens: white vanishes, black stays",
    "lightens: black vanishes, white stays",
    "adds light, good for flashes and glows",
    "removes light from what is below",
};

// Tinte con el que hay que dibujar la capa para que su opacidad se respete en
// el modo elegido (ver la nota de arriba sobre SCREEN).
inline Color BlendTintFX(int blend, float opacity) {
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    if (blend == BLEND_FX_SCREEN) {
        unsigned char v = (unsigned char)(opacity * 255.0f + 0.5f);
        return (Color){v, v, v, 255};
    }
    return (Color){255, 255, 255, (unsigned char)(opacity * 255.0f + 0.5f)};
}

inline void BeginBlendModeFX(int blend) {
    switch (blend) {
        case BLEND_FX_MULTIPLY:
            BeginBlendMode(BLEND_MULTIPLIED);
            break;
        case BLEND_FX_ADD:
            BeginBlendMode(BLEND_ADDITIVE);
            break;
        case BLEND_FX_SCREEN:
            // resultado = origen*(1-destino) + destino, que es la fórmula de
            // "trama" de toda la vida.
            rlSetBlendFactors(RL_ONE_MINUS_DST_COLOR, RL_ONE, RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM);
            break;
        case BLEND_FX_SUBTRACT:
            // resultado = destino - origen*alfa (resta invertida: lo de abajo
            // menos lo de arriba, que es lo que se espera de "restar").
            rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE, RL_FUNC_REVERSE_SUBTRACT);
            BeginBlendMode(BLEND_CUSTOM);
            break;
        default:
            break; // NORMAL: el modo por defecto, no hace falta tocar nada
    }
}

inline void EndBlendModeFX(int blend) {
    if (blend != BLEND_FX_NORMAL) EndBlendMode();
}
