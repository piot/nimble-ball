/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend_render.h"
#include "frontend.h"

void nlFrontendRenderInit(NlFrontendRender* self, SrWindow* window, SrFont font)
{
    self->window = window;
    self->font = font;

    self->defaultColor.g = 0x33;
    self->defaultColor.b = 0x44;
    self->defaultColor.r = 0x33;
    self->defaultColor.a = SDL_ALPHA_OPAQUE;

    self->selectedColor.g = 0xff;
    self->selectedColor.b = 0xff;
    self->selectedColor.r = 0xff;
    self->selectedColor.a = SDL_ALPHA_OPAQUE;
}

static SDL_Color selectColor(NlFrontendRender* render, bool isSelected)
{
    return isSelected ? render->selectedColor : render->defaultColor;
}

static void renderMainMenu(NlFrontendRender* self, const NlFrontend* frontend)
{
    srFontRenderAndCopy(&self->font, "Join Game", 220, 230,
                        selectColor(self, frontend->mainMenuSelect == NlFrontendMenuSelectJoin));
    srFontRenderAndCopy(&self->font, "Host Game", 220, 190,
                        selectColor(self, frontend->mainMenuSelect == NlFrontendMenuSelectHost));
}

void nlFrontendRenderUpdate(NlFrontendRender* self, const NlFrontend* frontend)
{
    switch (frontend->phase) {
        case NlFrontendPhaseMainMenu:
            renderMainMenu(self, frontend);
            break;
        case NlFrontendPhaseInGame:
            break;
        case NlFrontendPhaseJoining:
            break;
        case NlFrontendPhaseHosting:
            break;
    }
}
