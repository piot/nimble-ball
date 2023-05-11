/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_BALL_LAGOMETER_RENDER_H
#define NIMBLE_BALL_LAGOMETER_RENDER_H

#include <sdl-render/font.h>
#include <sdl-render/window.h>

struct SrRects;
struct Lagometer;

typedef struct NlLagometerRender {
   SrWindow* window;
   SDL_Color receivedColor;
   SDL_Color latencyHighColor;
   SDL_Color droppedColor;
   SDL_Color backgroundColor;
   SrFont font;
   struct SrRects* rectsRender;
} NlLagometerRender;

void nlLagometerRenderInit(NlLagometerRender* self, SrWindow* window, SrFont font, struct SrRects* rectsRender);
void nlLagometerRenderUpdate(NlLagometerRender* self, const struct Lagometer* lagometer);

#endif
