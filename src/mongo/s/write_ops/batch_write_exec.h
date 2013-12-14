/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>

#include <map>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/optime.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/multi_command_dispatch.h"
#include "mongo/s/shard_resolver.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

    class BatchWriteExecStats;

    /**
     * The BatchWriteExec is able to execute client batch write requests, resulting in a batch
     * response to send back to the client.
     *
     * There are two main interfaces the exec uses to "run" the batch:
     *
     *  - the "targeter" used to generate child batch operations to send to particular shards
     *
     *  - the "dispatcher" used to send child batches to several shards at once, and retrieve the
     *    results
     *
     * Both the targeter and dispatcher are assumed to be dedicated to this particular
     * BatchWriteExec instance.
     *
     */
    class BatchWriteExec {
    MONGO_DISALLOW_COPYING (BatchWriteExec);
    public:

        BatchWriteExec( NSTargeter* targeter,
                        ShardResolver* resolver,
                        MultiCommandDispatch* dispatcher );

        /**
         * Executes a client batch write request by sending child batches to several shard
         * endpoints, and returns a client batch write response.
         *
         * Several network round-trips are generally required to execute a write batch.
         *
         * This function does not throw, any errors are reported via the clientResponse.
         *
         * TODO: Stats?
         */
        void executeBatch( const BatchedCommandRequest& clientRequest,
                           BatchedCommandResponse* clientResponse );

        const BatchWriteExecStats& getStats();

        BatchWriteExecStats* releaseStats();

    private:

        // Not owned here
        NSTargeter* _targeter;

        // Not owned here
        ShardResolver* _resolver;

        // Not owned here
        MultiCommandDispatch* _dispatcher;

        // Stats
        auto_ptr<BatchWriteExecStats> _stats;
    };

    // Useful comparator for using connection strings in ordered sets and maps
    struct ConnectionStringComp {
        bool operator()( const ConnectionString& connStrA,
                         const ConnectionString& connStrB ) const {
            return connStrA.toString().compare( connStrB.toString() ) < 0;
        }
    };

    typedef std::map<ConnectionString, OpTime, ConnectionStringComp> HostOpTimeMap;

    class BatchWriteExecStats {
    public:

        // TODO: Other stats can go here

        void noteWriteAt( const ConnectionString& host, OpTime opTime );

        const HostOpTimeMap& getWriteOpTimes() const;

    private:

        HostOpTimeMap _writeOpTimes;
    };
}
