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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/stdx/variant.h"

namespace mongo {

/**
 * RAII-style class that locks a sharding state object using the state object's ResourceMutex. The
 * lock will be created and acquired on construction. The lock will be dismissed upon destruction
 * of the sharding state object.
 */
template <class ShardingState>
class ShardingStateLock {

public:
    /**
     * Locks the sharding state object with the sharding state object's ResourceMutex in MODE_IS.
     * When the object goes out of scope, the ResourceMutex will be unlocked.
     */
    static ShardingStateLock<ShardingState> lock(OperationContext* opCtx, ShardingState* state);

    /**
     * Follows the same functionality as the ShardingStateLock lock method, except that
     * lockExclusive takes the ResourceMutex in MODE_X.
     */
    static ShardingStateLock<ShardingState> lockExclusive(OperationContext* opCtx,
                                                          ShardingState* state);

private:
    using StateLock = stdx::variant<Lock::SharedLock, Lock::ExclusiveLock>;

    ShardingStateLock<ShardingState>(OperationContext* opCtx,
                                     ShardingState* state,
                                     LockMode lockMode);

    // The lock created and locked upon construction of a ShardingStateLock object. It locks the
    // ResourceMutex taken from the ShardingState class, passed in on construction.
    StateLock _lock;
};

template <class ShardingState>
ShardingStateLock<ShardingState>::ShardingStateLock(OperationContext* opCtx,
                                                    ShardingState* state,
                                                    LockMode lockMode)
    : _lock([&]() -> StateLock {
          invariant(lockMode == MODE_IS || lockMode == MODE_X);
          return (
              lockMode == MODE_IS
                  ? StateLock(Lock::SharedLock(opCtx->lockState(), state->_stateChangeMutex))
                  : StateLock(Lock::ExclusiveLock(opCtx->lockState(), state->_stateChangeMutex)));
      }()) {}

template <class ShardingState>
ShardingStateLock<ShardingState> ShardingStateLock<ShardingState>::lock(OperationContext* opCtx,
                                                                        ShardingState* state) {
    return ShardingStateLock(opCtx, state, MODE_IS);
}

template <class ShardingState>
ShardingStateLock<ShardingState> ShardingStateLock<ShardingState>::lockExclusive(
    OperationContext* opCtx, ShardingState* state) {
    return ShardingStateLock(opCtx, state, MODE_X);
}

}  // namespace mongo
