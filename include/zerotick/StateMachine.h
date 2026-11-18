#pragma once

#include <ostream>

namespace zerotick {

/**
 * State of a single stock worker.
 *
 * The lifecycle is deliberately sequential:
 * Monitoring -> EntryPending -> PositionOpen -> ExitPending -> Monitoring
 *
 * Terminal states exist so failures and shutdowns remain observable instead of
 * leaving a worker silently stuck in a position or order state.
 */
enum class WorkerState {
    Idle,
    Monitoring,
    EntryPending,
    PositionOpen,
    ExitPending,
    Cooldown,
    Stopped,
    Error
};

enum class WorkerEvent {
    Start,
    EntrySignal,
    EntryAccepted,
    EntryRejected,
    ExitSignal,
    ExitCompleted,
    ExitRejected,
    PollTimeout,
    Stop,
    Failure
};

class WorkerStateMachine {
public:
    explicit WorkerStateMachine(WorkerState initial = WorkerState::Idle);

    // Returns false if an event is invalid in the current state.
    bool transition(WorkerEvent event);

    WorkerState state() const noexcept;

private:
    WorkerState state_;
};

const char* toString(WorkerState state) noexcept;
const char* toString(WorkerEvent event) noexcept;
std::ostream& operator<<(std::ostream& stream, WorkerState state);
std::ostream& operator<<(std::ostream& stream, WorkerEvent event);

} // namespace zerotick
