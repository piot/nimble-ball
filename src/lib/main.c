/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include "frontend_render.h"
#include <clog/console.h>
#include <imprint/default_setup.h>
#include <nimble-ball-presentation/render.h>
#include <nimble-ball-simulation/nimble_ball_simulation_vm.h>
#include <rectify/rectify.h>
#include <sdl-render/gamepad.h>

clog_config g_clog;

static NlPlayerInput gamepadToPlayerInput(const SrGamepad* pad)
{
    NlPlayerInput playerInput;
    playerInput.input.inGameInput.horizontalAxis = pad->horizontalAxis;
    playerInput.input.inGameInput.verticalAxis = pad->verticalAxis;
    playerInput.input.inGameInput.passButton = pad->a;
    playerInput.inputType = NlPlayerInputTypeInGame;
    return playerInput;
}

typedef struct NlCombinedRender {
    NlFrontendRender frontendRender;
    NlRender inGame;
    NlFrontend frontend;
} NlCombinedRender;

typedef enum NlAppPhase {
    NlAppPhaseIdle,
    NlAppPhaseTryingToHost,
    NlAppPhaseTryingToJoin,
} NlAppPhase;

typedef struct NlAppState {
    NlAppPhase phase;
} NlAppState;

static void renderCallback(void* _self, SrWindow* window)
{
    NlCombinedRender* combinedRender = (NlCombinedRender*) _self;
    nlFrontendRenderUpdate(&combinedRender->frontendRender, &combinedRender->frontend);
}

int main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;

    g_clog.log = clog_console;
    CLOG_VERBOSE("Nimble Ball start!")

    ImprintDefaultSetup imprintDefaultSetup;
    imprintDefaultSetupInit(&imprintDefaultSetup, 5 * 1024 * 1024);

    NlCombinedRender combinedRender;

    nlRenderInit(&combinedRender.inGame);

    nlFrontendInit(&combinedRender.frontend);

    nlFrontendRenderInit(&combinedRender.frontendRender, &combinedRender.inGame.window, combinedRender.inGame.font);

    SrGamepad gamepads[2];

    srGamepadInit(&gamepads[0]);

    NlAppState appState;
    appState.phase = NlAppPhaseIdle;

    while (1) {
        int wantsToQuit = srGamepadPoll(gamepads, 2);
        if (wantsToQuit) {
            break;
        }

        nlFrontendHandleInput(&combinedRender.frontend, &gamepads[0]);
        switch (appState.phase) {
            case NlAppPhaseIdle:
                switch (combinedRender.frontend.mainMenuSelected) {
                    case NlFrontendMenuSelectJoin:
                        CLOG_DEBUG("Join a game")
                        appState.phase = NlAppPhaseTryingToJoin;
                        combinedRender.frontend.phase = NlFrontendPhaseJoining;
                        break;
                    case NlFrontendMenuSelectHost:
                        CLOG_DEBUG("Host a game")
                        appState.phase = NlAppPhaseTryingToHost;
                        combinedRender.frontend.phase = NlFrontendPhaseHosting;
                        break;
                    case NlFrontendMenuSelectUnknown:
                        break;
                }
                break;
            case NlAppPhaseTryingToHost:
                break;
            case NlAppPhaseTryingToJoin:
                break;
        }

        srWindowRender(&combinedRender.inGame.window, 0x115511, &combinedRender, renderCallback);
    }

    nlRenderClose(&combinedRender.inGame);
}
