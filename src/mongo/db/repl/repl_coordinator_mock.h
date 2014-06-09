/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_coordinator.h"

namespace mongo {
namespace repl {

    /**
     * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
     * into dbtests.
     */
    class ReplicationCoordinatorMock : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorMock);

    public:

        ReplicationCoordinatorMock();
        virtual ~ReplicationCoordinatorMock();

        virtual void startReplication();

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual bool isReplEnabled() const;

        virtual Mode getReplicationMode() const;

        virtual const MemberState& getCurrentMemberState() const;

        virtual Status awaitReplication(const OpTime& ts,
                                        const WriteConcernOptions& writeConcern,
                                        Milliseconds timeout);


        virtual bool canAcceptWritesFor(const NamespaceString& collection);

        virtual bool canServeReadsFor(const NamespaceString& collection);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(const HostAndPort& member, const OpTime& ts);

        virtual bool processHeartbeat(OperationContext* txn, 
                                      const BSONObj& cmdObj, 
                                      std::string* errmsg,
                                      BSONObjBuilder* result);
    };

} // namespace repl
} // namespace mongo
