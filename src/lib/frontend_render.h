/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_BALL_FRONTEND_RENDER_H
#define NIMBLE_BALL_FRONTEND_RENDER_H

#include <sdl-render/font.h>
#include <sdl-render/window.h>

struct NlFrontend;

typedef struct NlFrontendRender {
    SrWindow* window;
    SDL_Color defaultColor;
    SDL_Color selectedColor;
    SrFont font;
} NlFrontendRender;

void nlFrontendRenderInit(NlFrontendRender* self, SrWindow* window, SrFont font);
void nlFrontendRenderUpdate(NlFrontendRender* self, const struct NlFrontend* frontend);

#endif
