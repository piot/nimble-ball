/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include <clog/clog.h>
#include <sdl-render/gamepad.h>

void nlFrontendInit(NlFrontend* self)
{
    self->phase = NlFrontendPhaseMainMenu;
    self->mainMenuSelect = NlFrontendMenuSelectJoin;
    self->mainMenuSelected = NlFrontendMenuSelectUnknown;
}

void nlFrontendHandleInput(NlFrontend* self, SrGamepad* gamepad)
{
    int verticalAxis = gamepad->verticalAxis < 0 ? -1 : (gamepad->verticalAxis > 0 ? 1 : 0);

    int pressed = (verticalAxis != 0 && self->virtualGamepad.verticalAxis == 0);
    int pressedAdvance = (gamepad->a != 0) && !self->virtualGamepad.advanceHeldDown;
    self->virtualGamepad.verticalAxis = verticalAxis;
    self->virtualGamepad.advanceHeldDown = gamepad->a != 0;

    if (pressedAdvance) {
        self->mainMenuSelected = self->mainMenuSelect;
        return;
    }

    if (!pressed) {
        return;
    }

    CLOG_VERBOSE("pressed verticalAxis: %d", verticalAxis)

    if ((verticalAxis == 1) && self->mainMenuSelect == NlFrontendMenuSelectHost) {
        self->mainMenuSelect = NlFrontendMenuSelectJoin;
    } else if (verticalAxis == -1 && self->mainMenuSelect == NlFrontendMenuSelectJoin) {
        self->mainMenuSelect = NlFrontendMenuSelectHost;
    }
}
