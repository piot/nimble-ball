/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_BALL_FRONTEND_H
#define NIMBLE_BALL_FRONTEND_H

#include <stdbool.h>

struct SrGamepad;

typedef enum NlFrontendMenuSelect {
    NlFrontendMenuSelectUnknown,
    NlFrontendMenuSelectHost,
    NlFrontendMenuSelectJoin,
    NlFrontendMenuSelectHostOnline,
    NlFrontendMenuSelectJoinOnline,
} NlFrontendMenuSelect;

typedef enum NlFrontendPhase {
    NlFrontendPhaseMainMenu,
    NlFrontendPhaseJoining,
    NlFrontendPhaseHosting,
    NlFrontendPhaseHostingOnline,
    NlFrontendPhaseInGame,
} NlFrontendPhase;

typedef struct NlFrontendGamepad {
    int verticalAxis;
    bool advanceHeldDown;
} NlFrontendGamepad;

typedef struct NlFrontend {
    NlFrontendMenuSelect mainMenuSelect;
    NlFrontendMenuSelect mainMenuSelected;
    NlFrontendPhase phase;
    NlFrontendGamepad virtualGamepad;
} NlFrontend;

void nlFrontendInit(NlFrontend* self);
void nlFrontendHandleInput(NlFrontend* self, struct SrGamepad* gamepad);

#endif
