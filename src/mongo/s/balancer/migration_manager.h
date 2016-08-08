/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/balancer/balancer_policy.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class OperationContext;
class Status;
template <typename T>
class StatusWith;

// Uniquely identifies a migration, regardless of shard and version.
typedef std::string MigrationIdentifier;
typedef std::map<MigrationIdentifier, Status> MigrationStatuses;

/**
 * Manages and executes parallel migrations for the balancer.
 *
 * TODO: for v3.6, remove code making compatible with v3.2 shards that take distlock.
 */
class MigrationManager {
    MONGO_DISALLOW_COPYING(MigrationManager);

public:
    MigrationManager();
    ~MigrationManager();

    /**
     * A blocking method that attempts to schedule all the migrations specified in
     * "candidateMigrations" and wait for them to complete. Takes the distributed lock for each
     * collection with a chunk being migrated.
     *
     * If any of the migrations, which were scheduled in parallel fails with a LockBusy error
     * reported from the shard, retries it serially without the distributed lock.
     *
     * Returns a map of migration Status objects to indicate the success/failure of each migration.
     */
    MigrationStatuses executeMigrationsForAutoBalance(
        OperationContext* txn,
        const std::vector<MigrateInfo>& migrateInfos,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * A blocking method that attempts to schedule the migration specified in "migrateInfo" and
     * waits for it to complete. Takes the distributed lock for the namespace which is being
     * migrated.
     *
     * Returns the status of the migration.
     */
    Status scheduleManualMigration(OperationContext* txn,
                                   const MigrateInfo& migrateInfo,
                                   uint64_t maxChunkSizeBytes,
                                   const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                   bool waitForDelete);

private:
    /**
     * Tracks the execution state of a single migration.
     */
    struct Migration {
        Migration(NamespaceString nss, BSONObj moveChunkCmdObj);
        ~Migration();

        // Namespace for which this migration applies
        NamespaceString nss;

        // Command object representing the migration
        BSONObj moveChunkCmdObj;

        // Callback handle for the migration network request. If the migration has not yet been sent
        // on the network, this value is not set.
        boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle;

        // Notification, which will be signaled when the migration completes
        std::shared_ptr<Notification<Status>> completionNotification;
    };

    // Used as a type in which to store a list of active migrations. The reason to choose list is
    // that its iterators do not get invalidated when entries are removed around them. This allows
    // O(1) removal time.
    using MigrationsList = std::list<Migration>;

    /**
     * Contains the runtime state for a single collection. This class does not have concurrency
     * control of its own and relies on the migration manager's mutex.
     */
    class CollectionMigrationsState {
    public:
        CollectionMigrationsState(DistLockHandle distLockHandle);
        ~CollectionMigrationsState();

        /**
         * Registers a new migration with this state tracker. Must be followed by a call to
         * completeMigration with the returned handle.
         */
        MigrationsList::iterator addMigration(Migration migration);

        /**
         * Must be called exactly once, as a follow-up to an addMigration call, with the iterator
         * returned from it. Removes the specified migration entry from the migrations list and sets
         * its notification status.
         *
         * Returns true if this is the last migration for this collection, in which case it is the
         * caller's responsibility to free the collection distributed lock and get rid of the object
         * by removing it from the owning map.
         */
        bool completeMigration(MigrationsList::iterator it, Status status);

        /**
         * Retrieves the dist lock handle corresponding to the dist lock held for this collection.
         */
        const DistLockHandle& getDistLockHandle() const {
            return _distLockHandle;
        }

    private:
        // Dist lock handle, which should be released at destruction time
        DistLockHandle _distLockHandle;

        // Contains a set of migrations which are currently active for this namespace.
        MigrationsList _migrations;
    };

    using CollectionMigrationsStateMap =
        std::unordered_map<NamespaceString, CollectionMigrationsState>;

    /**
     * Optionally takes the collection distributed lock and schedules a chunk migration with the
     * specified parameters. May block for distributed lock acquisition. If dist lock acquisition is
     * successful (or not done), schedules the migration request and returns a notification which
     * can be used to obtain the outcome of the operation.
     *
     * The 'shardTakesCollectionDistLock' parameter controls whether the distributed lock is
     * acquired by the migration manager or by the shard executing the migration request.
     */
    std::shared_ptr<Notification<Status>> _schedule(
        OperationContext* txn,
        const MigrateInfo& migrateInfo,
        bool shardTakesCollectionDistLock,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * Acquires the collection distributed lock for the specified namespace and if it succeeds,
     * schedules the migration.
     *
     * The distributed lock is acquired before scheduling the first migration for the collection and
     * is only released when all active migrations on the collection have finished.
     */
    void _scheduleWithDistLock(OperationContext* txn,
                               const HostAndPort& targetHost,
                               Migration migration);

    /**
     * Immediately schedules the specified migration without attempting to acquire the collection
     * distributed lock or checking that it is not being held.
     *
     * This method is only used for retrying migrations that have failed with LockBusy errors
     * returned by the shard, which only happens with legacy 3.2 shards that take the collection
     * distributed lock themselves.
     */
    void _scheduleWithoutDistLock(OperationContext* txn,
                                  const HostAndPort& targetHost,
                                  Migration migration);

    // Protects the class state below
    stdx::mutex _mutex;

    // Holds information about each collection's distributed lock and active migrations via a
    // CollectionMigrationState object.
    CollectionMigrationsStateMap _activeMigrationsWithDistLock;

    // Holds information about migrations, which have been scheduled without the collection
    // distributed lock acquired (i.e., the shard is asked to acquire it).
    MigrationsList _activeMigrationsWithoutDistLock;
};

}  // namespace mongo
