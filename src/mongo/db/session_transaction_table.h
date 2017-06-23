/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#pragma once

#include "mongo/db/logical_session_id.h"
#include "mongo/db/session_txn_state_holder.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

class LogicalSessionCache;
class OperationContext;
class ServiceContext;

/**
 * Keeps track of the latest transaction for every session.
 */
class SessionTransactionTable {
public:
    explicit SessionTransactionTable(ServiceContext* serviceContext);

    /**
     * Instantiates a transaction table on the specified service context. Must be called only once
     * and is not thread-safe.
     */
    static void create(ServiceContext* service);

    /**
     * Retrieves the session transaction table associated with the service or operation context.
     * Must only be called after 'create' has been called.
     */
    static SessionTransactionTable* get(OperationContext* opCtx);
    static SessionTransactionTable* get(ServiceContext* service);

    /**
     * Invoked when the node enters the primary state. Ensures that the transactions collection is
     * created. Throws on severe exceptions due to which it is not safe to continue the step-up
     * process.
     */
    void onStepUp(OperationContext* opCtx);

    /**
     * Returns transaction state with the given sessionId and txnNum.
     * Throws if:
     *  - session is no longer active.
     *  - there is already a SessionTransaction that has a higher TxnNumber.
     */
    std::shared_ptr<SessionTxnStateHolder> getSessionTxnState(const LogicalSessionId& sessionId);

    /**
     * Removes all entries with sessions that are no longer active.
     */
    void cleanupInactiveSessions(OperationContext* opCtx);

private:
    ServiceContext* const _serviceContext;

    stdx::mutex _mutex;
    stdx::unordered_map<LogicalSessionId,
                        std::shared_ptr<SessionTxnStateHolder>,
                        LogicalSessionId::Hash>
        _txnTable;
};

}  // namespace mongo
