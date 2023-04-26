/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include <SDL2/SDL.h>
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

int main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;

    g_clog.log = clog_console;
    CLOG_VERBOSE("Nimble Ball start!")

    ImprintDefaultSetup imprintDefaultSetup;
    imprintDefaultSetupInit(&imprintDefaultSetup, 5 * 1024 * 1024);

    NlRender render;

    nlRenderInit(&render);

    NlSimulationVm authoritativeVm;
    Clog authSubLog;
    authSubLog.constantPrefix = "NimbleBallAuth";
    authSubLog.config = &g_clog;
    nlSimulationVmInit(&authoritativeVm, authSubLog);

    NlSimulationVm predictedVm;
    Clog predictedSubLog;
    predictedSubLog.constantPrefix = "NimbleBallPredicted";
    predictedSubLog.config = &g_clog;
    nlSimulationVmInit(&predictedVm, predictedSubLog);

    RectifySetup rectifySetup;
    Clog rectifySubLog;
    rectifySubLog.constantPrefix = "Rectify";
    rectifySubLog.config = &g_clog;
    rectifySetup.log = rectifySubLog;
    rectifySetup.maxPlayerCount = 8;
    rectifySetup.maxInputOctetSize = 120;
    rectifySetup.allocator = &imprintDefaultSetup.slabAllocator.info.allocator;

    Rectify rectify;

    StepId initialStepId = 0;

    NlGame authoritativeInitialGameState;
    nlGameInit(&authoritativeInitialGameState);

    TransmuteState initialAuthoritativeState;
    initialAuthoritativeState.state = &authoritativeInitialGameState;
    initialAuthoritativeState.octetSize = sizeof(authoritativeInitialGameState);

    rectifyInit(&rectify, authoritativeVm.transmuteVm, predictedVm.transmuteVm, rectifySetup, initialAuthoritativeState,
                initialStepId);

    StepId stepId = initialStepId;

    SrGamepad gamepad;

    srGamepadInit(&gamepad);

    while (1) {
        rectifyUpdate(&rectify);
        int wantsToQuit = srGamepadPoll(&gamepad);
        if (wantsToQuit) {
            break;
        }

        NlPlayerInput gameInput = gamepadToPlayerInput(&gamepad);
        gameInput.participantId = 2;

        TransmuteInput transmuteInput;
        TransmuteParticipantInput participantInputs[1];
        participantInputs[0].input = &gameInput;
        participantInputs[0].octetSize = sizeof(gameInput);

        transmuteInput.participantInputs = participantInputs;
        transmuteInput.participantCount = 1;

        rectifyAddAuthoritativeStep(&rectify, &transmuteInput, stepId++);

        TransmuteState authoritativeTransmuteState = transmuteVmGetState(&rectify.authoritative.transmuteVm);
        const NlGame* authoritativeGame = (NlGame*) authoritativeTransmuteState.state;

        TransmuteState predictedTransmuteState = transmuteVmGetState(&rectify.predicted.transmuteVm);
        const NlGame* predictedGame = (NlGame*) predictedTransmuteState.state;

        nlRenderUpdate(&render, authoritativeGame, predictedGame);

    }

    nlRenderClose(&render);
}
