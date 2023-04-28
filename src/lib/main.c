/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "frontend.h"
#include "frontend_render.h"
#include "rectify/rectify.h"
#include <clog/console.h>
#include <imprint/default_setup.h>
#include <nimble-ball-presentation/render.h>
#include <nimble-ball-simulation/nimble_ball_simulation_vm.h>
#include <nimble-client/network_realizer.h>
#include <nimble-server/server.h>
#include <sdl-render/gamepad.h>
#include <udp-client/udp_client.h>
#include <udp-server/udp_server.h>

clog_config g_clog;

struct DatagramTransportInOutUdpServer;

typedef struct DatagramTransportInOutUdpServerRemote {
    struct sockaddr_in addr;
    struct DatagramTransportInOutUdpServer* parent;
} DatagramTransportInOutUdpServerRemote;

typedef struct DatagramTransportInOutUdpServer {
    UdpServerSocket* udpServer;
    DatagramTransportInOutUdpServerRemote connections[16];
    size_t connectionCapacity;
} DatagramTransportInOutUdpServer;

void datagramTransportInOutUdpServerInit(DatagramTransportInOutUdpServer* self, UdpServerSocket* udpServer);
int datagramTransportInOutUdpServerReceive(DatagramTransportInOutUdpServer* self, int* connection, uint8_t* buf,
                                           size_t maxDatagramSize, UdpTransportOut* response);

void datagramTransportInOutUdpServerInit(DatagramTransportInOutUdpServer* self, UdpServerSocket* udpServer)
{
    self->udpServer = udpServer;
    self->connectionCapacity = 16;
    for (size_t i = 0; i < self->connectionCapacity; ++i) {
        self->connections[i].addr.sin_addr.s_addr = 0;
        self->connections[i].parent = self;
    }
}

static int wrappedSend(void* _self, const uint8_t* data, size_t size)
{
    DatagramTransportInOutUdpServerRemote* self = _self;
    return udpServerSend(self->parent->udpServer, data, size, &self->addr);
}

int datagramTransportInOutUdpServerReceive(DatagramTransportInOutUdpServer* self, int* connection, uint8_t* buf,
                                           size_t maxDatagramSize, UdpTransportOut* response)
{
    struct sockaddr_in receivedFromAddr;
    int octetsReceived = udpServerReceive(self->udpServer, buf, &maxDatagramSize, &receivedFromAddr);
    if (octetsReceived == 0) {
        return 0;
    }

    if (octetsReceived < 0) {
        return octetsReceived;
    }

    for (size_t i = 0; i < self->connectionCapacity; ++i) {
        if (receivedFromAddr.sin_addr.s_addr == self->connections[i].addr.sin_addr.s_addr &&
            receivedFromAddr.sin_port == self->connections[i].addr.sin_port) {
            *connection = i;
            response->self = &self->connections[i];
            response->send = wrappedSend;
            return octetsReceived;
        }
    }

    for (size_t i = 0; i < self->connectionCapacity; ++i) {
        if (self->connections[i].addr.sin_addr.s_addr == 0) {
            *connection = i;
            response->self = &self->connections[i];
            response->send = wrappedSend;
            self->connections[i].addr = receivedFromAddr;
            CLOG_DEBUG("Received from new udp address. Assigned to connectionId: %d", i)
            return octetsReceived;
        }
    }

    CLOG_SOFT_ERROR("could not receive from udp server")
    return -94;
}

/*
int datagramTransportUdpServerSend(DatagramTransportInOutUdpServer* self, int connectionId, uint8_t* buf, size_t
octetCount)
{
    DatagramTransportInOutUdpServerRemote* connection = &self->connections[connectionId];

    return udpServerSend(&self->udpServer, connection->addr, buf, octetCount);
}
*/

typedef struct NimbleEngineClientSetup {
    UdpTransportInOut transport;
    struct ImprintAllocator* memory;
    struct ImprintAllocatorWithFree* blobMemory;
    TransmuteVm authoritative;
    TransmuteVm predicted;
    size_t maximumInputOctetSize;
    size_t maximumPlayerCount;
    Clog log;
} NimbleEngineClientSetup;

typedef enum NimbleEngineClientPhase {
    NimbleEngineClientPhaseWaitingForInitialGameState,
    NimbleEngineClientPhaseInGame,
} NimbleEngineClientPhase;

typedef struct NimbleEngineClient {
    Rectify rectify;
    NimbleClientRealize nimbleClient;
    NimbleEngineClientPhase phase;
    TransmuteVm authoritative;
    TransmuteVm predicted;
    size_t maximumInputOctetSize;
    size_t maximumPlayerCount;
    Clog log;
} NimbleEngineClient;

typedef struct NimbleEngineClientPlayerJoinOptions {
    uint8_t localIndex;
} NimbleEngineClientPlayerJoinOptions;

typedef struct NimbleEngineClientGameJoinOptions {
    NimbleEngineClientPlayerJoinOptions players[8];
    size_t playerCount;
} NimbleEngineClientGameJoinOptions;

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup);
void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options);
void nimbleEngineClientUpdate(NimbleEngineClient* self);

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup)
{
    if (!transmuteVmVersionIsEqual(&setup.predicted.version, &setup.authoritative.version)) {
        CLOG_ERROR("not same transmuteVmVersion %d.%d.%d", setup.predicted.version.major, setup.predicted.version.minor,
                   setup.predicted.version.patch)
    }
    NimbleClientRealizeSettings serializeSetup;
    serializeSetup.memory = setup.memory;
    serializeSetup.blobMemory = setup.blobMemory;
    serializeSetup.transport = setup.transport;
    self->phase = NimbleEngineClientPhaseWaitingForInitialGameState;
    self->authoritative = setup.authoritative;
    self->predicted = setup.predicted;
    self->maximumInputOctetSize = setup.maximumInputOctetSize;
    self->log = setup.log;
    self->maximumPlayerCount = setup.maximumPlayerCount;
    nimbleClientRealizeInit(&self->nimbleClient, &serializeSetup);
}

void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options)
{
    NimbleSerializeGameJoinOptions joinOptions;

    for (size_t i = 0; i < options.playerCount; ++i) {
        joinOptions.players[i].localIndex = options.players[i].localIndex;
    }
    joinOptions.playerCount = options.playerCount;

    NimbleSerializeVersion applicationVersion = {self->authoritative.version.major, self->authoritative.version.minor,
                                                 self->authoritative.version.patch};
    joinOptions.applicationVersion = applicationVersion;
    nimbleClientRealizeJoinGame(&self->nimbleClient, joinOptions);
}

static void joinGameState(NimbleEngineClient* self)
{
    self->phase = NimbleEngineClientPhaseInGame;
    // TransmuteVm authoritativeVm, TransmuteVm predictVm, RectifySetup setup, TransmuteState state, StepId stepId)
    RectifySetup rectifySetup;
    rectifySetup.allocator = self->nimbleClient.settings.memory;
    rectifySetup.maxInputOctetSize = self->maximumInputOctetSize;
    rectifySetup.maxPlayerCount = self->maximumPlayerCount;
    rectifySetup.log = self->log;

    const NimbleClientGameState* joinedGameState = &self->nimbleClient.client.joinedGameState;
    TransmuteState joinedTransmuteState = {joinedGameState->gameState, joinedGameState->gameStateOctetCount};
    CLOG_C_DEBUG(&self->log, "Joined game state. octetCount: %zu step %04X", joinedGameState->gameStateOctetCount,
                 joinedGameState->stepId);

    rectifyInit(&self->rectify, self->authoritative, self->predicted, rectifySetup, joinedTransmuteState,
                joinedGameState->stepId);
}

void nimbleEngineClientUpdate(NimbleEngineClient* self)
{
    size_t targetFps;

    nimbleClientRealizeUpdate(&self->nimbleClient, monotonicTimeMsNow(), &targetFps);

    switch (self->phase) {
        case NimbleEngineClientPhaseWaitingForInitialGameState:
            switch (self->nimbleClient.state) {
                case NimbleClientRealizeStateInGame:
                    joinGameState(self);
                case NimbleClientRealizeStateInit:
                    break;
                case NimbleClientRealizeStateReInit:
                    break;
                case NimbleClientRealizeStateCleared:
                    break;
            }
        case NimbleEngineClientPhaseInGame:
            break;
    }
}

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

    // Set before every render
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
    DatagramTransportInOutUdpServer udpServerWrapper;
    NbdServer nimbleServer;
    UdpTransportInOut transportForHost;

    bool isHosting;
    NimbleEngineClient nimbleEngineClient;
    UdpClientSocket udpClient;
    UdpTransportInOut transportForClient;

    NlSimulationVm authoritative;
    NlSimulationVm predicted;
    SrWindow window;
} NlApp;

static void renderCallback(void* _self, SrWindow* window)
{
    NlCombinedRender* combinedRender = (NlCombinedRender*) _self;

    if (combinedRender->authoritative != 0 && combinedRender->predicted != 0) {
        NlRenderStats renderStats;
        renderStats.predictedTickId = 0;
        renderStats.authoritativeStepsInBuffer = 0;
        nlRenderUpdate(&combinedRender->inGame, combinedRender->authoritative, combinedRender->predicted, 0, 0,
                       renderStats);
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

    datagramTransportInOutUdpServerInit(&app->udpServerWrapper, &app->udpServer);

    CLOG_INFO("wrapped udp server to handle connections")

    NimbleSerializeVersion serverReportTransmuteVmVersion = {
        app->authoritative.transmuteVm.version.major,
        app->authoritative.transmuteVm.version.minor,
        app->authoritative.transmuteVm.version.patch,
    };

    errorCode = nbdServerInit(&app->nimbleServer, serverReportTransmuteVmVersion, allocator, allocatorWithFree);
    if (errorCode < 0) {
        CLOG_ERROR("could not initialize nimble server %d", errorCode)
    }
    CLOG_INFO("nimble server is initialized")

    NlGame initialServerState;
    nlGameInit(&initialServerState);
    // We just add a completely empty game. But it could be setup
    // with specific rules or game mode or similar
    // Since the whole game is blittable structs with no pointers, we can just cast it to an (uint8_t*)
    nbdServerReInitWithGame(&app->nimbleServer, (const uint8_t*) &initialServerState, sizeof(initialServerState));

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

static void startJoining(NlApp* app, NlFrontend* frontend, ImprintAllocator* allocator,
                         ImprintAllocatorWithFree* allocatorWithFree)
{
    CLOG_DEBUG("start joining")
    int errorCode = udpClientInit(&app->udpClient, "127.0.0.1", gameUdpPort);
    if (errorCode < 0) {
        CLOG_ERROR("could not start udp client")
    }
    CLOG_DEBUG("udp client is setup for connection")

    udpTransportInit(&app->transportForClient, app, clientSideSend, clientSideReceive);

    CLOG_DEBUG("client datagram transport is set")

    NimbleEngineClientSetup setup;
    setup.memory = allocator;
    setup.blobMemory = allocatorWithFree;
    setup.transport = app->transportForClient;

    setup.authoritative = app->authoritative.transmuteVm;
    setup.predicted = app->predicted.transmuteVm;
    setup.maximumInputOctetSize = 120;
    setup.maximumPlayerCount = 16;

    Clog nimbleEngineClientLog;
    nimbleEngineClientLog.config = &g_clog;
    nimbleEngineClientLog.constantPrefix = "NimbleEngineClient";

    setup.log = nimbleEngineClientLog;
    nimbleEngineClientInit(&app->nimbleEngineClient, setup);

    CLOG_DEBUG("nimble client is setup with transport")

    NimbleEngineClientGameJoinOptions joinOptions;
    joinOptions.playerCount = 1;
    joinOptions.players[0].localIndex = 32;
    nimbleEngineClientRequestJoin(&app->nimbleEngineClient, joinOptions);

    CLOG_DEBUG("nimble client is trying to join / rejoin server")
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
    nlFrontendInit(&combinedRender.frontend);

    SrGamepad gamepads[2];

    srGamepadInit(&gamepads[0]);

    NlApp app;

    srWindowInit(&app.window, 640, 360, "nimble ball");
    nlRenderInit(&combinedRender.inGame, app.window.renderer);
    nlFrontendRenderInit(&combinedRender.frontendRender, &app.window, combinedRender.inGame.font);

    app.phase = NlAppPhaseIdle;

    Clog authoritativeLog;
    authoritativeLog.constantPrefix = "NimbleBallAuth";
    authoritativeLog.config = &g_clog;
    nlSimulationVmInit(&app.authoritative, authoritativeLog);

    Clog predictedLog;
    predictedLog.constantPrefix = "NimbleBallAuth";
    predictedLog.config = &g_clog;
    nlSimulationVmInit(&app.predicted, predictedLog);

    while (1) {
        int wantsToQuit = srGamepadPoll(gamepads, 2);
        if (wantsToQuit) {
            break;
        }

        nlFrontendHandleInput(&combinedRender.frontend, &gamepads[0]);
        switch (app.phase) {
            case NlAppPhaseIdle:
                switch (combinedRender.frontend.mainMenuSelected) {
                    case NlFrontendMenuSelectJoin:
                        CLOG_DEBUG("Join a game")
                        combinedRender.frontend.phase = NlFrontendPhaseJoining;
                        break;
                    case NlFrontendMenuSelectHost:
                        CLOG_DEBUG("Host a game")
                        startHosting(&app, &combinedRender.frontend, &imprintDefaultSetup.tagAllocator.info,
                                     &imprintDefaultSetup.slabAllocator.info);
                        startJoining(&app, &combinedRender.frontend, &imprintDefaultSetup.tagAllocator.info,
                                     &imprintDefaultSetup.slabAllocator.info);
                        break;
                    case NlFrontendMenuSelectUnknown:
                        break;
                }
                break;

            case NlAppPhaseNetwork: {
                size_t targetFps;

                nimbleEngineClientUpdate(&app.nimbleEngineClient);

                if (app.isHosting) {
                    int connectionId;
                    uint8_t datagram[1200];
                    UdpTransportOut responseTransport;

                    int octetCountReceived = datagramTransportInOutUdpServerReceive(
                        &app.udpServerWrapper, &connectionId, datagram, 1200, &responseTransport);
                    if (octetCountReceived > 0) {
                        NbdResponse response;
                        response.transportOut = &responseTransport;
                        int errorCode = nbdServerFeed(&app.nimbleServer, connectionId, datagram, octetCountReceived,
                                                      &response);
                        if (errorCode < 0) {
                            CLOG_ERROR("can not feed server")
                        }
                    }
                }
            }

            break;
        }

        if (app.nimbleEngineClient.phase == NimbleEngineClientPhaseInGame) {
            TransmuteState authoritativeState = transmuteVmGetState(
                &app.nimbleEngineClient.rectify.authoritative.transmuteVm);
            CLOG_ASSERT(authoritativeState.octetSize == sizeof(NlGame), "internal error, wrong auth state size");
            TransmuteState predictedState = transmuteVmGetState(&app.nimbleEngineClient.rectify.predicted.transmuteVm);
            CLOG_ASSERT(predictedState.octetSize == sizeof(NlGame), "internal error, wrong state size");

            combinedRender.authoritative = (const NlGame*) authoritativeState.state;
            combinedRender.predicted = (const NlGame*) authoritativeState.state;
        } else {
            combinedRender.authoritative = 0;
            combinedRender.predicted = 0;
        }
        srWindowRender(&app.window, 0x115511, &combinedRender, renderCallback);
    }

    nlRenderClose(&combinedRender.inGame);
}
