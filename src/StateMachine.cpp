#include "zerotick/StateMachine.h"

namespace zerotick {

WorkerStateMachine::WorkerStateMachine(WorkerState initial)
    : state_(initial) {}

bool WorkerStateMachine::transition(WorkerEvent event) {
    switch (state_) {
        case WorkerState::Idle:
            if (event == WorkerEvent::Start) state_ = WorkerState::Monitoring;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else return false;
            break;

        case WorkerState::Monitoring:
            if (event == WorkerEvent::EntrySignal) state_ = WorkerState::EntryPending;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else if (event == WorkerEvent::PollTimeout) return true;
            else return false;
            break;

        case WorkerState::EntryPending:
            if (event == WorkerEvent::EntryAccepted) state_ = WorkerState::PositionOpen;
            else if (event == WorkerEvent::EntryRejected) state_ = WorkerState::Cooldown;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else return false;
            break;

        case WorkerState::PositionOpen:
            if (event == WorkerEvent::ExitSignal) state_ = WorkerState::ExitPending;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else return false;
            break;

        case WorkerState::ExitPending:
            if (event == WorkerEvent::ExitCompleted) state_ = WorkerState::Cooldown;
            else if (event == WorkerEvent::ExitRejected) state_ = WorkerState::Error;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else return false;
            break;

        case WorkerState::Cooldown:
            if (event == WorkerEvent::PollTimeout) state_ = WorkerState::Monitoring;
            else if (event == WorkerEvent::Stop) state_ = WorkerState::Stopped;
            else if (event == WorkerEvent::Failure) state_ = WorkerState::Error;
            else return false;
            break;

        case WorkerState::Stopped:
        case WorkerState::Error:
            return false;
    }

    return true;
}

WorkerState WorkerStateMachine::state() const noexcept {
    return state_;
}

const char* toString(WorkerState state) noexcept {
    switch (state) {
        case WorkerState::Idle: return "Idle";
        case WorkerState::Monitoring: return "Monitoring";
        case WorkerState::EntryPending: return "EntryPending";
        case WorkerState::PositionOpen: return "PositionOpen";
        case WorkerState::ExitPending: return "ExitPending";
        case WorkerState::Cooldown: return "Cooldown";
        case WorkerState::Stopped: return "Stopped";
        case WorkerState::Error: return "Error";
    }
    return "Unknown";
}

const char* toString(WorkerEvent event) noexcept {
    switch (event) {
        case WorkerEvent::Start: return "Start";
        case WorkerEvent::EntrySignal: return "EntrySignal";
        case WorkerEvent::EntryAccepted: return "EntryAccepted";
        case WorkerEvent::EntryRejected: return "EntryRejected";
        case WorkerEvent::ExitSignal: return "ExitSignal";
        case WorkerEvent::ExitCompleted: return "ExitCompleted";
        case WorkerEvent::ExitRejected: return "ExitRejected";
        case WorkerEvent::PollTimeout: return "PollTimeout";
        case WorkerEvent::Stop: return "Stop";
        case WorkerEvent::Failure: return "Failure";
    }
    return "Unknown";
}

std::ostream& operator<<(std::ostream& stream, WorkerState state) {
    return stream << toString(state);
}

std::ostream& operator<<(std::ostream& stream, WorkerEvent event) {
    return stream << toString(event);
}

} // namespace zerotick
