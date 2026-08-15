#include "expansion_state_machine.h"

#include "bridge_config.h"

void expansion_state_machine_init(ExpansionStateMachine* machine) {
    if(!machine) return;
    machine->state = ExpansionStateDisconnected;
    machine->reconnect_attempts = 0U;
}

static bool fail_connection(ExpansionStateMachine* machine, ExpansionAction* action) {
    if(machine->reconnect_attempts >= BRIDGE_EXPANSION_MAX_RETRIES) {
        machine->state = ExpansionStateDisconnected;
        *action = ExpansionActionNone;
    } else {
        ++machine->reconnect_attempts;
        machine->state = ExpansionStateBackoff;
        *action = ExpansionActionScheduleReconnect;
    }
    return true;
}

bool expansion_state_machine_apply(
    ExpansionStateMachine* machine,
    ExpansionEvent event,
    ExpansionAction* action) {
    if(!machine || !action) return false;
    *action = ExpansionActionNone;

    if(event == ExpansionEventTimeout || event == ExpansionEventFrameError) {
        return fail_connection(machine, action);
    }

    switch(machine->state) {
    case ExpansionStateDisconnected:
        if(event != ExpansionEventStart) return false;
        machine->state = ExpansionStateInitialUart;
        *action = ExpansionActionDetectPulse;
        return true;
    case ExpansionStateInitialUart:
        if(event != ExpansionEventHeartbeatReceived) return false;
        machine->state = ExpansionStateNegotiatingBaud;
        *action = ExpansionActionRequestBaud;
        return true;
    case ExpansionStateNegotiatingBaud:
        if(event == ExpansionEventBaudRejected) {
            *action = ExpansionActionTryLowerBaud;
            return true;
        }
        if(event != ExpansionEventBaudAccepted) return false;
        machine->state = ExpansionStateReady;
        machine->reconnect_attempts = 0U;
        *action = ExpansionActionSwitchBaud;
        return true;
    case ExpansionStateReady:
        if(event != ExpansionEventBeginRpc) return false;
        machine->state = ExpansionStateStartingRpc;
        *action = ExpansionActionSendStartRpc;
        return true;
    case ExpansionStateStartingRpc:
        if(event != ExpansionEventRpcStarted) return false;
        machine->state = ExpansionStateRpcReady;
        return true;
    case ExpansionStateRpcReady:
        if(event != ExpansionEventEndRpc) return false;
        machine->state = ExpansionStateStoppingRpc;
        *action = ExpansionActionSendStopRpc;
        return true;
    case ExpansionStateStoppingRpc:
        if(event != ExpansionEventRpcStopped) return false;
        machine->state = ExpansionStateReady;
        return true;
    case ExpansionStateBackoff:
        if(event != ExpansionEventBackoffElapsed) return false;
        machine->state = ExpansionStateInitialUart;
        *action = ExpansionActionDetectPulse;
        return true;
    }
    return false;
}
