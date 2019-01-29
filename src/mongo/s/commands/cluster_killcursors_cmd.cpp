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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/killcursors_common.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/transaction/transaction_router.h"

namespace mongo {
namespace {

class ClusterKillCursorsCmd final : public KillCursorsCmdBase {
public:
    ClusterKillCursorsCmd() = default;

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        // killCursors must support read concerns in order to be run in transactions.
        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        // killCursors must choose a global read timestamp if it is the first command in a
        // transaction with snapshot level read concern because any shards it may contact will not
        // be able to change the snapshot of the local transactions they begin.
        //
        // TODO SERVER-37045: This can be removed once killCursors is not allowed to start a
        // cross-shard transaction.
        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter->setAtClusterTimeToLatestTime(opCtx);
        }

        return runImpl(opCtx, dbname, cmdObj, result);
    }

private:
    Status _checkAuth(Client* client, const NamespaceString& nss, CursorId cursorId) const final {
        auto const authzSession = AuthorizationSession::get(client);
        auto authChecker = [&authzSession, &nss](UserNameIterator userNames) -> Status {
            return authzSession->checkAuthForKillCursors(nss, userNames);
        };

        return Grid::get(client->getOperationContext())
            ->getCursorManager()
            ->checkAuthForKillCursors(client->getOperationContext(), nss, cursorId, authChecker);
    }

    Status _killCursor(OperationContext* opCtx,
                       const NamespaceString& nss,
                       CursorId cursorId) const final {
        return Grid::get(opCtx)->getCursorManager()->killCursor(opCtx, nss, cursorId);
    }

} clusterKillCursorsCmd;

}  // namespace
}  // namespace mongo
