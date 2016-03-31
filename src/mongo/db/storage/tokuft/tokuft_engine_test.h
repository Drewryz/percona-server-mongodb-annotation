// tokuft_engine_test.cpp

/*======
This file is part of Percona Server for MongoDB.

Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    Percona Server for MongoDB is free software: you can redistribute
    it and/or modify it under the terms of the GNU Affero General
    Public License, version 3, as published by the Free Software
    Foundation.

    Percona Server for MongoDB is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public
    License along with Percona Server for MongoDB.  If not, see
    <http://www.gnu.org/licenses/>.
======= */
#pragma once

#include <boost/filesystem/operations.hpp>
#include <boost/scoped_ptr.hpp>

#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/kv/dictionary/kv_engine_impl.h"
#include "mongo/db/storage/tokuft/tokuft_engine.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

    class TokuFTEngineHarnessHelper : public KVHarnessHelper {
    public:
        TokuFTEngineHarnessHelper();
        virtual ~TokuFTEngineHarnessHelper();

        virtual KVEngine* getEngine();

        virtual KVEngine* restartEngine();

        KVEngineImpl* getKVEngine();
    private:
        void _doCleanShutdown();
        unittest::TempDir _dbpath;
        boost::scoped_ptr<TokuFTEngine> _engine;
    };

    TokuFTEngineHarnessHelper* createTokuFTEngineHarnessHelper();
}
