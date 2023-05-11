/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include "frontend_render.h"
#include "lagometer_render.h"
#include <clog/console.h>
#include <conclave-client/client.h>
#include <conclave-client/network_realizer.h>
#include <imprint/default_setup.h>
#include <nimble-ball-presentation/audio.h>
#include <nimble-ball-presentation/render.h>
#include <nimble-ball-simulation/nimble_ball_simulation_vm.h>
#include <nimble-engine-client/client.h>
#include <nimble-server/participant_connection.h>
#include <nimble-server/server.h>
#include <sdl-render/gamepad.h>
#include <transport-stack/multi.h>
#include <transport-stack/single.h>

const size_t gameRelayPort = 27003U;
const char* gameRelayHost = "127.0.0.1";

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
    bool nbdServerIsStarted;
} NlApp;

/// Nimble server and transport stack
typedef struct NlAppHost {
    NbdServer nimbleServer;
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
/// @param self
/// @param app
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
    serverLog.constantPrefix = "NbdServer";

    NbdServerSetup serverSetup;
    serverSetup.maxSingleParticipantStepOctetCount = maxSingleParticipantStepOctetCount;
    serverSetup.maxParticipantCount = maxParticipantCount;
    serverSetup.maxConnectionCount = maxConnectionCount;
    serverSetup.maxParticipantCountForEachConnection = 2;
    serverSetup.maxGameStateOctetCount = sizeof(NlGame);
    serverSetup.memory = app->allocator;
    serverSetup.blobAllocator = app->allocatorWithFree;
    serverSetup.applicationVersion = serverReportTransmuteVmVersion;
    serverSetup.now = monotonicTimeMsNow();
    serverSetup.log = serverLog;
    serverSetup.multiTransport = self->multiTransport.multiTransport;
    int errorCode = nbdServerInit(&self->nimbleServer, serverSetup);
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
    nbdServerReInitWithGame(&self->nimbleServer, (const uint8_t*) &initialServerState, sizeof(initialServerState),
                            stepId, monotonicTimeMsNow());

    CLOG_INFO("nimble server has initial game state. octet count: %zu", self->nimbleServer.game.latestState.octetCount)
    app->nbdServerIsStarted = true;
}

const int maxLocalPlayerCount = 2;
const int useLocalPlayerCount = 1;

/// Initializes a nimble engine client on a previously setup single datagram transport
/// @param self
/// @param app
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

    self->nimbleEngineClient.isHostingLocally = app->nbdServerIsStarted;

    CLOG_DEBUG("nimble client is trying to join / rejoin server")
}

/// Tries to create a room on a conclave transport
/// @note not implemented yet
/// @param app
/// @param host
/// @return
static int startHostOnline(NlApp* app, NlAppHost* host)
{
    if (host->multiTransport.mode == TransportStackModeConclave) {
        ClvSerializeRoomCreateOptions createRoomOptions;
        createRoomOptions.name = "New Room";
        createRoomOptions.maxNumberOfPlayers = 4;
        createRoomOptions.flags = 0;
        clvClientRealizeCreateRoom(&host->multiTransport.conclave.conclaveClient, &createRoomOptions);
    }

    app->phase = NlAppPhaseNetwork;
    app->frontend.phase = NlFrontendPhaseHostingOnline;

    return 0;
}

/// Initializes a multi datagram transport stack
/// Used by the server
/// @param multi
/// @param mode
static void initializeTransportStackMulti(TransportStackMulti* multi, TransportStackMode mode)
{
    Clog multiLog;
    multiLog.config = &g_clog;
    multiLog.constantPrefix = "multi";

    transportStackMultiInit(multi, mode, multiLog);
}

/// Initializes a single datagram transport stack
/// Used by the client only
/// @param single
/// @param mode
/// @param allocator
/// @param allocatorWithFree
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
                                          TransportStackMode transportStackMode)
{
    initializeTransportStackMulti(&host->multiTransport, transportStackMode);
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
            transportStackSingleConnect(&client->singleTransport, gameRelayHost, gameRelayPort);
            startJoiningOnClientTransport(client, app);
            break;
        case NlFrontendMenuSelectHost:
            CLOG_DEBUG("Host a LAN game")
            initializeConnectMultiAndHost(app, host, "", gameRelayPort, TransportStackModeLocalUdp);
            initializeTransportStackSingle(&client->singleTransport, TransportStackModeLocalUdp, app->allocator,
                                           app->allocatorWithFree);
            transportStackSingleConnect(&client->singleTransport, gameRelayHost, gameRelayPort);
            startJoiningOnClientTransport(client, app);
            break;
        case NlFrontendMenuSelectHostOnline:
            CLOG_DEBUG("Host Online")
            initializeConnectMultiAndHost(app, host, gameRelayHost, gameRelayPort, TransportStackModeConclave);
            break;
        case NlFrontendMenuSelectUnknown:
        default:
            break;
    }
}

/// Not implemented yet
/// @param app
/// @param host
/// @param client
static void updateConclave(NlApp* app, NlAppHost* host, NlAppClient* client)
{
    ClvClientRealize* conclaveClient = &client->singleTransport.conclave.conclaveClient;
    if (conclaveClient->state == ClvClientRealizeStateCreateRoom && !app->nbdServerIsStarted) {
        startHostingOnMultiTransport(host, app);
        startJoiningOnClientTransport(client, app);
    }
    if (app->frontend.phase == NlFrontendPhaseHostingOnline &&
        conclaveClient->state == ClvClientRealizeStateCreateRoom) {
        // We have created a room
        app->frontend.phase = NlFrontendPhaseInGame;
    }
}

/// Adds predicted input to the nimble engine client
/// @param client
static void addPredictedInput(NlAppClient* client)
{
    NlPlayerInput inputs[2];
    inputs[0] = gamepadToPlayerInput(&client->gamepads[0]);
    inputs[1] = gamepadToPlayerInput(&client->gamepads[1]);

    uint8_t participantId[2];

    participantId[0] = client->nimbleEngineClient.nimbleClient.client.localParticipantLookup[0].participantId;
    TransmuteParticipantInput participantInputs[2];
    participantInputs[0].input = &inputs[0];
    participantInputs[0].octetSize = sizeof(inputs[0]);
    participantInputs[0].participantId = participantId[0];

    participantId[1] = client->nimbleEngineClient.nimbleClient.client.localParticipantLookup[1].participantId;
    participantInputs[1].input = &inputs[1];
    participantInputs[1].octetSize = sizeof(inputs[1]);
    participantInputs[1].participantId = participantId[1];

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
    CLOG_ASSERT(authoritativeState.octetSize == sizeof(NlGame), "illegal authoritative state");
    nbdServerSetGameState(&host->nimbleServer, authoritativeState.state, authoritativeState.octetSize, outStepId);
}

/// Update host
/// @param host
/// @param client
static void updateHost(NlAppHost* host, NlAppClient* client)
{
    nbdServerUpdate(&host->nimbleServer, monotonicTimeMsNow());

    if (client->nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
        nbdServerMustProvideGameState(&host->nimbleServer)) {
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
    if (client->singleTransport.mode == TransportStackModeConclave) {
        updateConclave(app, host, client);
    }
    if (transportStackSingleIsConnected(&client->singleTransport)) {
        nimbleEngineClientUpdate(&client->nimbleEngineClient);
    } else {
        uint8_t buf[1200];
        udpTransportReceive(&client->singleTransport.singleTransport, buf, 1200);
    }
    if (client->nimbleEngineClient.phase == NimbleEngineClientPhaseSynced &&
        client->nimbleEngineClient.nimbleClient.client.localParticipantCount > 0 &&
        nimbleEngineClientMustAddPredictedInput(&client->nimbleEngineClient)) {
        addPredictedInput(client);
    }

    if (app->nbdServerIsStarted) {
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
        for (size_t i = 0; i<nimbleClient->localParticipantCount; ++i) {
            localParticipantIds[i] = nimbleClient->localParticipantLookup[i].participantId;
        }
        nlRenderUpdate(&client->inGame, authoritative, predicted, localParticipantIds, nimbleClient->localParticipantCount, renderStats);
        nlLagometerRenderUpdate(&client->lagometerRender, &client->nimbleEngineClient.nimbleClient.client.lagometer);
    }

    nlFrontendRenderUpdate(&client->frontendRender, &app->frontend);

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
            newMode = (TransportStackInternetSimulationMode) (((int) client->singleTransport.conclave
                                                                   .internetSimulationMode +
                                                               1) %
                                                              3);

        transportStackSingleSetInternetSimulationMode(&client->singleTransport, newMode);
        CLOG_NOTICE("internet simulation mode: %d", newMode)
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
    app.nbdServerIsStarted = false;
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
