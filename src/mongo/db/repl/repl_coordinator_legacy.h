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
     * An implementation of ReplicationCoordinator that simply delegates to existing code.
     */
    class LegacyReplicationCoordinator : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(LegacyReplicationCoordinator);

    public:

        LegacyReplicationCoordinator();
        virtual ~LegacyReplicationCoordinator();

        virtual void startReplication(TopologyCoordinator*,
                                      ReplicationExecutor::NetworkInterface*);

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

        virtual Status awaitReplication(const OpTime& ts,
                                        const WriteConcernOptions& writeConcern,
                                        Milliseconds timeout);

        virtual Status stepDown(bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual Status stepDownAndWaitForSecondary(const Milliseconds& initialWaitTime,
                                                   const Milliseconds& stepdownTime,
                                                   const Milliseconds& postStepdownWaitTime);

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual bool canServeReadsFor(const NamespaceString& collection);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(const HostAndPort& member, const OpTime& ts);

        virtual Status processHeartbeat(const BSONObj& cmdObj, BSONObjBuilder* resultObj);

    private:
        Status _stepDownHelper(bool force,
                               const Milliseconds& initialWaitTime,
                               const Milliseconds& stepdownTime,
                               const Milliseconds& postStepdownWaitTime);
    };

} // namespace repl
} // namespace mongo
