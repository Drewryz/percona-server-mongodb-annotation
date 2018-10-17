
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/platform/hash_namespace.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;

namespace executor {

struct ConnectionPoolStats;

/**
 * Executor with notions of events and callbacks.
 *
 * Callbacks represent work to be performed by the executor.
 * They may be scheduled by client threads or by other callbacks.  Methods that
 * schedule callbacks return a CallbackHandle if they are able to enqueue the callback in the
 * appropriate work queue.  Every CallbackHandle represents an invocation of a function that
 * will happen before the executor goes out of scope.  Calling cancel(CallbackHandle) schedules
 * the specified callback to run with a flag indicating that it is "canceled," but it will run.
 * Client threads may block waiting for a callback to execute by calling wait(CallbackHandle).
 *
 * Events are level-triggered and may only be signaled one time.  Client threads and callbacks
 * may schedule callbacks to be run by the executor after the event is signaled, and client
 * threads may ask the executor to block them until after the event is signaled.
 *
 * If an event is unsignaled when shutdown is called, the executor will ensure that any threads
 * blocked in waitForEvent() eventually return.
 */
class TaskExecutor {
    MONGO_DISALLOW_COPYING(TaskExecutor);

public:
    struct CallbackArgs;
    struct RemoteCommandCallbackArgs;
    class CallbackState;
    class CallbackHandle;
    class EventState;
    class EventHandle;

    using ResponseStatus = RemoteCommandResponse;

    /**
     * Type of a regular callback function.
     *
     * The status argument passed at invocation will have code ErrorCodes::CallbackCanceled if
     * the callback was canceled for any reason (including shutdown).  Otherwise, it should have
     * Status::OK().
     */
    using CallbackFn = stdx::function<void(const CallbackArgs&)>;

    /**
     * Type of a callback from a request to run a command on a remote MongoDB node.
     *
     * The StatusWith<const BSONObj> will have ErrorCodes::CallbackCanceled if the callback was
     * canceled.  Otherwise, its status will represent any failure to execute the command.
     * If the command executed and a response came back, then the status object will contain
     * the BSONObj returned by the command, with the "ok" field indicating the success of the
     * command in the usual way.
     */
    using RemoteCommandCallbackFn = stdx::function<void(const RemoteCommandCallbackArgs&)>;

    /**
     * Destroys the task executor. Implicitly performs the equivalent of shutdown() and join()
     * before returning, if necessary.
     */
    virtual ~TaskExecutor();

    /**
     * Causes the executor to initialize its internal state (start threads if appropriate, create
     * network sockets, etc). This method may be called at most once for the lifetime of an
     * executor.
     */
    virtual void startup() = 0;

    /**
     * Signals to the executor that it should shut down. This method may be called from within a
     * callback.  As such, this method must not block. After shutdown returns, attempts to schedule
     * more tasks on the executor will return errors.
     *
     * It is legal to call this method multiple times. If the task executor goes out of scope
     * before this method is called, the destructor performs this activity.
     */
    virtual void shutdown() = 0;

    /**
     * Waits for the shutdown sequence initiated by a call to shutdown() to complete. Must not be
     * called from within a callback.
     *
     * Unlike stdx::thread::join, this method may be called from any thread that wishes to wait for
     * shutdown to complete.
     */
    virtual void join() = 0;

    /**
     * Writes diagnostic information into "b".
     */
    virtual void appendDiagnosticBSON(BSONObjBuilder* b) const = 0;

    /**
     * Gets the current time.  Callbacks should use this method to read the system clock.
     */
    virtual Date_t now() = 0;

    /**
     * Creates a new event.  Returns a handle to the event, or ErrorCodes::ShutdownInProgress if
     * makeEvent() fails because the executor is shutting down.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual StatusWith<EventHandle> makeEvent() = 0;

    /**
     * Signals the event, making waiting client threads and callbacks runnable.
     *
     * May be called up to one time per event.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual void signalEvent(const EventHandle& event) = 0;

    /**
     * Schedules a callback, "work", to run after "event" is signaled.  If "event"
     * has already been signaled, marks "work" as immediately runnable.
     *
     * If "event" has yet to be signaled when "shutdown()" is called, "work" will
     * be scheduled with a status of ErrorCodes::CallbackCanceled.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual StatusWith<CallbackHandle> onEvent(const EventHandle& event,
                                               const CallbackFn& work) = 0;

    /**
     * Blocks the calling thread until "event" is signaled. Also returns if the event is never
     * signaled but shutdown() is called on the executor.
     *
     * TODO(schwerin): Return ErrorCodes::ShutdownInProgress when shutdown() has been called so that
     * the caller can know which of the two reasons led to this method returning.
     *
     * NOTE: Do not call from a callback running in the executor.
     */
    virtual void waitForEvent(const EventHandle& event) = 0;

    /**
     * Same as waitForEvent without an OperationContext, but returns Status::OK with
     * cv_status::timeout if the event was not triggered within deadline.
     */
    virtual StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                                     const EventHandle& event,
                                                     Date_t deadline) = 0;

    /**
     * Schedules "work" to be run by the executor ASAP.
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work) = 0;

    /**
     * Schedules "work" to be run by the executor no sooner than "when".
     *
     * If "when" is <= now(), then it schedules the "work" to be run ASAP.
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work) = 0;

    /**
     * Schedules "cb" to be run by the executor with the result of executing the remote command
     * described by "request".
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                             const RemoteCommandCallbackFn& cb) = 0;

    /**
     * If the callback referenced by "cbHandle" hasn't already executed, marks it as
     * canceled and runnable.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual void cancel(const CallbackHandle& cbHandle) = 0;

    /**
     * Blocks until the executor finishes running the callback referenced by "cbHandle".
     *
     * Because callbacks all run during shutdown if they weren't run beforehand, there is no need
     * to indicate the reason for returning from wait(CallbackHandle).  It is always that the
     * callback ran.
     *
     * NOTE: Do not call from a callback running in the executor.
     */
    virtual void wait(const CallbackHandle& cbHandle) = 0;

    /**
     * Appends information about the underlying network interface's connections to the given
     * builder.
     */
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const = 0;

protected:
    // Retrieves the Callback from a given CallbackHandle
    static CallbackState* getCallbackFromHandle(const CallbackHandle& cbHandle);

    // Retrieves the Event from a given EventHandle
    static EventState* getEventFromHandle(const EventHandle& eventHandle);

    // Sets the given CallbackHandle to point to the given callback.
    static void setCallbackForHandle(CallbackHandle* cbHandle,
                                     std::shared_ptr<CallbackState> callback);

    // Sets the given EventHandle to point to the given event.
    static void setEventForHandle(EventHandle* eventHandle, std::shared_ptr<EventState> event);

    TaskExecutor();
};

/**
 * Class representing a scheduled callback and providing methods for interacting with it.
 */
class TaskExecutor::CallbackState {
    MONGO_DISALLOW_COPYING(CallbackState);

public:
    virtual ~CallbackState();

    virtual void cancel() = 0;
    virtual void waitForCompletion() = 0;
    virtual bool isCanceled() const = 0;

protected:
    CallbackState();
};

/**
 * Handle to a CallbackState.
 */
class TaskExecutor::CallbackHandle {
    friend class TaskExecutor;

public:
    CallbackHandle();

    // Exposed solely for testing.
    explicit CallbackHandle(std::shared_ptr<CallbackState> cbData);

    bool operator==(const CallbackHandle& other) const {
        return _callback == other._callback;
    }

    bool operator!=(const CallbackHandle& other) const {
        return !(*this == other);
    }

    bool isValid() const {
        return _callback.get();
    }

    /**
     * True if this handle is valid.
     */
    explicit operator bool() const {
        return isValid();
    }

    std::size_t hash() const {
        return std::hash<decltype(_callback)>()(_callback);
    }

    bool isCanceled() const {
        return getCallback()->isCanceled();
    }

private:
    void setCallback(std::shared_ptr<CallbackState> callback) {
        _callback = callback;
    }

    CallbackState* getCallback() const {
        return _callback.get();
    }

    std::shared_ptr<CallbackState> _callback;
};

/**
 * Class representing a scheduled event and providing methods for interacting with it.
 */
class TaskExecutor::EventState {
    MONGO_DISALLOW_COPYING(EventState);

public:
    virtual ~EventState();

    virtual void signal() = 0;
    virtual void waitUntilSignaled() = 0;
    virtual bool isSignaled() = 0;

protected:
    EventState();
};

/**
 * Handle to an EventState.
 */
class TaskExecutor::EventHandle {
    friend class TaskExecutor;

public:
    EventHandle();
    explicit EventHandle(std::shared_ptr<EventState> event);

    bool operator==(const EventHandle& other) const {
        return _event == other._event;
    }

    bool operator!=(const EventHandle& other) const {
        return !(*this == other);
    }

    bool isValid() const {
        return _event.get();
    }

    /**
     * True if this event handle is valid.
     */
    explicit operator bool() const {
        return isValid();
    }

private:
    void setEvent(std::shared_ptr<EventState> event) {
        _event = event;
    }

    EventState* getEvent() const {
        return _event.get();
    }

    std::shared_ptr<EventState> _event;
};

/**
 * Argument passed to all callbacks scheduled via a TaskExecutor.
 */
struct TaskExecutor::CallbackArgs {
    CallbackArgs(TaskExecutor* theExecutor,
                 CallbackHandle theHandle,
                 Status theStatus,
                 OperationContext* opCtx = NULL);

    TaskExecutor* executor;
    CallbackHandle myHandle;
    Status status;
    OperationContext* opCtx;
};

/**
 * Argument passed to all remote command callbacks scheduled via a TaskExecutor.
 */
struct TaskExecutor::RemoteCommandCallbackArgs {
    RemoteCommandCallbackArgs(TaskExecutor* theExecutor,
                              const CallbackHandle& theHandle,
                              const RemoteCommandRequest& theRequest,
                              const ResponseStatus& theResponse);

    TaskExecutor* executor;
    CallbackHandle myHandle;
    RemoteCommandRequest request;
    ResponseStatus response;
};

}  // namespace executor
}  // namespace mongo

// Provide a specialization for hash<CallbackHandle> so it can easily be stored in unordered_set.
MONGO_HASH_NAMESPACE_START
template <>
struct hash<::mongo::executor::TaskExecutor::CallbackHandle> {
    size_t operator()(const ::mongo::executor::TaskExecutor::CallbackHandle& x) const {
        return x.hash();
    }
};
MONGO_HASH_NAMESPACE_END
