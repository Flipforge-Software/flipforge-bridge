#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ExpansionStateDisconnected,
    ExpansionStateInitialUart,
    ExpansionStateNegotiatingBaud,
    ExpansionStateReady,
    ExpansionStateStartingRpc,
    ExpansionStateRpcReady,
    ExpansionStateStoppingRpc,
    ExpansionStateBackoff,
} ExpansionState;

typedef enum {
    ExpansionEventStart,
    ExpansionEventHeartbeatReceived,
    ExpansionEventBaudAccepted,
    ExpansionEventBaudRejected,
    ExpansionEventBeginRpc,
    ExpansionEventRpcStarted,
    ExpansionEventEndRpc,
    ExpansionEventRpcStopped,
    ExpansionEventTimeout,
    ExpansionEventFrameError,
    ExpansionEventBackoffElapsed,
} ExpansionEvent;

typedef enum {
    ExpansionActionNone,
    ExpansionActionDetectPulse,
    ExpansionActionRequestBaud,
    ExpansionActionTryLowerBaud,
    ExpansionActionSwitchBaud,
    ExpansionActionSendStartRpc,
    ExpansionActionSendStopRpc,
    ExpansionActionScheduleReconnect,
} ExpansionAction;

typedef struct {
    ExpansionState state;
    uint8_t reconnect_attempts;
} ExpansionStateMachine;

void expansion_state_machine_init(ExpansionStateMachine* machine);
bool expansion_state_machine_apply(
    ExpansionStateMachine* machine,
    ExpansionEvent event,
    ExpansionAction* action);
