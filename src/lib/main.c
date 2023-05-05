/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include "frontend_render.h"
#include "nimble-engine-client/client.h"
#include <clog/console.h>
#include <hazy/transport.h>
#include <imprint/default_setup.h>
#include <nimble-ball-presentation/audio.h>
#include <nimble-ball-presentation/render.h>
#include <nimble-ball-simulation/nimble_ball_simulation_vm.h>
#include <nimble-server/server.h>
#include <sdl-render/gamepad.h>
#include <udp-client/udp_client.h>
#include <udp-server-connections/connections.h>

clog_config g_clog;

static NlPlayerInput gamepadToPlayerInput(const SrGamepad* pad)
{
    NlPlayerInput playerInput;
    playerInput.input.inGameInput.horizontalAxis = pad->horizontalAxis;
    playerInput.input.inGameInput.verticalAxis = pad->verticalAxis;
    uint8_t mask = pad->a ? 0x01 : 0x00 | pad->b ? 0x02 : 0x00;
    playerInput.input.inGameInput.buttons = mask;
    playerInput.inputType = NlPlayerInputTypeInGame;
    return playerInput;
}

typedef struct NlCombinedRender {
    NlFrontendRender frontendRender;
    NlRender inGame;
    NlFrontend frontend;

    // Set before every render
    NlRenderStats renderStats;
    const NlGame* authoritative;
    const NlGame* predicted;
} NlCombinedRender;

typedef enum NlAppPhase {
    NlAppPhaseIdle,
    NlAppPhaseNetwork,
} NlAppPhase;

typedef struct NlApp {
    NlAppPhase phase;
    UdpServerSocket udpServer;
    UdpServerConnections udpServerWrapper;
    NbdServer nimbleServer;
    UdpTransportInOut transportForHost;

    bool isHosting;
    NimbleEngineClient nimbleEngineClient;
    UdpClientSocket udpClient;
    UdpTransportInOut transportForClient;

    NlSimulationVm authoritative;
    NlSimulationVm predicted;
    SrWindow window;
    StatsIntPerSecond renderFps;
    SrAudio mixer;
    NlAudio audio;
    HazyDatagramTransportInOut hazyClientTransport;
} NlApp;

static void renderCallback(void* _self, SrWindow* window)
{
    NlCombinedRender* combinedRender = (NlCombinedRender*) _self;

    if (combinedRender->authoritative != 0 && combinedRender->predicted != 0) {
        nlRenderUpdate(&combinedRender->inGame, combinedRender->authoritative, combinedRender->predicted, 0, 0,
                       combinedRender->renderStats);
    }

    nlFrontendRenderUpdate(&combinedRender->frontendRender, &combinedRender->frontend);
}

const int gameUdpPort = 27001;

static void startHosting(NlApp* app, NlFrontend* frontend, ImprintAllocator* allocator,
                         ImprintAllocatorWithFree* allocatorWithFree)
{
    app->phase = NlAppPhaseNetwork;
    frontend->phase = NlFrontendPhaseHosting;
    int errorCode = udpServerInit(&app->udpServer, gameUdpPort, false);
    if (errorCode < 0) {
        CLOG_ERROR("could not host on port %d", gameUdpPort)
    }
    CLOG_INFO("listening on UDP port %d", gameUdpPort);

    udpServerConnectionsInit(&app->udpServerWrapper, &app->udpServer);

    CLOG_INFO("wrapped udp server to handle connections")

    NimbleSerializeVersion serverReportTransmuteVmVersion = {
        app->authoritative.transmuteVm.version.major,
        app->authoritative.transmuteVm.version.minor,
        app->authoritative.transmuteVm.version.patch,
    };

    const size_t maxConnectionCount = 4U;
    const size_t maxParticipantCount = maxConnectionCount * 2U;
    const size_t maxSingleParticipantStepOctetCount = sizeof(NlPlayerInput);

    Clog serverLog;
    serverLog.config = &g_clog;
    serverLog.constantPrefix = "NbdServer";

    NbdServerSetup serverSetup;
    serverSetup.maxSingleParticipantStepOctetCount = maxSingleParticipantStepOctetCount;
    serverSetup.maxParticipantCount = maxParticipantCount;
    serverSetup.maxConnectionCount = maxConnectionCount;
    serverSetup.maxParticipantCountForEachConnection = 2;
    serverSetup.maxGameStateOctetCount = sizeof(NlGame);
    serverSetup.memory = allocator;
    serverSetup.blobAllocator = allocatorWithFree;
    serverSetup.applicationVersion = serverReportTransmuteVmVersion;
    serverSetup.now = monotonicTimeMsNow();
    serverSetup.log = serverLog;
    errorCode = nbdServerInit(&app->nimbleServer, serverSetup);
    if (errorCode < 0) {
        CLOG_ERROR("could not initialize nimble server %d", errorCode)
    }
    CLOG_INFO("nimble server is initialized")

    NlGame initialServerState;
    nlGameInit(&initialServerState);
    // We just add a completely empty game. But it could be setup
    // with specific rules or game mode or similar
    // Since the whole game is blittable structs with no pointers, we can just cast it to an (uint8_t*)
    StepId stepId = 0xcafeU;
    nbdServerReInitWithGame(&app->nimbleServer, (const uint8_t*) &initialServerState, sizeof(initialServerState),
                            stepId, monotonicTimeMsNow());

    CLOG_INFO("nimble server has initial game state. octet count: %zu", app->nimbleServer.game.latestState.octetCount)
    app->isHosting = true;
}

static int clientSideSend(void* _self, const uint8_t* data, size_t size)
{
    NlApp* app = _self;
    return udpClientSend(&app->udpClient, data, size);
}

static int clientSideReceive(void* _self, uint8_t* data, size_t size)
{
    NlApp* app = _self;
    return udpClientReceive(&app->udpClient, data, size);
}

const int maxLocalPlayerCount = 2;
const int useLocalPlayerCount = 1;

static void startJoining(NlApp* app, NlFrontend* frontend, ImprintAllocator* allocator,
                         ImprintAllocatorWithFree* allocatorWithFree)
{
    CLOG_DEBUG("start joining")
    app->phase = NlAppPhaseNetwork;
    frontend->phase = NlFrontendPhaseJoining;

    int errorCode = udpClientInit(&app->udpClient, "127.0.0.1", gameUdpPort);
    if (errorCode < 0) {
        CLOG_ERROR("could not start udp client")
    }
    CLOG_DEBUG("udp client is setup for connection")

    udpTransportInit(&app->transportForClient, app, clientSideSend, clientSideReceive);

    Clog nimbleBallClientLog;
    nimbleBallClientLog.config = &g_clog;
    nimbleBallClientLog.constantPrefix = "NimbleBallClient";

    hazyDatagramTransportInOutInit(&app->hazyClientTransport, app->transportForClient, allocator, allocatorWithFree,
                                   hazyConfigRecommended(), nimbleBallClientLog);

    CLOG_DEBUG("client datagram transport is set")

    NimbleSerializeVersion clientReportTransmuteVmVersion = {
        app->authoritative.transmuteVm.version.major,
        app->authoritative.transmuteVm.version.minor,
        app->authoritative.transmuteVm.version.patch,
    };

    NimbleEngineClientSetup setup;
    setup.memory = allocator;
    setup.blobMemory = allocatorWithFree;
    setup.transport = app->hazyClientTransport.transport;

    setup.authoritative = app->authoritative.transmuteVm;
    setup.predicted = app->predicted.transmuteVm;
    setup.maximumSingleParticipantStepOctetCount = sizeof(NlPlayerInput);
    setup.maximumParticipantCount = 8;
    setup.applicationVersion = clientReportTransmuteVmVersion;
    setup.maxTicksFromAuthoritative = 20U;

    Clog nimbleEngineClientLog;
    nimbleEngineClientLog.config = &g_clog;
    nimbleEngineClientLog.constantPrefix = "NimbleEngineClient";

    setup.log = nimbleEngineClientLog;
    nimbleEngineClientInit(&app->nimbleEngineClient, setup);

    CLOG_DEBUG("nimble client is setup with transport")

    NimbleEngineClientGameJoinOptions joinOptions;
    joinOptions.playerCount = useLocalPlayerCount;
    joinOptions.players[0].localIndex = 99;
    joinOptions.players[1].localIndex = 42;
    nimbleEngineClientRequestJoin(&app->nimbleEngineClient, joinOptions);

    app->nimbleEngineClient.isHostingLocally = app->isHosting;

    CLOG_DEBUG("nimble client is trying to join / rejoin server")
}

static void serverConsumeAllDatagrams(UdpServerConnections* udpServerWrapper, NbdServer* nimbleServer)
{
    int connectionId;
    uint8_t datagram[1200];
    UdpTransportOut responseTransport;

    for (size_t i = 0; i < 32; ++i) {
        bool didConnectNow = false;
        int octetCountReceived = udpServerConnectionsReceive(udpServerWrapper, &connectionId, datagram, 1200,
                                                             &didConnectNow, &responseTransport);

        if (octetCountReceived == 0) {
            return;
        }
        if (didConnectNow) {
            nbdServerConnectionConnected(nimbleServer, connectionId);
        }
        NbdResponse response;
        response.transportOut = &responseTransport;
        int errorCode = nbdServerFeed(nimbleServer, connectionId, datagram, octetCountReceived, &response);
        if (errorCode < 0) {
            CLOG_ERROR("can not feed server")
        }
    }
}

int main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;

    g_clog.log = clog_console;
    g_clog.level = CLOG_TYPE_DEBUG;
    CLOG_VERBOSE("Nimble Ball start!")

    ImprintDefaultSetup imprintDefaultSetup;
    imprintDefaultSetupInit(&imprintDefaultSetup, 5 * 1024 * 1024);

    NlCombinedRender combinedRender;
    nlFrontendInit(&combinedRender.frontend);

    SrGamepad gamepads[2];

    srGamepadInit(&gamepads[0]);
    srGamepadInit(&gamepads[1]);

    NlApp app;

    statsIntPerSecondInit(&app.renderFps, monotonicTimeMsNow(), 1000);

    srWindowInit(&app.window, 640, 360, "nimble ball");

    srAudioInit(&app.mixer);

    nlAudioInit(&app.audio, &app.mixer);

    nlRenderInit(&combinedRender.inGame, app.window.renderer);
    nlFrontendRenderInit(&combinedRender.frontendRender, &app.window, combinedRender.inGame.font);

    app.phase = NlAppPhaseIdle;
    app.isHosting = false;

    Clog authoritativeLog;
    authoritativeLog.constantPrefix = "NimbleBallAuth";
    authoritativeLog.config = &g_clog;
    nlSimulationVmInit(&app.authoritative, authoritativeLog);

    Clog predictedLog;
    predictedLog.constantPrefix = "NimbleBallPredicted";
    predictedLog.config = &g_clog;
    nlSimulationVmInit(&app.predicted, predictedLog);

    bool menuPressedLast = false;

    while (true) {
        int wantsToQuit = srGamepadPoll(gamepads, maxLocalPlayerCount);
        if (wantsToQuit == 1) {
            break;
        }
        if (!menuPressedLast && gamepads[0].menu) {
            if (combinedRender.inGame.mode == NlRenderModeAuthoritative) {
                CLOG_NOTICE("TOGGLE: PREDICTED!")
                combinedRender.inGame.mode = NlRenderModePredicted;
            } else {
                CLOG_NOTICE("TOGGLE: AUTHORITATIVE!")
                combinedRender.inGame.mode = NlRenderModeAuthoritative;
            }
        }
        menuPressedLast = gamepads[0].menu;

        nlFrontendHandleInput(&combinedRender.frontend, &gamepads[0]);
        switch (app.phase) {
            case NlAppPhaseIdle:
                switch (combinedRender.frontend.mainMenuSelected) {
                    case NlFrontendMenuSelectJoin:
                        CLOG_DEBUG("Join a game")
                        startJoining(&app, &combinedRender.frontend, &imprintDefaultSetup.tagAllocator.info,
                                     &imprintDefaultSetup.slabAllocator.info);
                        break;
                    case NlFrontendMenuSelectHost:
                        CLOG_DEBUG("Host a game")
                        startHosting(&app, &combinedRender.frontend, &imprintDefaultSetup.tagAllocator.info,
                                     &imprintDefaultSetup.slabAllocator.info);
                        startJoining(&app, &combinedRender.frontend, &imprintDefaultSetup.tagAllocator.info,
                                     &imprintDefaultSetup.slabAllocator.info);
                        break;
                    case NlFrontendMenuSelectUnknown:
                    default:
                        break;
                }
                break;

            case NlAppPhaseNetwork: {
                hazyDatagramTransportInOutUpdate(&app.hazyClientTransport);
                nimbleEngineClientUpdate(&app.nimbleEngineClient);
                if (app.nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
                    app.nimbleEngineClient.nimbleClient.client.localParticipantCount > 0 &&
                    nimbleEngineClientMustAddPredictedInput(&app.nimbleEngineClient)) {
                    NlPlayerInput inputs[2];
                    inputs[0] = gamepadToPlayerInput(&gamepads[0]);
                    inputs[1] = gamepadToPlayerInput(&gamepads[1]);

                    uint8_t participantId[2];

                    participantId[0] = app.nimbleEngineClient.nimbleClient.client.localParticipantLookup[0]
                                           .participantId;
                    TransmuteParticipantInput participantInputs[2];
                    participantInputs[0].input = &inputs[0];
                    participantInputs[0].octetSize = sizeof(inputs[0]);
                    participantInputs[0].participantId = participantId[0];

                    participantId[1] = app.nimbleEngineClient.nimbleClient.client.localParticipantLookup[1]
                                           .participantId;
                    participantInputs[1].input = &inputs[1];
                    participantInputs[1].octetSize = sizeof(inputs[1]);
                    participantInputs[1].participantId = participantId[1];

                    TransmuteInput transmuteInput;
                    transmuteInput.participantInputs = participantInputs;
                    transmuteInput.participantCount = useLocalPlayerCount;

                    nimbleEngineClientAddPredictedInput(&app.nimbleEngineClient, &transmuteInput);
                }

                if (app.isHosting) {
                    nbdServerUpdate(&app.nimbleServer, monotonicTimeMsNow());
                    serverConsumeAllDatagrams(&app.udpServerWrapper, &app.nimbleServer);

                    if (app.nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
                        nbdServerMustProvideGameState(&app.nimbleServer)) {
                        StepId outStepId;
                        TransmuteState authoritativeState = assentGetState(
                            &app.nimbleEngineClient.rectify.authoritative, &outStepId);
                        CLOG_ASSERT(authoritativeState.octetSize == sizeof(NlGame), "illegal authoritative state");
                        nbdServerSetGameState(&app.nimbleServer, authoritativeState.state, authoritativeState.octetSize,
                                              outStepId);
                    }
                }
            }

            break;
        }

        NlRenderStats renderStats;
        if (app.phase == NlAppPhaseNetwork && app.nimbleEngineClient.phase == NimbleEngineClientPhaseSynced) {
            NimbleGameState authoritativeState;
            NimbleGameState predictedState;

            nimbleEngineClientGetGameStates(&app.nimbleEngineClient, &authoritativeState, &predictedState);

            renderStats.authoritativeTickId = authoritativeState.tickId;
            renderStats.predictedTickId = predictedState.tickId;

            combinedRender.authoritative = (const NlGame*) authoritativeState.state.state;
            combinedRender.predicted = (const NlGame*) predictedState.state.state;

            CLOG_ASSERT(authoritativeState.state.octetSize == sizeof(NlGame), "internal error, wrong auth state size");
            CLOG_ASSERT(predictedState.state.octetSize == sizeof(NlGame), "internal error, wrong state size");

            NimbleEngineClientStats stats;
            nimbleEngineClientGetStats(&app.nimbleEngineClient, &stats);

            renderStats.authoritativeStepsInBuffer = stats.authoritativeBufferDeltaStat;
        } else {
            combinedRender.authoritative = 0;
            combinedRender.predicted = 0;

            renderStats.predictedTickId = 0;
            renderStats.authoritativeTickId = 0;
            renderStats.authoritativeStepsInBuffer = 0;
        }

        renderStats.renderFps = app.renderFps.avg;
        renderStats.latencyMs = app.nimbleEngineClient.nimbleClient.client.latencyMsStat.avg;
        combinedRender.renderStats = renderStats;

        srWindowRender(&app.window, 0x115511, &combinedRender, renderCallback);
        statsIntPerSecondAdd(&app.renderFps, 1);
        statsIntPerSecondUpdate(&app.renderFps, monotonicTimeMsNow());
        if (combinedRender.authoritative != 0 && combinedRender.predicted != 0) {
            nlAudioUpdate(&app.audio, combinedRender.authoritative, combinedRender.predicted, 0, 0U);
        }
    }

    nlRenderClose(&combinedRender.inGame);

    srAudioClose(&app.mixer);
    srWindowClose(&app.window);
}
