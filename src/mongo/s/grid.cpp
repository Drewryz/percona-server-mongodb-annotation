// grid.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/grid.h"

#include <iomanip>
#include <pcrecpp.h>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using std::endl;
    using std::istringstream;
    using std::map;
    using std::ostringstream;
    using std::set;
    using std::setfill;
    using std::setw;
    using std::stringstream;
    using std::vector;

    MONGO_FP_DECLARE(neverBalance);

    Grid::Grid() : _allowLocalShard(true) {

    }

    bool Grid::initCatalogManager(const std::vector<std::string>& configHosts) {
        std::auto_ptr<CatalogManagerLegacy> cm(new CatalogManagerLegacy());
        Status status = cm->init(configHosts);
        if (!status.isOK()) {
            severe() << "Catalog manager failed to initialize " << status;
            return false;
        }

        _catalogManager.reset(cm.release());
        return true;
    }

    DBConfigPtr Grid::getDBConfig( StringData ns , bool create , const string& shardNameHint ) {
        string database = nsToDatabase( ns );

        if ( database == "config" )
            return configServerPtr;

        uassert( 15918,
                 str::stream() << "invalid database name: " << database,
                 NamespaceString::validDBName( database ) );

        boost::lock_guard<boost::mutex> l( _lock );

        DBConfigPtr& dbConfig = _databases[database];
        if( ! dbConfig ){

            dbConfig.reset(new DBConfig( database ));

            // Protect initial load from connectivity errors, otherwise we won't be able
            // to perform any task.
            bool loaded = false;
            try {
                try {
                    loaded = dbConfig->load();
                }
                catch ( const DBException& ) {
                    // Retry again, if the config server are now up, the previous call should have
                    // cleared all the bad connections in the pool and this should succeed.
                    loaded = dbConfig->load();
                }
            }
            catch( DBException& e ){
                e.addContext( "error loading initial database config information" );
                warning() << e.what() << endl;
                dbConfig.reset();
                throw;
            }

            if( ! loaded ){

                if( create ){

                    // Protect creation of initial db doc from connectivity errors
                    try{

                        // note here that cc->primary == 0.
                        log() << "couldn't find database [" << database << "] in config db" << endl;

                        {
                            // lets check case
                            ScopedDbConnection conn(configServer.modelServer(), 30);

                            BSONObjBuilder b;
                            b.appendRegex( "_id" , (string)"^" +
                                           pcrecpp::RE::QuoteMeta( database ) + "$" , "i" );
                            BSONObj dbObj = conn->findOne( DatabaseType::ConfigNS , b.obj() );
                            conn.done();

                            // If our name is exactly the same as the name we want, try loading
                            // the database again.
                            if (!dbObj.isEmpty() &&
                                dbObj[DatabaseType::name()].String() == database)
                            {
                                if (dbConfig->load()) return dbConfig;
                            }

                            // TODO: This really shouldn't fall through, but without metadata
                            // management there's no good way to make sure this works all the time
                            // when the database is getting rapidly created and dropped.
                            // For now, just do exactly what we used to do.

                            if ( ! dbObj.isEmpty() ) {
                                uasserted( DatabaseDifferCaseCode, str::stream()
                                    <<  "can't have 2 databases that just differ on case "
                                    << " have: " << dbObj[DatabaseType::name()].String()
                                    << " want to add: " << database );
                            }
                        }

                        Shard primary;
                        if ( database == "admin" ) {
                            primary = configServer.getPrimary();

                        }
                        else if ( shardNameHint.empty() ) {
                            primary = Shard::pick();

                        }
                        else {
                            // use the shard name if provided
                            Shard shard;
                            shard.reset( shardNameHint );
                            primary = shard;
                        }

                        if ( primary.ok() ) {
                            dbConfig->setPrimary( primary.getName() ); // saves 'cc' to configDB
                            log() << "\t put [" << database << "] on: " << primary << endl;
                        }
                        else {
                            uasserted( 10185 ,  "can't find a shard to put new db on" );
                        }
                    }
                    catch( DBException& e ){
                        e.addContext( "error creating initial database config information" );
                        warning() << e.what() << endl;
                        dbConfig.reset();
                        throw;
                    }
                }
                else {
                    dbConfig.reset();
                }
            }
        }

        return dbConfig;
    }

    void Grid::removeDB( const std::string& database ) {
        uassert( 10186 ,  "removeDB expects db name" , database.find( '.' ) == string::npos );
        boost::lock_guard<boost::mutex> l( _lock );
        _databases.erase( database );

    }

    void Grid::removeDBIfExists(const DBConfig& database) {
        boost::lock_guard<boost::mutex> l(_lock);

        map<string, DBConfigPtr>::iterator it = _databases.find(database.name());
        if (it != _databases.end() && it->second.get() == &database) {
            _databases.erase(it);
            log() << "erased database " << database.name() << " from local registry";
        }
        else {
            log() << database.name() << "already erased from local registry";
        }
    }

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ) {
        _allowLocalShard = allow;
    }

    bool Grid::addShard( string* name , const ConnectionString& servers , long long maxSize , string& errMsg ) {
        // name can be NULL, so provide a dummy one here to avoid testing it elsewhere
        string nameInternal;
        if ( ! name ) {
            name = &nameInternal;
        }

        ReplicaSetMonitorPtr rsMonitor;

        // Check whether the host (or set) exists and run several sanity checks on this request.
        // There are two set of sanity checks: making sure adding this particular shard is consistent
        // with the replica set state (if it exists) and making sure this shards databases can be
        // brought into the grid without conflict.

        vector<string> dbNames;
        try {
            ScopedDbConnection newShardConn(servers.toString());
            newShardConn->getLastError();

            if ( newShardConn->type() == ConnectionString::SYNC ) {
                newShardConn.done();
                errMsg = "can't use sync cluster as a shard.  for replica set, have to use <setname>/<server1>,<server2>,...";
                return false;
            }
            
            BSONObj resIsMongos;
            bool ok = newShardConn->runCommand( "admin" , BSON( "isdbgrid" << 1 ) , resIsMongos );

            // should return ok=0, cmd not found if it's a normal mongod
            if ( ok ) {
                errMsg = "can't add a mongos process as a shard";
                newShardConn.done();
                return false;
            }

            BSONObj resIsMaster;
            ok =  newShardConn->runCommand( "admin" , BSON( "isMaster" << 1 ) , resIsMaster );
            if ( !ok ) {
                ostringstream ss;
                ss << "failed running isMaster: " << resIsMaster;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard has only one host, make sure it is not part of a replica set
            string setName = resIsMaster["setName"].str();
            string commandSetName = servers.getSetName();
            if ( commandSetName.empty() && ! setName.empty() ) {
                ostringstream ss;
                ss << "host is part of set " << setName << ", use replica set url format <setname>/<server1>,<server2>,....";
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }
            if ( !commandSetName.empty() && setName.empty() ) {
                ostringstream ss;
                ss << "host did not return a set name, is the replica set still initializing? " << resIsMaster;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard is part of replica set, make sure it is the right one
            if ( ! commandSetName.empty() && ( commandSetName != setName ) ) {
                ostringstream ss;
                ss << "host is part of a different set: " << setName;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            if( setName.empty() ) { 
                // check this isn't a --configsvr
                BSONObj res;
                bool ok = newShardConn->runCommand("admin",BSON("replSetGetStatus"<<1),res);
                ostringstream ss;
                if( !ok && res["info"].type() == String && res["info"].String() == "configsvr" ) {
                    errMsg = "the specified mongod is a --configsvr and should thus not be a shard server";
                    newShardConn.done();
                    return false;
                }                
            }

            // if the shard is part of a replica set, make sure all the hosts mentioned in 'servers' are part of
            // the set. It is fine if not all members of the set are present in 'servers'.
            bool foundAll = true;
            string offendingHost;
            if ( ! commandSetName.empty() ) {
                set<string> hostSet;
                BSONObjIterator iter( resIsMaster["hosts"].Obj() );
                while ( iter.more() ) {
                    hostSet.insert( iter.next().String() ); // host:port
                }
                if ( resIsMaster["passives"].isABSONObj() ) {
                    BSONObjIterator piter( resIsMaster["passives"].Obj() );
                    while ( piter.more() ) {
                        hostSet.insert( piter.next().String() ); // host:port
                    }
                }
                if ( resIsMaster["arbiters"].isABSONObj() ) {
                    BSONObjIterator piter( resIsMaster["arbiters"].Obj() );
                    while ( piter.more() ) {
                        hostSet.insert( piter.next().String() ); // host:port
                    }
                }

                vector<HostAndPort> hosts = servers.getServers();
                for ( size_t i = 0 ; i < hosts.size() ; i++ ) {
                    if (!hosts[i].hasPort()) {
                        hosts[i] = HostAndPort(hosts[i].host(), hosts[i].port());
                    }
                    string host = hosts[i].toString(); // host:port
                    if ( hostSet.find( host ) == hostSet.end() ) {
                        offendingHost = host;
                        foundAll = false;
                        break;
                    }
                }
            }
            if ( ! foundAll ) {
                ostringstream ss;
                ss << "in seed list " << servers.toString() << ", host " << offendingHost
                   << " does not belong to replica set " << setName;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // shard name defaults to the name of the replica set
            if ( name->empty() && ! setName.empty() )
                *name = setName;

            // disallow adding shard replica set with name 'config'
            if (*name == "config") {
                errMsg = "use of shard replica set with name 'config' is not allowed";
                newShardConn.done();
                return false;
            }

            // In order to be accepted as a new shard, that mongod must not have any database name that exists already
            // in any other shards. If that test passes, the new shard's databases are going to be entered as
            // non-sharded db's whose primary is the newly added shard.

            BSONObj resListDB;
            ok = newShardConn->runCommand( "admin" , BSON( "listDatabases" << 1 ) , resListDB );
            if ( !ok ) {
                ostringstream ss;
                ss << "failed listing " << servers.toString() << "'s databases:" << resListDB;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            BSONObjIterator i( resListDB["databases"].Obj() );
            while ( i.more() ) {
                BSONObj dbEntry = i.next().Obj();
                const string& dbName = dbEntry["name"].String();
                if ( _isSpecialLocalDB( dbName ) ) {
                    // 'local', 'admin', and 'config' are system DBs and should be excluded here
                    continue;
                }
                else {
                    dbNames.push_back( dbName );
                }
            }

            if ( newShardConn->type() == ConnectionString::SET ) 
                rsMonitor = ReplicaSetMonitor::get( setName );

            newShardConn.done();
        }
        catch ( DBException& e ) {
            if ( servers.type() == ConnectionString::SET ) {
                ReplicaSetMonitor::remove( servers.getSetName() );
            }
            ostringstream ss;
            ss << "couldn't connect to new shard ";
            ss << e.what();
            errMsg = ss.str();
            return false;
        }

        // check that none of the existing shard candidate's db's exist elsewhere
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ) {
            DBConfigPtr config = getDBConfig( *it , false );
            if ( config.get() != NULL ) {
                ostringstream ss;
                ss << "can't add shard " << servers.toString() << " because a local database '" << *it;
                ss << "' exists in another " << config->getPrimary().toString();
                errMsg = ss.str();
                return false;
            }
        }

        // if a name for a shard wasn't provided, pick one.
        if ( name->empty() && ! _getNewShardName( name ) ) {
            errMsg = "error generating new shard name";
            return false;
        }

        // build the ConfigDB shard document
        BSONObjBuilder b;
        b.append(ShardType::name(), *name);
        b.append(ShardType::host(),
                 rsMonitor ? rsMonitor->getServerAddress() : servers.toString());
        if (maxSize > 0) {
            b.append(ShardType::maxSize(), maxSize);
        }
        BSONObj shardDoc = b.obj();

        {
            ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

            // check whether the set of hosts (or single host) is not an already a known shard
            BSONObj old = conn->findOne(ShardType::ConfigNS,
                                        BSON(ShardType::host(servers.toString())));

            if ( ! old.isEmpty() ) {
                errMsg = "host already used";
                conn.done();
                return false;
            }
            conn.done();
        }

        log() << "going to add shard: " << shardDoc << endl;

        Status result = catalogManager()->insert(ShardType::ConfigNS, shardDoc, NULL);
        if (!result.isOK()) {
            errMsg = result.reason();
            log() << "error adding shard: " << shardDoc << " err: " << errMsg;
            return false;
        }

        Shard::reloadShardInfo();

        // add all databases of the new shard
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ) {
            DBConfigPtr config = getDBConfig( *it , true , *name );
            if ( ! config ) {
                log() << "adding shard " << servers << " even though could not add database " << *it << endl;
            }
        }

        // Record in changelog
        BSONObjBuilder shardDetails;
        shardDetails.append("name", *name);
        shardDetails.append("host", servers.toString());

        grid.catalogManager()->logChange(NULL, "addShard", "", shardDetails.obj());

        return true;
    }

    bool Grid::knowAboutShard( const string& name ) const {
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
        BSONObj shard = conn->findOne(ShardType::ConfigNS, BSON(ShardType::host(name)));
        conn.done();
        return ! shard.isEmpty();
    }

    bool Grid::_getNewShardName(string* name) const {
        invariant(name);

        bool ok = false;
        int count = 0;

        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
        BSONObj o = conn->findOne(ShardType::ConfigNS,
                                  Query(fromjson("{" + ShardType::name() + ": /^shard/}"))
                                      .sort(BSON(ShardType::name() << -1 )));
        if (!o.isEmpty()) {
            string last = o[ShardType::name()].String();
            istringstream is(last.substr(5));
            is >> count;
            count++;
        }

        if (count < 9999) {
            stringstream ss;
            ss << "shard" << setfill('0') << setw(4) << count;
            *name = ss.str();
            ok = true;
        }

        conn.done();

        return ok;
    }

    /*
     * Returns whether balancing is enabled, with optional namespace "ns" parameter for balancing on a particular
     * collection.
     */

    bool Grid::shouldBalance(const SettingsType& balancerSettings) const {
        // Allow disabling the balancer for testing
        if (MONGO_FAIL_POINT(neverBalance)) return false;

        if (balancerSettings.isBalancerStoppedSet() && balancerSettings.getBalancerStopped()) {
            return false;
        }

        if (balancerSettings.isBalancerActiveWindowSet()) {
            boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
            return _inBalancingWindow(balancerSettings.getBalancerActiveWindow(), now);
        }

        return true;
    }

    bool Grid::getBalancerSettings(SettingsType* settings, string* errMsg) const {
        BSONObj balancerDoc;
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

        try {
            balancerDoc = conn->findOne(SettingsType::ConfigNS,
                                        BSON(SettingsType::key("balancer")));
            conn.done();
        }
        catch (const DBException& ex) {
            *errMsg = str::stream() << "failed to read balancer settings from " << conn.getHost()
                                    << ": " << causedBy(ex);
            return false;
        }

        return settings->parseBSON(balancerDoc, errMsg);
    }

    bool Grid::getConfigShouldBalance() const {
        SettingsType balSettings;
        string errMsg;

        if (!getBalancerSettings(&balSettings, &errMsg)) {
            warning() << errMsg;
            return false;
        }

        if (!balSettings.isKeySet()) {
            // Balancer settings doc does not exist. Default to yes.
            return true;
        }

        return shouldBalance(balSettings);
    }

    bool Grid::getCollShouldBalance(const std::string& ns) const {
        BSONObj collDoc;
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

        try {
            collDoc = conn->findOne(CollectionType::ConfigNS, BSON(CollectionType::ns(ns)));
            conn.done();
        }
        catch (const DBException& e){
            conn.kill();
            warning() << "could not determine whether balancer should be running, error getting"
                      << "config data from " << conn.getHost() << causedBy(e) << endl;
            // if anything goes wrong, we shouldn't try balancing
            return false;
        }

        return !collDoc[CollectionType::noBalance()].trueValue();
    }

    bool Grid::_inBalancingWindow( const BSONObj& balancerDoc , const boost::posix_time::ptime& now ) {
        // check the 'activeWindow' marker
        // if present, it is an interval during the day when the balancer should be active
        // { start: "08:00" , stop: "19:30" }, strftime format is %H:%M
        BSONElement windowElem = balancerDoc[SettingsType::balancerActiveWindow()];
        if ( windowElem.eoo() ) {
            return true;
        }

        // check if both 'start' and 'stop' are present
        if ( ! windowElem.isABSONObj() ) {
            warning() << "'activeWindow' format is { start: \"hh:mm\" , stop: ... }" << balancerDoc << endl;
            return true;
        }
        BSONObj intervalDoc = windowElem.Obj();
        const string start = intervalDoc["start"].str();
        const string stop = intervalDoc["stop"].str();
        if ( start.empty() || stop.empty() ) {
            warning() << "must specify both start and end of balancing window: " << intervalDoc << endl;
            return true;
        }

        // check that both 'start' and 'stop' are valid time-of-day
        boost::posix_time::ptime startTime, stopTime;
        if ( ! toPointInTime( start , &startTime ) || ! toPointInTime( stop , &stopTime ) ) {
            warning() << "cannot parse active window (use hh:mm 24hs format): " << intervalDoc << endl;
            return true;
        }

        LOG(1).stream() << "_inBalancingWindow: "
                        << " now: " << now
                        << " startTime: " << startTime
                        << " stopTime: " << stopTime;

        // allow balancing if during the activeWindow
        // note that a window may be open during the night
        if ( stopTime > startTime ) {
            if ( ( now >= startTime ) && ( now <= stopTime ) ) {
                return true;
            }
        }
        else if ( startTime > stopTime ) {
            if ( ( now >=startTime ) || ( now <= stopTime ) ) {
                return true;
            }
        }

        return false;
    }

    bool Grid::_isSpecialLocalDB( const string& dbName ) {
        return ( dbName == "local" ) || ( dbName == "admin" ) || ( dbName == "config" );
    }

    void Grid::flushConfig() {
        boost::lock_guard<boost::mutex> lk( _lock );
        _databases.clear();
    }

    BSONObj Grid::getConfigSetting( const std::string& name ) const {
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
        BSONObj result = conn->findOne( SettingsType::ConfigNS,
                                        BSON( SettingsType::key(name) ) );
        conn.done();

        return result;
    }

    Grid grid;
}
