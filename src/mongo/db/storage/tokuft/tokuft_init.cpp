/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 Tokutek Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/base/init.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/tokuft/tokuft_engine.h"

namespace mongo {

    class TokuFTStorageEngine : public KVStorageEngine {
        MONGO_DISALLOW_COPYING(TokuFTStorageEngine);
    public:
        TokuFTStorageEngine(const std::string &path) :
            KVStorageEngine(new TokuFTEngine(path)) {
        }

        virtual ~TokuFTStorageEngine() { }

        /**
         * TokuFT supports row-level ("document-level") locking
         */
        bool supportsDocLocking() const {
            return true;
        }
    };

    class TokuFTFactory : public StorageEngine::Factory {
    public:
        virtual ~TokuFTFactory() { }
        virtual StorageEngine *create(const StorageGlobalParams &params) const {
            return new TokuFTStorageEngine(params.dbpath);
        }
    };

    MONGO_INITIALIZER_WITH_PREREQUISITES(TokuFTStorageEngineInit,
                                         ("SetGlobalEnvironment"))
                                         (InitializerContext *context) {
        getGlobalEnvironment()->registerStorageEngine("tokuft", new TokuFTFactory());
        return Status::OK();
    }

} // namespace mongo
