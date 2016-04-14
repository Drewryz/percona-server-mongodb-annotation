/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
// Enabling the maxTimeAlwaysTimeOut fail point will cause any query or command run with a
// valid non-zero max time to fail immediately.  Any getmore operation on a cursor already
// created with a valid non-zero max time will also fail immediately.
//
// This fail point cannot be used with the maxTimeNeverTimeOut fail point.
MONGO_FP_DECLARE(maxTimeAlwaysTimeOut);

// Enabling the maxTimeNeverTimeOut fail point will cause the server to never time out any
// query, command, or getmore operation, regardless of whether a max time is set.
//
// This fail point cannot be used with the maxTimeAlwaysTimeOut fail point.
MONGO_FP_DECLARE(maxTimeNeverTimeOut);

// Enabling the checkForInterruptFail fail point will start a game of random chance on the
// connection specified in the fail point data, generating an interrupt with a given fixed
// probability.  Example invocation:
//
// {configureFailPoint: "checkForInterruptFail",
//  mode: "alwaysOn",
//  data: {conn: 17, chance: .01}}
//
// Both data fields must be specified.  In the above example, all interrupt points on connection 17
// will generate a kill on the current operation with probability p(.01), including interrupt points
// of nested operations.  "chance" must be a double between 0 and 1, inclusive.
MONGO_FP_DECLARE(checkForInterruptFail);

}  // namespace

OperationContext::OperationContext(Client* client, unsigned int opId, Locker* locker)
    : _client(client), _opId(opId), _locker(locker) {}

void OperationContext::markKilled(ErrorCodes::Error killCode) {
    invariant(killCode != ErrorCodes::OK);
    _killCode.compareAndSwap(ErrorCodes::OK, killCode);
}

void OperationContext::setDeadlineByDate(Date_t when) {
    _deadline = when;
}

void OperationContext::setDeadlineRelativeToNow(Milliseconds maxTimeMs) {
    auto clock = getServiceContext()->getFastClockSource();
    setDeadlineByDate(clock->now() + clock->getPrecision() + maxTimeMs);
}

bool OperationContext::hasDeadlineExpired() const {
    if (MONGO_FAIL_POINT(maxTimeNeverTimeOut)) {
        return false;
    }
    if (hasDeadline() && MONGO_FAIL_POINT(maxTimeAlwaysTimeOut)) {
        return true;
    }

    const auto now = getServiceContext()->getFastClockSource()->now();
    return now >= getDeadline();
}

Milliseconds OperationContext::getTimeUntilDeadline() const {
    const auto now = getServiceContext()->getFastClockSource()->now();
    return getDeadline() - now;
}

void OperationContext::setMaxTimeMicros(uint64_t maxTimeMicros) {
    const auto maxTimeMicrosSigned = static_cast<int64_t>(maxTimeMicros);
    if (maxTimeMicrosSigned <= 0) {
        // Do not adjust the deadline if the user specified "0", meaning "forever", or
        // if they chose a value too large to represent as a 64-bit signed integer.
        return;
    }
    if (maxTimeMicrosSigned == 1) {
        // This indicates that the time is already expired. Set the deadline to the epoch.
        setDeadlineByDate(Date_t{});
        return;
    }
    setDeadlineRelativeToNow(Microseconds{maxTimeMicrosSigned});
}

bool OperationContext::isMaxTimeSet() const {
    return hasDeadline();
}

uint64_t OperationContext::getRemainingMaxTimeMicros() const {
    if (!hasDeadline()) {
        return 0U;
    }
    const auto microsRemaining = durationCount<Microseconds>(getTimeUntilDeadline());
    if (microsRemaining <= 0) {
        // If the operation deadline has passed, say there is 1 microsecond remaining, to
        // distinguish from 0, which means infinity.
        return 1U;
    }
    return static_cast<uint64_t>(microsRemaining);
}

void OperationContext::checkForInterrupt() {
    uassertStatusOK(checkForInterruptNoAssert());
}

namespace {

// Helper function for checkForInterrupt fail point.  Decides whether the operation currently
// being run by the given Client meet the (probabilistic) conditions for interruption as
// specified in the fail point info.
bool opShouldFail(const OperationContext* opCtx, const BSONObj& failPointInfo) {
    // Only target the client with the specified connection number.
    if (opCtx->getClient()->getConnectionId() != failPointInfo["conn"].safeNumberLong()) {
        return false;
    }

    // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
    double next = static_cast<double>(std::abs(opCtx->getClient()->getPrng().nextInt64()));
    double upperBound =
        std::numeric_limits<int64_t>::max() * failPointInfo["chance"].numberDouble();
    if (next > upperBound) {
        return false;
    }
    return true;
}

}  // namespace

Status OperationContext::checkForInterruptNoAssert() {
    // TODO: Remove once all OperationContexts are properly connected to Clients and ServiceContexts
    // in tests.
    if (MONGO_unlikely(!getClient() || !getServiceContext() ||
                       !getServiceContext()->getFastClockSource())) {
        return Status::OK();
    }

    if (getServiceContext() && getServiceContext()->getKillAllOperations()) {
        return Status(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
    }

    if (hasDeadlineExpired()) {
        markKilled();
        return Status(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
    }

    MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
        if (opShouldFail(this, scopedFailPoint.getData())) {
            log() << "set pending kill on op " << getOpID() << ", for checkForInterruptFail";
            markKilled();
        }
    }

    const auto killStatus = getKillStatus();
    if (killStatus != ErrorCodes::OK) {
        return Status(killStatus, "operation was interrupted");
    }

    return Status::OK();
}

}  // namespace mongo
