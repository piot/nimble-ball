/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "network_icons_render.h"

static void setupDroppedDatagramSprite(SrSprite* sprite, SDL_Texture* texture)
{
    sprite->rect.x = 0;
    sprite->rect.y = 80;
    sprite->rect.w = 32;
    sprite->rect.h = 32;
    sprite->texture = texture;
}

static void setupAuthoritativeTimeIntervalWarningSprite(SrSprite* sprite, SDL_Texture* texture)
{
    sprite->rect.x = 32;
    sprite->rect.y = 80;
    sprite->rect.w = 32;
    sprite->rect.h = 32;
    sprite->texture = texture;
}

static void setupImpendingDisconnectSprite(SrSprite* sprite, SDL_Texture* texture)
{
    sprite->rect.x = 64;
    sprite->rect.y = 80;
    sprite->rect.w = 32;
    sprite->rect.h = 32;
    sprite->texture = texture;
}

static void setupDisconnectedSprite(SrSprite* sprite, SDL_Texture* texture)
{
    sprite->rect.x = 96;
    sprite->rect.y = 80;
    sprite->rect.w = 32;
    sprite->rect.h = 32;
    sprite->texture = texture;
}

void nlNetworkIconsRenderInit(NlNetworkIconsRender* self, struct SrSprites* spritesRender, SDL_Texture* texture)
{
    self->spritesRender = spritesRender;
    setupDroppedDatagramSprite(&self->droppedDatagramSprite, texture);
    setupAuthoritativeTimeIntervalWarningSprite(&self->authoritativeTimeIntervalWarningSprite, texture);
    setupImpendingDisconnectSprite(&self->impendingDisconnectWarningSprite, texture);
    setupDisconnectedSprite(&self->disconnectedSprite, texture);
}

void nlNetworkIconsRenderUpdate(NlNetworkIconsRender* self, NlNetworkIconsState state)
{
    int x = self->spritesRender->width - 50;
    int y = self->spritesRender->height - 40;

    if (state.droppedDatagram) {
        srSpritesCopyEx(self->spritesRender, &self->droppedDatagramSprite, x, y, 0, 1.0f, SDL_ALPHA_OPAQUE);
    }

    y -= 40;

    if (state.authoritativeTimeIntervalWarning) {
        srSpritesCopyEx(self->spritesRender, &self->authoritativeTimeIntervalWarningSprite, x, y, 0, 1.0f,
                        SDL_ALPHA_OPAQUE);
    }

    y -= 40;

    switch (state.disconnectInfo) {
        case NlNetworkIconsDisconnectDisconnected:
            srSpritesCopyEx(self->spritesRender, &self->disconnectedSprite, x, y, 0, 1.0f, SDL_ALPHA_OPAQUE);
            break;
        case NlNetworkIconsDisconnectImpending:
            srSpritesCopyEx(self->spritesRender, &self->impendingDisconnectWarningSprite, x, y, 0, 1.0f,
                            SDL_ALPHA_OPAQUE);
            break;
        case NlNetworkIconsDisconnectInfoNone:
            break;
    }
}
