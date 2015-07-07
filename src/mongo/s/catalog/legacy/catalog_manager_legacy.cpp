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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"

#include <map>
#include <pcrecpp.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/catalog/legacy/config_coordinator.h"
#include "mongo/s/catalog/legacy/config_upgrade.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/legacy/legacy_dist_lock_manager.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::map;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;
using str::stream;

namespace {

bool validConfigWC(const BSONObj& writeConcern) {
    BSONElement elem(writeConcern["w"]);
    if (elem.eoo()) {
        return true;
    }

    if (elem.isNumber() && elem.numberInt() <= 1) {
        return true;
    }

    if (elem.type() == String && elem.str() == "majority") {
        return true;
    }

    return false;
}

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);

    dassert(response->isValid(NULL));
}

}  // namespace


CatalogManagerLegacy::CatalogManagerLegacy() = default;

CatalogManagerLegacy::~CatalogManagerLegacy() = default;

Status CatalogManagerLegacy::init(const ConnectionString& configDBCS) {
    // Initialization should not happen more than once
    invariant(!_configServerConnectionString.isValid());
    invariant(_configServers.empty());
    invariant(configDBCS.isValid());

    // Extract the hosts in HOST:PORT format
    set<HostAndPort> configHostsAndPortsSet;
    set<string> configHostsOnly;
    std::vector<HostAndPort> configHostAndPorts = configDBCS.getServers();
    for (size_t i = 0; i < configHostAndPorts.size(); i++) {
        // Append the default port, if not specified
        HostAndPort configHost = configHostAndPorts[i];
        if (!configHost.hasPort()) {
            configHost = HostAndPort(configHost.host(), ServerGlobalParams::ConfigServerPort);
        }

        // Make sure there are no duplicates
        if (!configHostsAndPortsSet.insert(configHost).second) {
            StringBuilder sb;
            sb << "Host " << configHost.toString()
               << " exists twice in the config servers listing.";

            return Status(ErrorCodes::InvalidOptions, sb.str());
        }

        configHostsOnly.insert(configHost.host());
    }

    // Make sure the hosts are reachable
    for (set<string>::const_iterator i = configHostsOnly.begin(); i != configHostsOnly.end(); i++) {
        const string host = *i;

        // If this is a CUSTOM connection string (for testing) don't do DNS resolution
        string errMsg;
        if (ConnectionString::parse(host, errMsg).type() == ConnectionString::CUSTOM) {
            continue;
        }

        bool ok = false;

        for (int x = 10; x > 0; x--) {
            if (!hostbyname(host.c_str()).empty()) {
                ok = true;
                break;
            }

            log() << "can't resolve DNS for [" << host << "]  sleeping and trying " << x
                  << " more times";
            sleepsecs(10);
        }

        if (!ok) {
            return Status(ErrorCodes::HostNotFound,
                          stream() << "unable to resolve DNS for host " << host);
        }
    }

    LOG(1) << " config string : " << configDBCS.toString();

    // Now that the config hosts are verified, initialize the catalog manager. The code below
    // should never fail.

    _configServerConnectionString = configDBCS;

    if (_configServerConnectionString.type() == ConnectionString::MASTER) {
        _configServers.push_back(_configServerConnectionString);
    } else if (_configServerConnectionString.type() == ConnectionString::SYNC ||
               (_configServerConnectionString.type() == ConnectionString::SET &&
                _configServerConnectionString.getServers().size() == 1)) {
        // TODO(spencer): Remove second part of the above or statement that allows replset
        // config server strings once we've separated the legacy catalog manager from the
        // CSRS version.
        const vector<HostAndPort> configHPs = _configServerConnectionString.getServers();
        for (vector<HostAndPort>::const_iterator it = configHPs.begin(); it != configHPs.end();
             ++it) {
            _configServers.push_back(ConnectionString(*it));
        }
    } else {
        // This is only for tests.
        invariant(_configServerConnectionString.type() == ConnectionString::CUSTOM);
        _configServers.push_back(_configServerConnectionString);
    }

    _distLockManager = stdx::make_unique<LegacyDistLockManager>(_configServerConnectionString);
    _distLockManager->startUp();

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = false;
        _consistentFromLastCheck = true;
    }

    return Status::OK();
}

Status CatalogManagerLegacy::startup() {
    Status status = _startConfigServerChecker();
    if (!status.isOK()) {
        return status;
    }

    return status;
}

Status CatalogManagerLegacy::checkAndUpgrade(bool checkOnly) {
    VersionType initVersionInfo;
    VersionType versionInfo;
    string errMsg;

    bool upgraded =
        checkAndUpgradeConfigVersion(this, !checkOnly, &initVersionInfo, &versionInfo, &errMsg);
    if (!upgraded) {
        return Status(ErrorCodes::IncompatibleShardingMetadata,
                      str::stream() << "error upgrading config database to v"
                                    << CURRENT_CONFIG_VERSION << causedBy(errMsg));
    }

    return Status::OK();
}

Status CatalogManagerLegacy::_startConfigServerChecker() {
    if (!_checkConfigServersConsistent()) {
        return Status(ErrorCodes::ConfigServersInconsistent,
                      "Data inconsistency detected amongst config servers");
    }

    stdx::thread t(stdx::bind(&CatalogManagerLegacy::_consistencyChecker, this));
    _consistencyCheckerThread.swap(t);

    return Status::OK();
}

ConnectionString CatalogManagerLegacy::connectionString() const {
    return _configServerConnectionString;
}

void CatalogManagerLegacy::shutDown() {
    LOG(1) << "CatalogManagerLegacy::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
        _consistencyCheckerCV.notify_one();
    }

    // Only try to join the thread if we actually started it.
    if (_consistencyCheckerThread.joinable())
        _consistencyCheckerThread.join();

    invariant(_distLockManager);
    _distLockManager->shutDown();
}

Status CatalogManagerLegacy::shardCollection(OperationContext* txn,
                                             const string& ns,
                                             const ShardKeyPattern& fieldsAndOrder,
                                             bool unique,
                                             const vector<BSONObj>& initPoints,
                                             const set<ShardId>& initShardIds) {
    // Lock the collection globally so that no other mongos can try to shard or drop the collection
    // at the same time.
    auto scopedDistLock = getDistLockManager()->lock(ns, "shardCollection");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    StatusWith<DatabaseType> status = getDatabase(nsToDatabase(ns));
    if (!status.isOK()) {
        return status.getStatus();
    }

    DatabaseType dbt = status.getValue();
    ShardId dbPrimaryShardId = dbt.getPrimary();

    // This is an extra safety check that the collection is not getting sharded concurrently by
    // two different mongos instances. It is not 100%-proof, but it reduces the chance that two
    // invocations of shard collection will step on each other's toes.
    {
        ScopedDbConnection conn(_configServerConnectionString, 30);
        unsigned long long existingChunks =
            conn->count(ChunkType::ConfigNS, BSON(ChunkType::ns(ns)));
        if (existingChunks > 0) {
            conn.done();
            return Status(ErrorCodes::AlreadyInitialized,
                          str::stream() << "collection " << ns << " already sharded with "
                                        << existingChunks << " chunks.");
        }

        conn.done();
    }

    log() << "enable sharding on: " << ns << " with shard key: " << fieldsAndOrder;

    // Record start in changelog
    BSONObjBuilder collectionDetail;
    collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
    collectionDetail.append("collection", ns);
    string dbPrimaryShardStr;
    {
        const auto shard = grid.shardRegistry()->getShard(dbPrimaryShardId);
        dbPrimaryShardStr = shard->toString();
    }
    collectionDetail.append("primary", dbPrimaryShardStr);

    {
        BSONArrayBuilder initialShards(collectionDetail.subarrayStart("initShards"));
        for (const ShardId& shardId : initShardIds) {
            initialShards.append(shardId);
        }
    }

    collectionDetail.append("numChunks", static_cast<int>(initPoints.size() + 1));

    logChange(
        txn->getClient()->clientAddress(true), "shardCollection.start", ns, collectionDetail.obj());

    ChunkManagerPtr manager(new ChunkManager(ns, fieldsAndOrder, unique));
    manager->createFirstChunks(dbPrimaryShardId, &initPoints, &initShardIds);
    manager->loadExistingRanges(nullptr);

    CollectionInfo collInfo;
    collInfo.useChunkManager(manager);
    collInfo.save(ns);
    manager->reload(true);

    // Tell the primary mongod to refresh its data
    // TODO:  Think the real fix here is for mongos to just
    //        assume that all collections are sharded, when we get there
    for (int i = 0; i < 4; i++) {
        if (i == 3) {
            warning() << "too many tries updating initial version of " << ns << " on shard primary "
                      << dbPrimaryShardStr
                      << ", other mongoses may not see the collection as sharded immediately";
            break;
        }

        try {
            const auto shard = grid.shardRegistry()->getShard(dbPrimaryShardId);
            ShardConnection conn(shard->getConnString(), ns);
            bool isVersionSet = conn.setVersion();
            conn.done();
            if (!isVersionSet) {
                warning() << "could not update initial version of " << ns << " on shard primary "
                          << dbPrimaryShardStr;
            } else {
                break;
            }
        } catch (const DBException& e) {
            warning() << "could not update initial version of " << ns << " on shard primary "
                      << dbPrimaryShardStr << causedBy(e);
        }

        sleepsecs(i);
    }

    // Record finish in changelog
    BSONObjBuilder finishDetail;

    finishDetail.append("version", manager->getVersion().toString());

    logChange(txn->getClient()->clientAddress(true), "shardCollection", ns, finishDetail.obj());

    return Status::OK();
}

StatusWith<ShardDrainingStatus> CatalogManagerLegacy::removeShard(OperationContext* txn,
                                                                  const std::string& name) {
    ScopedDbConnection conn(_configServerConnectionString, 30);

    if (conn->count(ShardType::ConfigNS,
                    BSON(ShardType::name() << NE << name << ShardType::draining(true)))) {
        conn.done();
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't have more than one draining shard at a time");
    }

    if (conn->count(ShardType::ConfigNS, BSON(ShardType::name() << NE << name)) == 0) {
        conn.done();
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    BSONObj searchDoc = BSON(ShardType::name() << name);

    // Case 1: start draining chunks
    BSONObj drainingDoc = BSON(ShardType::name() << name << ShardType::draining(true));
    BSONObj shardDoc = conn->findOne(ShardType::ConfigNS, drainingDoc);
    if (shardDoc.isEmpty()) {
        log() << "going to start draining shard: " << name;
        BSONObj newStatus = BSON("$set" << BSON(ShardType::draining(true)));

        Status status = update(ShardType::ConfigNS, searchDoc, newStatus, false, false, NULL);
        if (!status.isOK()) {
            log() << "error starting removeShard: " << name << "; err: " << status.reason();
            return status;
        }

        Shard::reloadShardInfo();
        conn.done();

        // Record start in changelog
        logChange(
            txn->getClient()->clientAddress(true), "removeShard.start", "", BSON("shard" << name));
        return ShardDrainingStatus::STARTED;
    }

    // Case 2: all chunks drained
    BSONObj shardIDDoc = BSON(ChunkType::shard(shardDoc[ShardType::name()].str()));
    long long chunkCount = conn->count(ChunkType::ConfigNS, shardIDDoc);
    long long dbCount =
        conn->count(DatabaseType::ConfigNS,
                    BSON(DatabaseType::name.ne("local") << DatabaseType::primary(name)));
    if (chunkCount == 0 && dbCount == 0) {
        log() << "going to remove shard: " << name;
        audit::logRemoveShard(txn->getClient(), name);

        Status status = remove(ShardType::ConfigNS, searchDoc, 0, NULL);
        if (!status.isOK()) {
            log() << "Error concluding removeShard operation on: " << name
                  << "; err: " << status.reason();
            return status;
        }

        grid.shardRegistry()->remove(name);
        Shard::reloadShardInfo();
        conn.done();

        // Record finish in changelog
        logChange(txn->getClient()->clientAddress(true), "removeShard", "", BSON("shard" << name));
        return ShardDrainingStatus::COMPLETED;
    }

    // case 3: draining ongoing
    return ShardDrainingStatus::ONGOING;
}

StatusWith<DatabaseType> CatalogManagerLegacy::getDatabase(const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The two databases that are hosted on the config server are config and admin
    if (dbName == "config" || dbName == "admin") {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setSharded(false);
        dbt.setPrimary("config");

        return dbt;
    }

    ScopedDbConnection conn(_configServerConnectionString, 30.0);

    BSONObj dbObj = conn->findOne(DatabaseType::ConfigNS, BSON(DatabaseType::name(dbName)));
    if (dbObj.isEmpty()) {
        conn.done();
        return {ErrorCodes::DatabaseNotFound, stream() << "database " << dbName << " not found"};
    }

    conn.done();
    return DatabaseType::fromBSON(dbObj);
}

StatusWith<CollectionType> CatalogManagerLegacy::getCollection(const std::string& collNs) {
    ScopedDbConnection conn(_configServerConnectionString, 30.0);

    BSONObj collObj = conn->findOne(CollectionType::ConfigNS, BSON(CollectionType::fullNs(collNs)));
    if (collObj.isEmpty()) {
        conn.done();
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << collNs << " not found");
    }

    conn.done();
    return CollectionType::fromBSON(collObj);
}

Status CatalogManagerLegacy::getCollections(const std::string* dbName,
                                            std::vector<CollectionType>* collections) {
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      (string) "^" + pcrecpp::RE::QuoteMeta(*dbName) + "\\.");
    }

    ScopedDbConnection conn(_configServerConnectionString, 30.0);

    std::unique_ptr<DBClientCursor> cursor(
        _safeCursor(conn->query(CollectionType::ConfigNS, b.obj())));

    while (cursor->more()) {
        const BSONObj collObj = cursor->next();

        const auto collectionResult = CollectionType::fromBSON(collObj);
        if (!collectionResult.isOK()) {
            conn.done();
            collections->clear();
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "error while parsing " << CollectionType::ConfigNS
                                        << " document: " << collObj << " : "
                                        << collectionResult.getStatus().toString());
        }

        collections->push_back(collectionResult.getValue());
    }

    conn.done();
    return Status::OK();
}

Status CatalogManagerLegacy::dropCollection(OperationContext* txn,
                                            const std::string& collectionNs) {
    logChange(
        txn->getClient()->clientAddress(true), "dropCollection.start", collectionNs, BSONObj());

    // Lock the collection globally so that split/migrate cannot run
    auto scopedDistLock = getDistLockManager()->lock(collectionNs, "drop");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    LOG(1) << "dropCollection " << collectionNs << " started";

    // This cleans up the collection on all shards
    vector<ShardType> allShards;
    Status status = getAllShards(&allShards);
    if (!status.isOK()) {
        return status;
    }

    LOG(1) << "dropCollection " << collectionNs << " locked";

    map<string, BSONObj> errors;

    // Delete data from all mongods
    for (vector<ShardType>::const_iterator i = allShards.begin(); i != allShards.end(); i++) {
        const auto shard = grid.shardRegistry()->getShard(i->getName());
        ScopedDbConnection conn(shard->getConnString());

        BSONObj info;
        if (!conn->dropCollection(collectionNs, &info)) {
            // Ignore the database not found errors
            if (info["code"].isNumber() && (info["code"].Int() == ErrorCodes::NamespaceNotFound)) {
                conn.done();
                continue;
            }

            errors[shard->getConnString().toString()] = info;
        }

        conn.done();
    }

    if (!errors.empty()) {
        StringBuilder sb;
        sb << "Dropping collection failed on the following hosts: ";

        for (map<string, BSONObj>::const_iterator it = errors.begin(); it != errors.end(); ++it) {
            if (it != errors.begin()) {
                sb << ", ";
            }

            sb << it->first << ": " << it->second;
        }

        return Status(ErrorCodes::OperationFailed, sb.str());
    }

    LOG(1) << "dropCollection " << collectionNs << " shard data deleted";

    // remove chunk data
    Status result = remove(ChunkType::ConfigNS, BSON(ChunkType::ns(collectionNs)), 0, NULL);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << collectionNs << " chunk data deleted";

    for (vector<ShardType>::const_iterator i = allShards.begin(); i != allShards.end(); i++) {
        const auto shard = grid.shardRegistry()->getShard(i->getName());
        ScopedDbConnection conn(shard->getConnString());

        BSONObj res;

        // this is horrible
        // we need a special command for dropping on the d side
        // this hack works for the moment

        if (!setShardVersion(conn.conn(),
                             collectionNs,
                             _configServerConnectionString.toString(),
                             ChunkVersion(0, 0, OID()),
                             NULL,
                             true,
                             res)) {
            return Status(static_cast<ErrorCodes::Error>(8071),
                          str::stream() << "cleaning up after drop failed: " << res);
        }

        conn->simpleCommand("admin", 0, "unsetSharding");
        conn.done();
    }

    LOG(1) << "dropCollection " << collectionNs << " completed";

    logChange(txn->getClient()->clientAddress(true), "dropCollection", collectionNs, BSONObj());

    return Status::OK();
}

void CatalogManagerLegacy::logAction(const ActionLogType& actionLog) {
    // Create the action log collection and ensure that it is capped. Wrap in try/catch,
    // because creating an existing collection throws.
    if (_actionLogCollectionCreated.load() == 0) {
        try {
            ScopedDbConnection conn(_configServerConnectionString, 30.0);
            conn->createCollection(ActionLogType::ConfigNS, 1024 * 1024 * 2, true);
            conn.done();

            _actionLogCollectionCreated.store(1);
        } catch (const DBException& ex) {
            if (ex.toStatus() == ErrorCodes::NamespaceExists) {
                _actionLogCollectionCreated.store(1);
            } else {
                LOG(1) << "couldn't create actionlog collection: " << ex;
                // If we couldn't create the collection don't attempt the insert otherwise we might
                // implicitly create the collection without it being capped.
                return;
            }
        }
    }

    Status result = insert(ActionLogType::ConfigNS, actionLog.toBSON(), NULL);
    if (!result.isOK()) {
        log() << "error encountered while logging action: " << result;
    }
}

void CatalogManagerLegacy::logChange(const string& clientAddress,
                                     const string& what,
                                     const string& ns,
                                     const BSONObj& detail) {
    // Create the change log collection and ensure that it is capped. Wrap in try/catch,
    // because creating an existing collection throws.
    if (_changeLogCollectionCreated.load() == 0) {
        try {
            ScopedDbConnection conn(_configServerConnectionString, 30.0);
            conn->createCollection(ChangeLogType::ConfigNS, 1024 * 1024 * 10, true);
            conn.done();

            _changeLogCollectionCreated.store(1);
        } catch (const DBException& ex) {
            if (ex.toStatus() == ErrorCodes::NamespaceExists) {
                _changeLogCollectionCreated.store(1);
            } else {
                LOG(1) << "couldn't create changelog collection: " << ex;
                // If we couldn't create the collection don't attempt the insert otherwise we might
                // implicitly create the collection without it being capped.
                return;
            }
        }
    }

    ChangeLogType changeLog;
    {
        // Store this entry's ID so we can use on the exception code path too
        StringBuilder changeIdBuilder;
        changeIdBuilder << getHostNameCached() << "-" << Date_t::now().toString() << "-"
                        << OID::gen();
        changeLog.setChangeId(changeIdBuilder.str());
    }
    changeLog.setServer(getHostNameCached());
    changeLog.setClientAddr(clientAddress);
    changeLog.setTime(Date_t::now());
    changeLog.setWhat(what);
    changeLog.setNS(ns);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    // Send a copy of the message to the local log in case it doesn't manage to reach
    // config.changelog
    log() << "about to log metadata event: " << changeLogBSON;

    Status result = insert(ChangeLogType::ConfigNS, changeLogBSON, NULL);
    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID "
                  << changeLog.getChangeId() << ": " << result;
    }
}

StatusWith<SettingsType> CatalogManagerLegacy::getGlobalSettings(const string& key) {
    try {
        ScopedDbConnection conn(_configServerConnectionString, 30);
        BSONObj settingsDoc = conn->findOne(SettingsType::ConfigNS, BSON(SettingsType::key(key)));
        conn.done();

        if (settingsDoc.isEmpty()) {
            return Status(ErrorCodes::NoMatchingDocument,
                          str::stream() << "can't find settings document with key: " << key);
        }

        StatusWith<SettingsType> settingsResult = SettingsType::fromBSON(settingsDoc);
        if (!settingsResult.isOK()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "error while parsing settings document: " << settingsDoc
                                        << " : " << settingsResult.getStatus().toString());
        }

        const SettingsType& settings = settingsResult.getValue();

        Status validationStatus = settings.validate();
        if (!validationStatus.isOK()) {
            return validationStatus;
        }

        return settingsResult;
    } catch (const DBException& ex) {
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "unable to successfully obtain "
                                    << "config.settings document: " << causedBy(ex));
    }
}

Status CatalogManagerLegacy::getDatabasesForShard(const string& shardName, vector<string>* dbs) {
    dbs->clear();

    try {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        std::unique_ptr<DBClientCursor> cursor(_safeCursor(
            conn->query(DatabaseType::ConfigNS, Query(BSON(DatabaseType::primary(shardName))))));
        if (!cursor.get()) {
            conn.done();
            return Status(ErrorCodes::HostUnreachable,
                          str::stream() << "unable to open cursor for " << DatabaseType::ConfigNS);
        }

        while (cursor->more()) {
            BSONObj dbObj = cursor->nextSafe();

            string dbName;
            Status status = bsonExtractStringField(dbObj, DatabaseType::name(), &dbName);
            if (!status.isOK()) {
                dbs->clear();
                return status;
            }

            dbs->push_back(dbName);
        }

        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

Status CatalogManagerLegacy::getChunks(const BSONObj& query,
                                       const BSONObj& sort,
                                       boost::optional<int> limit,
                                       vector<ChunkType>* chunks) {
    chunks->clear();

    try {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);

        const Query queryWithSort(Query(query).sort(sort));

        std::unique_ptr<DBClientCursor> cursor(
            _safeCursor(conn->query(ChunkType::ConfigNS, queryWithSort, limit.get_value_or(0))));
        if (!cursor.get()) {
            conn.done();
            return Status(ErrorCodes::HostUnreachable, "unable to open chunk cursor");
        }

        while (cursor->more()) {
            BSONObj chunkObj = cursor->nextSafe();

            StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(chunkObj);
            if (!chunkRes.isOK()) {
                conn.done();
                chunks->clear();
                return {ErrorCodes::FailedToParse,
                        stream() << "Failed to parse chunk with id ("
                                 << chunkObj[ChunkType::name()].toString()
                                 << "): " << chunkRes.getStatus().toString()};
            }

            chunks->push_back(chunkRes.getValue());
        }

        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

Status CatalogManagerLegacy::getTagsForCollection(const std::string& collectionNs,
                                                  std::vector<TagsType>* tags) {
    tags->clear();

    try {
        ScopedDbConnection conn(_configServerConnectionString, 30);
        std::unique_ptr<DBClientCursor> cursor(_safeCursor(conn->query(
            TagsType::ConfigNS, Query(BSON(TagsType::ns(collectionNs))).sort(TagsType::min()))));
        if (!cursor.get()) {
            conn.done();
            return Status(ErrorCodes::HostUnreachable, "unable to open tags cursor");
        }

        while (cursor->more()) {
            BSONObj tagObj = cursor->nextSafe();

            StatusWith<TagsType> tagRes = TagsType::fromBSON(tagObj);
            if (!tagRes.isOK()) {
                tags->clear();
                conn.done();
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << "Failed to parse tag: " << tagRes.getStatus().toString());
            }

            tags->push_back(tagRes.getValue());
        }

        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

StatusWith<string> CatalogManagerLegacy::getTagForChunk(const std::string& collectionNs,
                                                        const ChunkType& chunk) {
    BSONObj tagDoc;

    try {
        ScopedDbConnection conn(_configServerConnectionString, 30);

        Query query(BSON(TagsType::ns(collectionNs)
                         << TagsType::min() << BSON("$lte" << chunk.getMin()) << TagsType::max()
                         << BSON("$gte" << chunk.getMax())));

        tagDoc = conn->findOne(TagsType::ConfigNS, query);
        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (tagDoc.isEmpty()) {
        return std::string("");
    }

    auto status = TagsType::fromBSON(tagDoc);
    if (status.isOK()) {
        return status.getValue().getTag();
    }

    return status.getStatus();
}

Status CatalogManagerLegacy::getAllShards(vector<ShardType>* shards) {
    ScopedDbConnection conn(_configServerConnectionString, 30.0);
    std::unique_ptr<DBClientCursor> cursor(
        _safeCursor(conn->query(ShardType::ConfigNS, BSONObj())));
    while (cursor->more()) {
        BSONObj shardObj = cursor->nextSafe();

        StatusWith<ShardType> shardRes = ShardType::fromBSON(shardObj);
        if (!shardRes.isOK()) {
            shards->clear();
            conn.done();
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Failed to parse shard with id ("
                                        << shardObj[ShardType::name()].toString()
                                        << "): " << shardRes.getStatus().toString());
        }

        shards->push_back(shardRes.getValue());
    }
    conn.done();

    return Status::OK();
}

bool CatalogManagerLegacy::isShardHost(const ConnectionString& connectionString) {
    return _getShardCount(BSON(ShardType::host(connectionString.toString())));
}

bool CatalogManagerLegacy::runUserManagementWriteCommand(const string& commandName,
                                                         const string& dbname,
                                                         const BSONObj& cmdObj,
                                                         BSONObjBuilder* result) {
    DBClientMultiCommand dispatcher;
    for (const ConnectionString& configServer : _configServers) {
        dispatcher.addCommand(configServer, dbname, cmdObj);
    }

    auto scopedDistLock = getDistLockManager()->lock("authorizationData", commandName, Seconds{5});
    if (!scopedDistLock.isOK()) {
        return Command::appendCommandStatus(*result, scopedDistLock.getStatus());
    }

    dispatcher.sendAll();

    BSONObj responseObj;

    Status prevStatus{Status::OK()};
    Status currStatus{Status::OK()};

    BSONObjBuilder responses;
    unsigned failedCount = 0;
    bool sameError = true;
    while (dispatcher.numPending() > 0) {
        ConnectionString host;
        RawBSONSerializable responseCmdSerial;

        Status dispatchStatus = dispatcher.recvAny(&host, &responseCmdSerial);

        if (!dispatchStatus.isOK()) {
            return Command::appendCommandStatus(*result, dispatchStatus);
        }

        responseObj = responseCmdSerial.toBSON();
        responses.append(host.toString(), responseObj);

        currStatus = Command::getStatusFromCommandResult(responseObj);
        if (!currStatus.isOK()) {
            // same error <=> adjacent error statuses are the same
            if (failedCount > 0 && prevStatus != currStatus) {
                sameError = false;
            }
            failedCount++;
            prevStatus = currStatus;
        }
    }

    if (failedCount == 0) {
        result->appendElements(responseObj);
        return true;
    }

    // if the command succeeds on at least one config server and fails on at least one,
    // manual intervention is required
    if (failedCount < _configServers.size()) {
        Status status(ErrorCodes::ManualInterventionRequired,
                      str::stream() << "Config write was not consistent - "
                                    << "user management command failed on at least one "
                                    << "config server but passed on at least one other. "
                                    << "Manual intervention may be required. "
                                    << "Config responses: " << responses.obj().toString());
        return Command::appendCommandStatus(*result, status);
    }

    if (sameError) {
        result->appendElements(responseObj);
        return false;
    }

    Status status(ErrorCodes::ManualInterventionRequired,
                  str::stream() << "Config write was not consistent - "
                                << "user management command produced inconsistent results. "
                                << "Manual intervention may be required. "
                                << "Config responses: " << responses.obj().toString());
    return Command::appendCommandStatus(*result, status);
}

bool CatalogManagerLegacy::runReadCommand(const string& dbname,
                                          const BSONObj& cmdObj,
                                          BSONObjBuilder* result) {
    try {
        // let SyncClusterConnection handle connecting to the first config server
        // that is reachable and returns data
        ScopedDbConnection conn(_configServerConnectionString, 30);

        BSONObj cmdResult;
        const bool ok = conn->runCommand(dbname, cmdObj, cmdResult);
        result->appendElements(cmdResult);
        conn.done();
        return ok;
    } catch (const DBException& ex) {
        return Command::appendCommandStatus(*result, ex.toStatus());
    }
}

Status CatalogManagerLegacy::applyChunkOpsDeprecated(const BSONArray& updateOps,
                                                     const BSONArray& preCondition) {
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition);
    BSONObj cmdResult;
    try {
        ScopedDbConnection conn(_configServerConnectionString, 30);
        conn->runCommand("config", cmd, cmdResult);
        conn.done();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Status status = Command::getStatusFromCommandResult(cmdResult);
    if (!status.isOK()) {
        string errMsg(str::stream() << "Unable to save chunk ops. Command: " << cmd
                                    << ". Result: " << cmdResult);

        return Status(status.code(), errMsg);
    }

    return Status::OK();
}

void CatalogManagerLegacy::writeConfigServerDirect(const BatchedCommandRequest& request,
                                                   BatchedCommandResponse* response) {
    // check if config servers are consistent
    if (!_isConsistentFromLastCheck()) {
        toBatchError(Status(ErrorCodes::ConfigServersInconsistent,
                            "Data inconsistency detected amongst config servers"),
                     response);
        return;
    }

    // We only support batch sizes of one for config writes
    if (request.sizeWriteOps() != 1) {
        toBatchError(Status(ErrorCodes::InvalidOptions,
                            str::stream() << "Writes to config servers must have batch size of 1, "
                                          << "found " << request.sizeWriteOps()),
                     response);

        return;
    }

    // We only support {w: 0}, {w: 1}, and {w: 'majority'} write concern for config writes
    if (request.isWriteConcernSet() && !validConfigWC(request.getWriteConcern())) {
        toBatchError(Status(ErrorCodes::InvalidOptions,
                            str::stream() << "Invalid write concern for write to "
                                          << "config servers: " << request.getWriteConcern()),
                     response);

        return;
    }

    DBClientMultiCommand dispatcher;
    if (_configServers.size() > 1) {
        // We can't support no-_id upserts to multiple config servers - the _ids will differ
        if (BatchedCommandRequest::containsNoIDUpsert(request)) {
            toBatchError(
                Status(ErrorCodes::InvalidOptions,
                       str::stream() << "upserts to multiple config servers must include _id"),
                response);
            return;
        }
    }

    ConfigCoordinator exec(&dispatcher, _configServerConnectionString);
    exec.executeBatch(request, response);
}

Status CatalogManagerLegacy::_checkDbDoesNotExist(const std::string& dbName,
                                                  DatabaseType* db) const {
    ScopedDbConnection conn(_configServerConnectionString, 30);

    BSONObjBuilder b;
    b.appendRegex(DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    BSONObj dbObj = conn->findOne(DatabaseType::ConfigNS, b.obj());
    conn.done();

    // If our name is exactly the same as the name we want, try loading
    // the database again.
    if (!dbObj.isEmpty() && dbObj[DatabaseType::name()].String() == dbName) {
        if (db) {
            auto parseDBStatus = DatabaseType::fromBSON(dbObj);
            if (!parseDBStatus.isOK()) {
                return parseDBStatus.getStatus();
            }

            *db = parseDBStatus.getValue();
        }

        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "database " << dbName << " already exists");
    }

    if (!dbObj.isEmpty()) {
        return Status(ErrorCodes::DatabaseDifferCase,
                      str::stream() << "can't have 2 databases that just differ on case "
                                    << " have: " << dbObj[DatabaseType::name()].String()
                                    << " want to add: " << dbName);
    }

    return Status::OK();
}

StatusWith<string> CatalogManagerLegacy::_generateNewShardName() const {
    BSONObj o;
    {
        ScopedDbConnection conn(_configServerConnectionString, 30);
        o = conn->findOne(ShardType::ConfigNS,
                          Query(fromjson("{" + ShardType::name() + ": /^shard/}"))
                              .sort(BSON(ShardType::name() << -1)));
        conn.done();
    }

    int count = 0;
    if (!o.isEmpty()) {
        string last = o[ShardType::name()].String();
        std::istringstream is(last.substr(5));
        is >> count;
        count++;
    }

    // TODO fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

size_t CatalogManagerLegacy::_getShardCount(const BSONObj& query) const {
    ScopedDbConnection conn(_configServerConnectionString, 30.0);
    long long shardCount = conn->count(ShardType::ConfigNS, query);
    conn.done();

    return shardCount;
}

DistLockManager* CatalogManagerLegacy::getDistLockManager() const {
    invariant(_distLockManager);
    return _distLockManager.get();
}

bool CatalogManagerLegacy::_checkConfigServersConsistent(const unsigned tries) const {
    if (tries <= 0)
        return false;

    unsigned firstGood = 0;
    int up = 0;
    vector<BSONObj> res;

    // The last error we saw on a config server
    string errMsg;

    for (unsigned i = 0; i < _configServers.size(); i++) {
        BSONObj result;
        std::unique_ptr<ScopedDbConnection> conn;

        try {
            conn.reset(new ScopedDbConnection(_configServers[i], 30.0));

            if (!conn->get()->runCommand(
                    "config",
                    BSON("dbhash" << 1 << "collections" << BSON_ARRAY("chunks"
                                                                      << "databases"
                                                                      << "collections"
                                                                      << "shards"
                                                                      << "version")),
                    result)) {
                errMsg = result["errmsg"].eoo() ? "" : result["errmsg"].String();
                if (!result["assertion"].eoo())
                    errMsg = result["assertion"].String();

                warning() << "couldn't check dbhash on config server " << _configServers[i]
                          << causedBy(result.toString());

                result = BSONObj();
            } else {
                result = result.getOwned();
                if (up == 0)
                    firstGood = i;
                up++;
            }
            conn->done();
        } catch (const DBException& e) {
            if (conn) {
                conn->kill();
            }

            // We need to catch DBExceptions b/c sometimes we throw them
            // instead of socket exceptions when findN fails

            errMsg = e.toString();
            warning() << " couldn't check dbhash on config server " << _configServers[i]
                      << causedBy(e);
        }
        res.push_back(result);
    }

    if (_configServers.size() == 1)
        return true;

    if (up == 0) {
        // Use a ptr to error so if empty we won't add causedby
        error() << "no config servers successfully contacted" << causedBy(&errMsg);
        return false;
    } else if (up == 1) {
        warning() << "only 1 config server reachable, continuing";
        return true;
    }

    BSONObj base = res[firstGood];
    for (unsigned i = firstGood + 1; i < res.size(); i++) {
        if (res[i].isEmpty())
            continue;

        string chunksHash1 = base.getFieldDotted("collections.chunks");
        string chunksHash2 = res[i].getFieldDotted("collections.chunks");

        string databaseHash1 = base.getFieldDotted("collections.databases");
        string databaseHash2 = res[i].getFieldDotted("collections.databases");

        string collectionsHash1 = base.getFieldDotted("collections.collections");
        string collectionsHash2 = res[i].getFieldDotted("collections.collections");

        string shardHash1 = base.getFieldDotted("collections.shards");
        string shardHash2 = res[i].getFieldDotted("collections.shards");

        string versionHash1 = base.getFieldDotted("collections.version");
        string versionHash2 = res[i].getFieldDotted("collections.version");

        if (chunksHash1 == chunksHash2 && databaseHash1 == databaseHash2 &&
            collectionsHash1 == collectionsHash2 && shardHash1 == shardHash2 &&
            versionHash1 == versionHash2) {
            continue;
        }

        warning() << "config servers " << _configServers[firstGood].toString() << " and "
                  << _configServers[i].toString() << " differ";
        if (tries <= 1) {
            error() << ": " << base["collections"].Obj() << " vs " << res[i]["collections"].Obj();
            return false;
        }

        return _checkConfigServersConsistent(tries - 1);
    }

    return true;
}

void CatalogManagerLegacy::_consistencyChecker() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_inShutdown) {
        lk.unlock();
        const bool isConsistent = _checkConfigServersConsistent();

        lk.lock();
        _consistentFromLastCheck = isConsistent;
        if (_inShutdown)
            break;
        _consistencyCheckerCV.wait_for(lk, Seconds(60));
    }
    LOG(1) << "Consistency checker thread shutting down";
}

bool CatalogManagerLegacy::_isConsistentFromLastCheck() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _consistentFromLastCheck;
}

}  // namespace mongo
