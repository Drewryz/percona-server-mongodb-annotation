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

#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * GlobalLockAcquisitionTracker keeps track of the global lock modes acquired during the
 * operation's lifetime. This class is used to track if we ever did a transaction with the
 * intent to do a write, so that we can enforce write concern on noop writes. Also, used
 * during step down to kill all user operations except those that acquired global lock in
 * IS mode.
 */
class GlobalLockAcquisitionTracker {
public:
    static const OperationContext::Decoration<GlobalLockAcquisitionTracker> get;

    // Decoration requires a default constructor.
    GlobalLockAcquisitionTracker() = default;

    /**
     * Returns whether we have ever taken a global lock in X or IX mode in this operation.
     */
    bool getGlobalWriteLocked() const;

    /**
     * Returns whether we have ever taken a global lock in S mode in this operation.
     */
    bool getGlobalSharedLockTaken() const;

    /**
     * Returns whether we have ever taken a global lock in this operation.
     */
    bool getGlobalLockTaken() const;

    /**
     * Sets the mode bit in _globalLockMode. Once a mode bit is set, we won't clear it.
     */
    void setGlobalLockModeBit(LockMode mode);

private:
    // keeps track of the global lock modes acquired for this operation.
    unsigned char _globalLockMode = (1 << MODE_NONE);
};

}  // namespace mongo
