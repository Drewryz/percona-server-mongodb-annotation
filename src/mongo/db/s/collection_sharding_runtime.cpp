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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(useFCV44CheckShardVersionProtocol);

namespace {

/**
 * Returns whether the specified namespace is used for sharding-internal purposes only and can never
 * be marked as anything other than UNSHARDED, because the call sites which reference these
 * collections are not prepared to handle StaleConfig errors.
 */
bool isNamespaceAlwaysUnsharded(const NamespaceString& nss) {
    // There should never be a case to mark as sharded collections which are on the config server
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer)
        return true;

    return nss.isNamespaceAlwaysUnsharded();
}

class UnshardedCollection : public ScopedCollectionMetadata::Impl {
public:
    UnshardedCollection() = default;

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

const auto kUnshardedCollection = std::make_shared<UnshardedCollection>();

boost::optional<ChunkVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    auto& oss = OperationShardingState::get(opCtx);

    // If there is a version attached to the OperationContext, use it as the received version,
    // otherwise get the received version from the ShardedConnectionInfo
    if (oss.hasShardVersion()) {
        return oss.getShardVersion(nss);
    } else if (auto const info = ShardedConnectionInfo::get(opCtx->getClient(), false)) {
        auto connectionShardVersion = info->getVersion(nss.ns());

        // For backwards compatibility with map/reduce, which can access up to 2 sharded collections
        // in a single call, the lack of version for a namespace on the collection must be treated
        // as UNSHARDED
        return connectionShardVersion.value_or(ChunkVersion::UNSHARDED());
    }

    // There is no shard version information on either 'opCtx' or 'client'. This means that the
    // operation represented by 'opCtx' is unversioned, and the shard version is always OK for
    // unversioned operations
    return boost::none;
}

}  // namespace

CollectionShardingRuntime::CollectionShardingRuntime(ServiceContext* sc,
                                                     NamespaceString nss,
                                                     executor::TaskExecutor* rangeDeleterExecutor)
    : _stateChangeMutex(nss.toString()),
      _nss(std::move(nss)),
      _metadataManager(std::make_shared<MetadataManager>(sc, _nss, rangeDeleterExecutor)) {
    if (isNamespaceAlwaysUnsharded(_nss)) {
        _metadataManager->setFilteringMetadata(CollectionMetadata());
    }
}

CollectionShardingRuntime* CollectionShardingRuntime::get(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get(opCtx, nss);
    return checked_cast<CollectionShardingRuntime*>(css);
}

CollectionShardingRuntime* CollectionShardingRuntime::get_UNSAFE(ServiceContext* svcCtx,
                                                                 const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get_UNSAFE(svcCtx, nss);
    return checked_cast<CollectionShardingRuntime*>(css);
}

ScopedCollectionMetadata CollectionShardingRuntime::getOrphansFilter(OperationContext* opCtx,
                                                                     bool isCollection) {
    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    auto optMetadata = _getMetadataWithVersionCheckAt(opCtx, atClusterTime, isCollection);

    if (!optMetadata)
        return {kUnshardedCollection};

    return {std::move(*optMetadata)};
}

ScopedCollectionMetadata CollectionShardingRuntime::getCurrentMetadata() {
    auto optMetadata = _metadataManager->getActiveMetadata(_metadataManager, boost::none);

    if (!optMetadata)
        return {kUnshardedCollection};

    return {std::move(*optMetadata)};
}

boost::optional<ScopedCollectionMetadata> CollectionShardingRuntime::getCurrentMetadataIfKnown() {
    return _metadataManager->getActiveMetadata(_metadataManager, boost::none);
}

boost::optional<ChunkVersion> CollectionShardingRuntime::getCurrentShardVersionIfKnown() {
    const auto optMetadata = _metadataManager->getActiveMetadata(_metadataManager, boost::none);
    if (!optMetadata)
        return boost::none;

    const auto& metadata = *optMetadata;
    if (!metadata->isSharded())
        return ChunkVersion::UNSHARDED();

    return metadata->getCollVersion();
}

void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx,
                                                         bool isCollection) {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none, isCollection);
}

Status CollectionShardingRuntime::checkShardVersionNoThrow(OperationContext* opCtx,
                                                           bool isCollection) noexcept {
    try {
        checkShardVersionOrThrow(opCtx, isCollection);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.enterCriticalSectionCatchUpPhase();
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.enterCriticalSectionCommitPhase();
}

void CollectionShardingRuntime::exitCriticalSection(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IX));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.exitCriticalSection();
}

std::shared_ptr<Notification<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    ShardingMigrationCriticalSection::Operation op) const {
    return _critSec.getSignal(op);
}

void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    invariant(!newMetadata.isSharded() || !isNamespaceAlwaysUnsharded(_nss),
              str::stream() << "Namespace " << _nss.ns() << " must never be sharded.");

    auto csrLock = CSRLock::lockExclusive(opCtx, this);

    _metadataManager->setFilteringMetadata(std::move(newMetadata));
}

void CollectionShardingRuntime::clearFilteringMetadata() {
    if (!isNamespaceAlwaysUnsharded(_nss)) {
        _metadataManager->clearFilteringMetadata();
    }
}

auto CollectionShardingRuntime::beginReceive(ChunkRange const& range) -> CleanupNotification {
    return _metadataManager->beginReceive(range);
}

void CollectionShardingRuntime::forgetReceive(const ChunkRange& range) {
    _metadataManager->forgetReceive(range);
}

auto CollectionShardingRuntime::cleanUpRange(ChunkRange const& range, CleanWhen when)
    -> CleanupNotification {
    Date_t time =
        (when == kNow) ? Date_t{} : Date_t::now() + Seconds(orphanCleanupDelaySecs.load());
    return _metadataManager->cleanUpRange(range, time);
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               OID const& epoch,
                                               ChunkRange orphanRange) {
    while (true) {
        boost::optional<CleanupNotification> stillScheduled;

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto* const self = CollectionShardingRuntime::get(opCtx, nss);

            {
                // First, see if collection was dropped, but do it in a separate scope in order to
                // not hold reference on it, which would make it appear in use
                const auto optMetadata =
                    self->_metadataManager->getActiveMetadata(self->_metadataManager, boost::none);
                if (!optMetadata)
                    return {ErrorCodes::ConflictingOperationInProgress,
                            "Collection being migrated had its metadata reset"};

                const auto& metadata = *optMetadata;
                if (!metadata->isSharded() || metadata->getCollVersion().epoch() != epoch) {
                    return {ErrorCodes::ConflictingOperationInProgress,
                            "Collection being migrated was dropped"};
                }
            }

            stillScheduled = self->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                log() << "Finished deleting " << nss.ns() << " range "
                      << redact(orphanRange.toString());
                return Status::OK();
            }
        }

        log() << "Waiting for deletion of " << nss.ns() << " range " << orphanRange;

        Status result = stillScheduled->waitStatus(opCtx);
        if (!result.isOK()) {
            return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                    << " range " << orphanRange.toString());
        }
    }

    MONGO_UNREACHABLE;
}

auto CollectionShardingRuntime::trackOrphanedDataCleanup(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    return _metadataManager->trackOrphanedDataCleanup(range);
}

boost::optional<ChunkRange> CollectionShardingRuntime::getNextOrphanRange(BSONObj const& from) {
    return _metadataManager->getNextOrphanRange(from);
}

boost::optional<ScopedCollectionMetadata> CollectionShardingRuntime::_getMetadataWithVersionCheckAt(
    OperationContext* opCtx,
    const boost::optional<mongo::LogicalTime>& atClusterTime,
    bool isCollection) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion) {
        return boost::none;
    }

    const auto& receivedShardVersion = *optReceivedShardVersion;
    if (ChunkVersion::isIgnoredVersion(receivedShardVersion)) {
        return boost::none;
    }

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    auto csrLock = CSRLock::lockShared(opCtx, this);

    auto metadata = _metadataManager->getActiveMetadata(_metadataManager, atClusterTime);
    auto wantedShardVersion = ChunkVersion::UNSHARDED();

    if (MONGO_unlikely(useFCV44CheckShardVersionProtocol.shouldFail())) {
        LOG(0) << "Received shardVersion: " << receivedShardVersion << " for " << _nss.ns();
        if (isCollection) {
            LOG(0) << "Namespace " << _nss.ns() << " is collection, "
                   << (metadata ? "have shardVersion cached" : "don't know shardVersion");
            uassert(StaleConfigInfo(_nss, receivedShardVersion, wantedShardVersion),
                    "don't know shardVersion",
                    metadata);
            wantedShardVersion = (*metadata)->getShardVersion();
        }
        LOG(0) << "Wanted shardVersion: " << wantedShardVersion << " for " << _nss.ns();
    } else {
        if (metadata && (*metadata)->isSharded()) {
            wantedShardVersion = (*metadata)->getShardVersion();
        }
    }

    auto criticalSectionSignal = [&] {
        return _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                      ? ShardingMigrationCriticalSection::kWrite
                                      : ShardingMigrationCriticalSection::kRead);
    }();

    if (criticalSectionSignal) {
        uasserted(
            StaleConfigInfo(_nss, receivedShardVersion, wantedShardVersion, criticalSectionSignal),
            str::stream() << "migration commit in progress for " << _nss.ns());
    }

    if (receivedShardVersion.isWriteCompatibleWith(wantedShardVersion)) {
        return metadata;
    }

    //
    // Figure out exactly why not compatible, send appropriate error message
    // The versions themselves are returned in the error, so not needed in messages here
    //

    StaleConfigInfo sci(_nss, receivedShardVersion, wantedShardVersion);

    uassert(std::move(sci),
            str::stream() << "epoch mismatch detected for " << _nss.ns() << ", "
                          << "the collection may have been dropped and recreated",
            wantedShardVersion.epoch() == receivedShardVersion.epoch());

    if (!wantedShardVersion.isSet() && receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (wantedShardVersion.isSet() && !receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

    if (wantedShardVersion.majorVersion() != receivedShardVersion.majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci), str::stream() << "version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx, NamespaceString ns)
    : _nss(std::move(ns)), _opCtx(opCtx) {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->enterCriticalSectionCatchUpPhase(_opCtx);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->exitCriticalSection(_opCtx);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->enterCriticalSectionCommitPhase(_opCtx);
}

}  // namespace mongo
