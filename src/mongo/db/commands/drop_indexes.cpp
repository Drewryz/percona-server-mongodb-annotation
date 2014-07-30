// drop_indexes.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/instance.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/log.h"

namespace mongo {

    /* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
    class CmdDropIndexes : public Command {
    public:
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "drop indexes for a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::dropIndex);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db, 
                                                     const BSONObj& cmdObj) {
            std::string toDeleteNs = db->name() + "." + cmdObj.firstElement().valuestr();
            Collection* collection = db->getCollection(opCtx, toDeleteNs);
            IndexCatalog::IndexKillCriteria criteria;

            // Get index name to drop
            BSONElement toDrop = cmdObj.getField("index");

            if (toDrop.type() == String) {
                // Kill all in-progress indexes
                if (strcmp("*", toDrop.valuestr()) == 0) {
                    criteria.ns = toDeleteNs;
                    return IndexBuilder::killMatchingIndexBuilds(collection, criteria);
                }
                // Kill an in-progress index by name
                else {
                    criteria.name = toDrop.valuestr();
                    return IndexBuilder::killMatchingIndexBuilds(collection, criteria);
                }
            }
            // Kill an in-progress index build by index key
            else if (toDrop.type() == Object) {
                criteria.key = toDrop.Obj();
                return IndexBuilder::killMatchingIndexBuilds(collection, criteria);
            }

            return std::vector<BSONObj>();
        }

        CmdDropIndexes() : Command("dropIndexes", false, "deleteIndexes") { }
        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& anObjBuilder, bool fromRepl) {
            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            bool ok = wrappedRun(txn, dbname, jsobj, errmsg, anObjBuilder);
            if (!ok) {
                return false;
            }
            if (!fromRepl)
                repl::logOp(txn, "c",(dbname + ".$cmd").c_str(), jsobj);
            wunit.commit();
            return true;
        }

        bool wrappedRun(OperationContext* txn,
                        const string& dbname,
                        BSONObj& jsobj,
                        string& errmsg,
                        BSONObjBuilder& anObjBuilder) {
            BSONElement e = jsobj.firstElement();
            const string toDeleteNs = dbname + '.' + e.valuestr();
            if (!serverGlobalParams.quiet) {
                LOG(0) << "CMD: dropIndexes " << toDeleteNs << endl;
            }

            Client::Context ctx(txn, toDeleteNs);
            Database* db = ctx.db();

            Collection* collection = db->getCollection( txn, toDeleteNs );
            if ( ! collection ) {
                errmsg = "ns not found";
                return false;
            }

            stopIndexBuilds(txn, db, jsobj);

            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            anObjBuilder.appendNumber("nIndexesWas", indexCatalog->numIndexesTotal() );


            BSONElement f = jsobj.getField("index");
            if ( f.type() == String ) {

                string indexToDelete = f.valuestr();

                if ( indexToDelete == "*" ) {
                    Status s = indexCatalog->dropAllIndexes(txn, false);
                    if ( !s.isOK() ) {
                        appendCommandStatus( anObjBuilder, s );
                        return false;
                    }
                    anObjBuilder.append("msg", "non-_id indexes dropped for collection");
                    return true;
                }

                IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName( indexToDelete );
                if ( desc == NULL ) {
                    errmsg = str::stream() << "index not found with name [" << indexToDelete << "]";
                    return false;
                }

                if ( desc->isIdIndex() ) {
                    errmsg = "cannot drop _id index";
                    return false;
                }

                Status s = indexCatalog->dropIndex(txn, desc);
                if ( !s.isOK() ) {
                    appendCommandStatus( anObjBuilder, s );
                    return false;
                }

                return true;
            }

            if ( f.type() == Object ) {
                IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByKeyPattern( f.embeddedObject() );
                if ( desc == NULL ) {
                    errmsg = "can't find index with key:";
                    errmsg += f.embeddedObject().toString();
                    return false;
                }

                if ( desc->isIdIndex() ) {
                    errmsg = "cannot drop _id index";
                    return false;
                }

                Status s = indexCatalog->dropIndex(txn, desc);
                if ( !s.isOK() ) {
                    appendCommandStatus( anObjBuilder, s );
                    return false;
                }

                return true;
            }

            errmsg = "invalid index name spec";
            return false;
        }

    } cmdDropIndexes;

    class CmdReIndex : public Command {
    public:
        virtual bool slaveOk() const { return true; }    // can reindex on a secondary
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "re-index a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::reIndex);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }
        CmdReIndex() : Command("reIndex") { }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            std::string ns = db->name() + '.' + cmdObj["reIndex"].valuestrsafe();
            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = ns;
            return IndexBuilder::killMatchingIndexBuilds(db->getCollection(opCtx, ns), criteria);
        }

        bool run(OperationContext* txn, const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            DBDirectClient db(txn);

            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();

            LOG(0) << "CMD: reIndex " << toDeleteNs << endl;

            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            Client::Context ctx(txn, toDeleteNs);

            Collection* collection = ctx.db()->getCollection( txn, toDeleteNs );

            if ( !collection ) {
                errmsg = "ns not found";
                return false;
            }

            BackgroundOperation::assertNoBgOpInProgForNs( toDeleteNs );

            std::vector<BSONObj> indexesInProg = stopIndexBuilds(txn, ctx.db(), jsobj);

            vector<BSONObj> all;
            {
                vector<string> indexNames;
                collection->getCatalogEntry()->getAllIndexes( &indexNames );
                for ( size_t i = 0; i < indexNames.size(); i++ ) {
                    const string& name = indexNames[i];
                    BSONObj spec = collection->getCatalogEntry()->getIndexSpec( name );
                    all.push_back(spec.removeField("v").getOwned());

                    const BSONObj key = spec.getObjectField("key");
                    const Status keyStatus = validateKeyPattern(key);
                    if (!keyStatus.isOK()) {
                        errmsg = str::stream()
                            << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                            << " For more info see http://dochub.mongodb.org/core/index-validation";
                        return false;
                    }
                }
            }

            result.appendNumber( "nIndexesWas", all.size() );

            {
                WriteUnitOfWork wunit(txn->recoveryUnit());
                Status s = collection->getIndexCatalog()->dropAllIndexes(txn, true);
                if ( !s.isOK() ) {
                    errmsg = "dropIndexes failed";
                    return appendCommandStatus( result, s );
                }
                wunit.commit();
            }

            MultiIndexBlock indexer(txn, collection);
            indexer.allowBackgroundBuilding();
            // do not want interruption as that will leave us without indexes.

            Status status = indexer.init(all);
            if (!status.isOK())
                return appendCommandStatus( result, status );

            status = indexer.insertAllDocumentsInCollection();
            if (!status.isOK())
                return appendCommandStatus( result, status );

            {
                WriteUnitOfWork wunit(txn->recoveryUnit());
                indexer.commit();
                wunit.commit();
            }

            result.append( "nIndexes", (int)all.size() );
            result.append( "indexes", all );

            IndexBuilder::restoreIndexes(indexesInProg);
            return true;
        }
    } cmdReIndex;


}
