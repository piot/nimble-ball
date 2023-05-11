/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "lagometer_render.h"
#include <lagometer/lagometer.h>
#include <sdl-render/rect.h>

void nlLagometerRenderInit(NlLagometerRender* self, SrWindow* window, SrFont font, SrRects* rectsRender)
{
    self->window = window;
    self->font = font;
    self->rectsRender = rectsRender;

    Uint8 alpha = 68;
    self->receivedColor.r = 0x33;
    self->receivedColor.g = 0xee;
    self->receivedColor.b = 0xcc;
    self->receivedColor.a = alpha;

    self->droppedColor.r = 0xff;
    self->droppedColor.g = 0x22;
    self->droppedColor.b = 0x11;
    self->droppedColor.a = alpha;

    self->latencyHighColor.r = 0xff;
    self->latencyHighColor.g = 0xee;
    self->latencyHighColor.b = 0x11;
    self->latencyHighColor.a = alpha;

    self->backgroundColor.r = 0x22;
    self->backgroundColor.g = 0x33;
    self->backgroundColor.b = 0xee;
    self->backgroundColor.a = alpha;
}

void nlLagometerRenderUpdate(NlLagometerRender* self, const Lagometer* lagometer)
{
    const int barWidth = 2;
    const int fullBarHeight = 200;
    int fullLagometerWidth = lagometer->packets.capacity * barWidth;
    int xOffset = self->rectsRender->width - fullLagometerWidth - 20;
    const int maxLatencyMs = 270;
    const float factor = (float) fullBarHeight / (float) maxLatencyMs;
    int yOffset = fullBarHeight + 10;

    SDL_Color backgroundColor = self->backgroundColor;
    SDL_SetRenderDrawColor(self->rectsRender->renderer, backgroundColor.r, backgroundColor.g, backgroundColor.b,
                           backgroundColor.a);
    srRectsFillRect(self->rectsRender, xOffset, yOffset - fullBarHeight - 2, fullLagometerWidth, fullBarHeight + 2);

    for (size_t i = 0U; i < lagometer->packets.count; ++i) {
        size_t index = (lagometer->packets.readIndex + i) % lagometer->packets.capacity;
        const LagometerPacket* packet = &lagometer->packets.packets[index];
        int x = i * barWidth + xOffset;
        int y = yOffset - fullBarHeight;

        int latencyHeight = (float) packet->latencyMs * factor;
        SDL_Color color = self->receivedColor;
        if (packet->status == LagometerPacketStatusDropped) {
            latencyHeight = fullBarHeight;
            color = self->droppedColor;
        }
        if (packet->latencyMs > 110U) {
            color = self->latencyHighColor;
        }

        if (latencyHeight > fullBarHeight) {
            latencyHeight = fullBarHeight;
        }

        SDL_SetRenderDrawColor(self->rectsRender->renderer, color.r, color.g, color.b, color.a);
        srRectsFillRect(self->rectsRender, x, y, barWidth, latencyHeight);
    }
}
