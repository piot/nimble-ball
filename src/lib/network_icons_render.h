/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_BALL_NETWORK_ICONS_RENDER_H
#define NIMBLE_BALL_NETWORK_ICONS_RENDER_H

#include <sdl-render/sprite.h>
#include <sdl-render/window.h>

struct SrRects;
struct Lagometer;

typedef struct NlNetworkIconsRender {
    struct SrSprites* spritesRender;
    SrSprite droppedDatagramSprite;
    SrSprite authoritativeTimeIntervalWarningSprite;
} NlNetworkIconsRender;

typedef struct NlNetworkIconsState {
    bool droppedDatagram;
    bool authoritativeTimeIntervalWarning;
} NlNetworkIconsState;

void nlNetworkIconsRenderInit(NlNetworkIconsRender* self, struct SrSprites* spritesRender, SDL_Texture* texture);
void nlNetworkIconsRenderUpdate(NlNetworkIconsRender* self, NlNetworkIconsState state);

#endif
