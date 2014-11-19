// tokuft_engine_test.cpp

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

#include <boost/filesystem/operations.hpp>
#include <boost/scoped_ptr.hpp>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/tokuft/tokuft_engine.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

    class TokuFTEngineHarnessHelper : public KVHarnessHelper {
    public:
        TokuFTEngineHarnessHelper() : _dbpath("mongo-tokuft-engine-test") {
            boost::filesystem::remove_all(_dbpath.path());
            boost::filesystem::create_directory(_dbpath.path());
            _engine.reset(new TokuFTEngine(_dbpath.path()));
        }

        virtual ~TokuFTEngineHarnessHelper() {
            _doCleanShutdown();
        }

        virtual KVEngine* getEngine() { return _engine.get(); }

        virtual KVEngine* restartEngine() {
            _doCleanShutdown();
            _engine.reset(new TokuFTEngine(_dbpath.path()));
            return _engine.get();
        }

    private:
        void _doCleanShutdown() {
            if (_engine) {
                scoped_ptr<OperationContext> opCtx(new OperationContextNoop(_engine->newRecoveryUnit()));
                _engine->cleanShutdown(opCtx.get());
                _engine.reset();
            }
        }

        unittest::TempDir _dbpath;

        boost::scoped_ptr<TokuFTEngine> _engine;
    };

    KVHarnessHelper* KVHarnessHelper::create() {
        return new TokuFTEngineHarnessHelper();
    }

}
