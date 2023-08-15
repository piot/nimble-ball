/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include "frontend_render.h"
#include "lagometer_render.h"
#include "network_icons_render.h"
#include <clog/console.h>
#include <imprint/default_setup.h>
#include <nimble-ball-presentation/audio.h>
#include <nimble-ball-presentation/render.h>
#include <nimble-ball-simulation/nimble_ball_simulation_vm.h>
#include <nimble-engine-client/client.h>
#include <nimble-server/participant_connection.h>
#include <nimble-server/server.h>
#include <transport-stack/multi.h>
#include <transport-stack/single.h>

static const size_t gameRelayPort = 27003U;
static const char* gameRelayHost = "127.0.0.1";
// static const char* gameRelayDevHost = "gamerelay.dev";

clog_config g_clog;

char g_clog_temp_str[CLOG_TEMP_STR_SIZE];

static NlPlayerInput gamepadToPlayerInput(const SrGamepad* pad)
{
    NlPlayerInput playerInput;
    playerInput.input.inGameInput.horizontalAxis = (int8_t) pad->horizontalAxis;
    playerInput.input.inGameInput.verticalAxis = (int8_t) pad->verticalAxis;
    uint8_t mask = pad->a ? 0x01 : 0x00 | pad->b ? 0x02 : 0x00;
    playerInput.input.inGameInput.buttons = mask;
    playerInput.inputType = NlPlayerInputTypeInGame;
    return playerInput;
}

typedef enum NlAppPhase {
    NlAppPhaseIdle,
    NlAppPhaseNetwork,
} NlAppPhase;

/// Shared resources
typedef struct NlApp {
    NlAppPhase phase;
    ImprintAllocator* allocator;
    ImprintAllocatorWithFree* allocatorWithFree;
    Clog log;
    NlSimulationVm authoritative;
    NlSimulationVm predicted;
    NlFrontend frontend;
    bool nimbleServerIsStarted;
} NlApp;

/// Nimble server and transport stack
typedef struct NlAppHost {
    NimbleServer nimbleServer;
    TransportStackMulti multiTransport;
    Clog log;
} NlAppHost;

/// Nimble client, transport stack and presentation
typedef struct NlAppClient {
    SrGamepad gamepads[2];
    SrFunctionKeys functionKeys;
    SrWindow window;
    NlRender inGame;
    NlFrontendRender frontendRender;
    NlLagometerRender lagometerRender;
    NlNetworkIconsRender networkIconsRender;
    StatsIntPerSecond renderFps;
    SrAudio mixer;
    NlAudio audio;
    TransportStackSingle singleTransport;
    ImprintAllocator* allocator;
    ImprintAllocatorWithFree* allocatorWithFree;
    NimbleEngineClient nimbleEngineClient;
    Clog log;
    SrFunctionKeys functionKeysPressedLast;
} NlAppClient;

/// Initializes the nimble server on the previously setup multi transport
/// @param self appHost
/// @param app Application
static void startHostingOnMultiTransport(NlAppHost* self, NlApp* app)
{
    app->phase = NlAppPhaseNetwork;
    app->frontend.phase = NlFrontendPhaseHosting;

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
    serverLog.constantPrefix = "NimbleServer";

    NimbleServerSetup serverSetup;
    serverSetup.maxSingleParticipantStepOctetCount = maxSingleParticipantStepOctetCount;
    serverSetup.maxParticipantCount = maxParticipantCount;
    serverSetup.maxConnectionCount = maxConnectionCount;
    serverSetup.maxParticipantCountForEachConnection = 2;
    serverSetup.maxGameStateOctetCount = sizeof(NlGame);
    serverSetup.memory = app->allocator;
    serverSetup.applicationVersion = serverReportTransmuteVmVersion;
    serverSetup.now = monotonicTimeMsNow();
    serverSetup.log = serverLog;
    serverSetup.multiTransport = self->multiTransport.multiTransport;
    int errorCode = nimbleServerInit(&self->nimbleServer, serverSetup);
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
    nimbleServerReInitWithGame(&self->nimbleServer, (const uint8_t*) &initialServerState, sizeof(initialServerState),
                               stepId, monotonicTimeMsNow());

    CLOG_INFO("nimble server has initial game state. octet count: %zu", self->nimbleServer.game.latestState.octetCount)
    app->nimbleServerIsStarted = true;
}

static const int maxLocalPlayerCount = 2;
static const int useLocalPlayerCount = 1;

/// Initializes a nimble engine client on a previously setup single datagram transport
/// @param self app client
/// @param app application
static void startJoiningOnClientTransport(NlAppClient* self, NlApp* app)
{
    CLOG_DEBUG("start joining")
    app->phase = NlAppPhaseNetwork;
    app->frontend.phase = NlFrontendPhaseJoining;

    CLOG_DEBUG("client datagram transport is set")

    NimbleSerializeVersion clientReportTransmuteVmVersion = {
        app->authoritative.transmuteVm.version.major,
        app->authoritative.transmuteVm.version.minor,
        app->authoritative.transmuteVm.version.patch,
    };

    NimbleEngineClientSetup setup;
    setup.memory = app->allocator;
    setup.blobMemory = app->allocatorWithFree;
    setup.transport = self->singleTransport.singleTransport;

    setup.authoritative = app->authoritative.transmuteVm;
    setup.predicted = app->predicted.transmuteVm;
    setup.maximumSingleParticipantStepOctetCount = sizeof(NlPlayerInput);
    setup.maximumParticipantCount = 8;
    setup.applicationVersion = clientReportTransmuteVmVersion;
    setup.maxTicksFromAuthoritative = 10U;

    Clog nimbleEngineClientLog;
    nimbleEngineClientLog.config = &g_clog;
    nimbleEngineClientLog.constantPrefix = "NimbleEngineClient";

    setup.log = nimbleEngineClientLog;
    nimbleEngineClientInit(&self->nimbleEngineClient, setup);

    CLOG_DEBUG("nimble client is setup with transport")

    NimbleEngineClientGameJoinOptions joinOptions;
    joinOptions.playerCount = useLocalPlayerCount;
    joinOptions.players[0].localIndex = 99;
    joinOptions.players[1].localIndex = 42;
    nimbleEngineClientRequestJoin(&self->nimbleEngineClient, joinOptions);

    // self->nimbleEngineClient.isHostingLocally = app->nimbleServerIsStarted;

    CLOG_DEBUG("nimble client is trying to join / rejoin server")
}

/*
/// Tries to create a room on a conclave transport
/// @note not implemented yet
/// @param app Application
/// @param host Host
/// @return zero
static int startHostOnline(NlApp* app, NlAppHost* host)
{
    (void) host;

    app->phase = NlAppPhaseNetwork;
    app->frontend.phase = NlFrontendPhaseHostingOnline;

    return 0;
}

/// Tries to join an existing room on a conclave transport
/// @note not implemented yet
/// @param app app
/// @param client appclient
/// @return zero
static int startJoinOnline(NlApp* app, NlAppClient* client)
{
    (void) client;
    app->phase = NlAppPhaseNetwork;
    app->frontend.phase = NlFrontendPhaseHostingOnline;

    return 0;
}
*/


/// Initializes a multi datagram transport stack
/// Used by the server
/// @param multi transport stack
/// @param mode which transport stack to use
/// @param allocator allocator
/// @param allocatorWithFree allocator with free
static void initializeTransportStackMulti(TransportStackMulti* multi, TransportStackMode mode,
                                          struct ImprintAllocator* allocator,
                                          struct ImprintAllocatorWithFree* allocatorWithFree)
{
    Clog multiLog;
    multiLog.config = &g_clog;
    multiLog.constantPrefix = "multi";

    transportStackMultiInit(multi, allocator, allocatorWithFree, mode, multiLog);
}

/// Initializes a single datagram transport stack
/// Used by the client only
/// @param single single transport stack
/// @param mode wich mode
/// @param allocator allocator
/// @param allocatorWithFree allocatorWithFree
static void initializeTransportStackSingle(TransportStackSingle* single, TransportStackMode mode,
                                           struct ImprintAllocator* allocator,
                                           struct ImprintAllocatorWithFree* allocatorWithFree)
{
    Clog singleLog;
    singleLog.config = &g_clog;
    singleLog.constantPrefix = "single";

    transportStackSingleInit(single, allocator, allocatorWithFree, mode, singleLog);
}

static void initializeConnectMultiAndHost(NlApp* app, NlAppHost* host, const char* hostname, uint16_t port,
                                          TransportStackMode transportStackMode, struct ImprintAllocator* allocator,
                                          struct ImprintAllocatorWithFree* allocatorWithFree)
{
    initializeTransportStackMulti(&host->multiTransport, transportStackMode, allocator, allocatorWithFree);
    transportStackMultiListen(&host->multiTransport, hostname, port);
    startHostingOnMultiTransport(host, app);
}

/// Handles menu selection when not actively trying to create, play or join a game
/// @param app
/// @param host
/// @param client
static void updateFrontendInIdle(NlApp* app, NlAppHost* host, NlAppClient* client)
{
    switch (app->frontend.mainMenuSelected) {
        case NlFrontendMenuSelectJoin:
            CLOG_DEBUG("Join a LAN game")
            initializeTransportStackSingle(&client->singleTransport, TransportStackModeLocalUdp, app->allocator,
                                           app->allocatorWithFree);
            transportStackSingleConnect(&client->singleTransport, gameRelayHost, gameRelayPort, 0);
            startJoiningOnClientTransport(client, app);
            break;
        case NlFrontendMenuSelectHost:
            CLOG_DEBUG("Host a LAN game")
            initializeConnectMultiAndHost(app, host, "", gameRelayPort, TransportStackModeLocalUdp, app->allocator,
                                          app->allocatorWithFree);
            initializeTransportStackSingle(&client->singleTransport, TransportStackModeLocalUdp, app->allocator,
                                           app->allocatorWithFree);
            transportStackSingleConnect(&client->singleTransport, gameRelayHost, gameRelayPort, 0);
            startJoiningOnClientTransport(client, app);
            break;
        case NlFrontendMenuSelectHostOnline:
        case NlFrontendMenuSelectJoinOnline:
            break;
            /*
        case NlFrontendMenuSelectHostOnline:
            CLOG_DEBUG("Host Online")
            initializeConnectMultiAndHost(app, host, gameRelayDevHost, gameRelayPort, TransportStackModeConclave,
                                          app->allocator, app->allocatorWithFree);
            startHostOnline(app, host);

            initializeTransportStackSingle(&client->singleTransport, TransportStackModeConclave, app->allocator,
                                           app->allocatorWithFree);
            transportStackSingleConnect(&client->singleTransport, gameRelayDevHost, gameRelayPort);
            break;
        case NlFrontendMenuSelectJoinOnline:
            CLOG_DEBUG("Join an Online game")
            initializeTransportStackSingle(&client->singleTransport, TransportStackModeConclave, app->allocator,
                                           app->allocatorWithFree);
            transportStackSingleConnect(&client->singleTransport, gameRelayDevHost, gameRelayPort);
            startJoiningOnClientTransport(client, app);
            startJoinOnline(app, client);
            break;
             */
        case NlFrontendMenuSelectUnknown:
            break;
    }
}

/// Adds predicted input to the nimble engine client
/// @param client
static void addPredictedInput(NlAppClient* client)
{
    NlPlayerInput inputs[NLR_MAX_LOCAL_PLAYERS];
    uint8_t participantId[NLR_MAX_LOCAL_PLAYERS];
    TransmuteParticipantInput participantInputs[2];

    StepId outStepId;
    TransmuteState authoritativeState = assentGetState(&client->nimbleEngineClient.rectify.authoritative, &outStepId);

    const NlGame* authoritative = (const NlGame*) authoritativeState.state;
    for (size_t i = 0U; i < useLocalPlayerCount; ++i) {
        participantId[i] = client->nimbleEngineClient.nimbleClient.client.localParticipantLookup[i].participantId;
        NlrLocalPlayer* renderLocalPlayer = nlRenderFindLocalPlayerFromParticipantId(&client->inGame, participantId[i]);
        const NlPlayer* simulationPlayer = nlGameFindSimulationPlayerFromParticipantId(authoritative, participantId[i]);
        if (simulationPlayer != 0 && simulationPlayer->phase == NlPlayerPhaseSelectTeam && renderLocalPlayer != NULL &&
            renderLocalPlayer->selectedTeamIndex != NL_TEAM_UNDEFINED) {
            inputs[i].inputType = NlPlayerInputTypeSelectTeam;
            inputs[i].input.selectTeam.preferredTeamToJoin = (uint8_t) renderLocalPlayer->selectedTeamIndex;
            CLOG_INFO("sent selected team %d", inputs[i].input.selectTeam.preferredTeamToJoin)
        } else {
            inputs[i] = gamepadToPlayerInput(&client->gamepads[0]);
        }
        participantInputs[i].input = &inputs[i];
        participantInputs[i].octetSize = sizeof(inputs[i]);
        participantInputs[i].participantId = participantId[i];
    }

    TransmuteInput transmuteInput;
    transmuteInput.participantInputs = participantInputs;
    transmuteInput.participantCount = useLocalPlayerCount;

    nimbleEngineClientAddPredictedInput(&client->nimbleEngineClient, &transmuteInput);
}

/// Sets the locally simulated game state to the local nimble server
/// @param host
/// @param client
static void setGameStateToHost(NlAppHost* host, NlAppClient* client)
{
    StepId outStepId;
    TransmuteState authoritativeState = assentGetState(&client->nimbleEngineClient.rectify.authoritative, &outStepId);
    CLOG_ASSERT(authoritativeState.octetSize == sizeof(NlGame), "illegal authoritative state")
    nimbleServerSetGameState(&host->nimbleServer, authoritativeState.state, authoritativeState.octetSize, outStepId);
}

/// Update host
/// @param host
/// @param client
static void updateHost(NlAppHost* host, NlAppClient* client)
{
    transportStackMultiUpdate(&host->multiTransport);
    nimbleServerUpdate(&host->nimbleServer, monotonicTimeMsNow());

    if (client->nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
        nimbleServerMustProvideGameState(&host->nimbleServer)) {
        setGameStateToHost(host, client);
    }
}

/// Update nimble engine client and, if hosting, the nimble engine server
/// @param app
/// @param host
/// @param client
static void updateInNetwork(NlApp* app, NlAppHost* host, NlAppClient* client)
{
    transportStackSingleUpdate(&client->singleTransport);

    if (transportStackSingleIsConnected(&client->singleTransport)) {
        nimbleEngineClientUpdate(&client->nimbleEngineClient);
    } else {
        uint8_t buf[1200];
        datagramTransportReceive(&client->singleTransport.singleTransport, buf, 1200);
    }
    if (client->nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
        client->nimbleEngineClient.nimbleClient.client.localParticipantCount > 0 &&
        nimbleEngineClientMustAddPredictedInput(&client->nimbleEngineClient)) {
        addPredictedInput(client);
    }

    if (app->nimbleServerIsStarted) {
        updateHost(host, client);
    }
}

/// Presents the authoritative and predicted state (if available) and the front end.
/// @param app
/// @param client
static void presentPredictedAndAuthoritativeStatesAndFrontend(const NlApp* app, NlAppClient* client)
{
    NlRenderStats renderStats;
    const NlGame* authoritative;
    const NlGame* predicted;

    if (app->phase == NlAppPhaseNetwork && client->nimbleEngineClient.phase == NimbleEngineClientPhaseSynced) {
        NimbleGameState authoritativeState;
        NimbleGameState predictedState;

        nimbleEngineClientGetGameStates(&client->nimbleEngineClient, &authoritativeState, &predictedState);

        renderStats.authoritativeTickId = authoritativeState.tickId;
        renderStats.predictedTickId = predictedState.tickId;

        authoritative = (const NlGame*) authoritativeState.state.state;
        predicted = (const NlGame*) predictedState.state.state;

        CLOG_ASSERT(authoritativeState.state.octetSize == sizeof(NlGame), "internal error, wrong auth state size");
        CLOG_ASSERT(predictedState.state.octetSize == sizeof(NlGame), "internal error, wrong state size");

        NimbleEngineClientStats stats;
        nimbleEngineClientGetStats(&client->nimbleEngineClient, &stats);

        renderStats.authoritativeStepsInBuffer = stats.authoritativeBufferDeltaStat;
    } else {
        authoritative = 0;
        predicted = 0;

        renderStats.predictedTickId = 0;
        renderStats.authoritativeTickId = 0;
        renderStats.authoritativeStepsInBuffer = 0;
    }

    renderStats.renderFps = client->renderFps.avg;
    renderStats.latencyMs = client->nimbleEngineClient.nimbleClient.client.latencyMsStat.avg;

    srWindowRenderPrepare(&client->window, 0x115511);
    if (authoritative != NULL && predicted != NULL) {
        nlAudioUpdate(&client->audio, authoritative, predicted, 0, 0U);
        uint8_t localParticipantIds[4];
        const NimbleClient* nimbleClient = &client->nimbleEngineClient.nimbleClient.client;
        for (size_t i = 0; i < nimbleClient->localParticipantCount; ++i) {
            localParticipantIds[i] = nimbleClient->localParticipantLookup[i].participantId;
        }
        nlRenderFeedInput(&client->inGame, client->gamepads, predicted, localParticipantIds,
                          nimbleClient->localParticipantCount);

        nlRenderUpdate(&client->inGame, authoritative, predicted, localParticipantIds,
                       nimbleClient->localParticipantCount, renderStats);
        nlLagometerRenderUpdate(&client->lagometerRender, &client->nimbleEngineClient.nimbleClient.client.lagometer);
    }

    nlFrontendRenderUpdate(&client->frontendRender, &app->frontend);

    NlNetworkIconsState iconsState;
    iconsState.authoritativeTimeIntervalWarning = client->nimbleEngineClient.detectedGapInAuthoritativeSteps
                                                      .isOrWasTrue;
    iconsState.droppedDatagram = client->nimbleEngineClient.nimbleClient.client.droppingDatagramWarning.isOrWasTrue;
    iconsState.disconnectInfo = NlNetworkIconsDisconnectInfoNone;
    if (client->nimbleEngineClient.nimbleClient.state == NimbleClientRealizeStateDisconnected) {
        iconsState.disconnectInfo = NlNetworkIconsDisconnectDisconnected;
    } else {
        bool impending = client->nimbleEngineClient.nimbleClient.client.impendingDisconnectWarning.isOrWasTrue |
                         client->nimbleEngineClient.bigGapInAuthoritativeSteps.isOrWasTrue;
        if (impending) {
            iconsState.disconnectInfo = NlNetworkIconsDisconnectImpending;
        }
    }
    nlNetworkIconsRenderUpdate(&client->networkIconsRender, iconsState);

    srWindowRenderPresent(&client->window);
}

/// Polls the gamepad and handle special function buttons
/// Gamepads are currently they keyboard keys [w,a,s,d,space,left-shift] and [i,j,k,l,h]
/// @param client
/// @return true if the app should continue to run, false otherwise
static bool pollInputAndHandleSpecialButtons(NlAppClient* client)
{
    int wantsToQuit = srGamepadPoll(client->gamepads, maxLocalPlayerCount, &client->functionKeys);
    if (wantsToQuit == 1) {
        return false;
    }

    if (!client->functionKeysPressedLast.functionKeys[SR_KEY_F2] && client->functionKeys.functionKeys[SR_KEY_F2]) {
        if (client->inGame.mode == NlRenderModeAuthoritative) {
            CLOG_NOTICE("TOGGLE: PREDICTED!")
            client->inGame.mode = NlRenderModePredicted;
        } else {
            CLOG_NOTICE("TOGGLE: AUTHORITATIVE!")
            client->inGame.mode = NlRenderModeAuthoritative;
        }
    }

    if (!client->functionKeysPressedLast.functionKeys[SR_KEY_F3] && client->functionKeys.functionKeys[SR_KEY_F3]) {
        TransportStackInternetSimulationMode
            newMode = (TransportStackInternetSimulationMode) (((int) client->singleTransport.lowerLevel
                                                                   .internetSimulationMode +
                                                               1) %
                                                              3);

        transportStackSingleSetInternetSimulationMode(&client->singleTransport, newMode);
        CLOG_NOTICE("internet simulation mode: %d", newMode)
    }

    if (!client->functionKeysPressedLast.functionKeys[SR_KEY_F4] && client->functionKeys.functionKeys[SR_KEY_F4]) {
        hazyDatagramTransportDebugDiscardIncoming(&client->singleTransport.lowerLevel.hazyTransport);
        CLOG_NOTICE("stopping incoming hazy transport");
    }

    client->functionKeysPressedLast = client->functionKeys;

    return true;
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

    // App Initialization
    NlApp app;
    nlFrontendInit(&app.frontend);
    app.phase = NlAppPhaseIdle;
    app.nimbleServerIsStarted = false;
    app.allocator = &imprintDefaultSetup.tagAllocator.info;
    app.allocatorWithFree = &imprintDefaultSetup.slabAllocator.info;
    app.log.config = &g_clog;
    app.log.constantPrefix = "App";

    Clog authoritativeLog;
    authoritativeLog.constantPrefix = "NimbleBallAuth";
    authoritativeLog.config = &g_clog;
    nlSimulationVmInit(&app.authoritative, authoritativeLog);

    Clog predictedLog;
    predictedLog.constantPrefix = "NimbleBallPredicted";
    predictedLog.config = &g_clog;
    nlSimulationVmInit(&app.predicted, predictedLog);

    // Client Initialization
    NlAppClient client;
    srGamepadInit(&client.gamepads[0]);
    srGamepadInit(&client.gamepads[1]);
    srFunctionKeysInit(&client.functionKeysPressedLast);
    srFunctionKeysInit(&client.functionKeys);

    statsIntPerSecondInit(&client.renderFps, monotonicTimeMsNow(), 1000);
    srWindowInit(&client.window, 640, 360, "nimble ball");
    srAudioInit(&client.mixer);
    nlAudioInit(&client.audio, &client.mixer);
    nlRenderInit(&client.inGame, client.window.renderer);
    nlFrontendRenderInit(&client.frontendRender, &client.window, client.inGame.font);
    nlLagometerRenderInit(&client.lagometerRender, &client.window, client.inGame.font, &client.inGame.rectangleRender);
    nlNetworkIconsRenderInit(&client.networkIconsRender, &client.inGame.spriteRender,
                             client.inGame.jerseySprite[0].texture);
    client.log = app.log;

    // Host Initialization
    NlAppHost host;

    while (pollInputAndHandleSpecialButtons(&client)) {
        nlFrontendHandleInput(&app.frontend, &client.gamepads[0]);
        switch (app.phase) {
            case NlAppPhaseIdle:
                updateFrontendInIdle(&app, &host, &client);
                break;

            case NlAppPhaseNetwork: {
                updateInNetwork(&app, &host, &client);
            } break;
        }

        presentPredictedAndAuthoritativeStatesAndFrontend(&app, &client);

        statsIntPerSecondAdd(&client.renderFps, 1);
        statsIntPerSecondUpdate(&client.renderFps, monotonicTimeMsNow());
    }

    nlRenderClose(&client.inGame);
    srAudioClose(&client.mixer);
    srWindowClose(&client.window);
}
