// tokuft_record_store_test.cpp

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

#include <boost/filesystem/operations.hpp>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/tokuft/tokuft_recovery_unit.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    class TokuFTRecordStoreHarnessHelper : public HarnessHelper {
    public:
        TokuFTRecordStoreHarnessHelper() :
            _kvHarness(KVHarnessHelper::create()),
            _engine(_kvHarness->getEngine()),
            _seq(0) {
        }

        virtual ~TokuFTRecordStoreHarnessHelper() { }

        virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() {
            std::auto_ptr<OperationContext> opCtx(new OperationContextNoop(newRecoveryUnit()));

            const std::string ident = mongoutils::str::stream() << "PerconaFTRecordStore-" << _seq++;
            Status status = _engine->createRecordStore(opCtx.get(), "ns", ident, CollectionOptions());
            invariant(status.isOK());

            RecordStore* p = _engine->getRecordStore(opCtx.get(), "ns", ident, CollectionOptions());
            return std::unique_ptr<RecordStore>(p);
        }

        virtual std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedSizeBytes = HarnessHelper::kDefaultCapedSizeBytes, 
                                                                  int64_t cappedMaxDocs = -1) {
            std::auto_ptr<OperationContext> opCtx(new OperationContextNoop(newRecoveryUnit()));

            const std::string ident = mongoutils::str::stream() << "PerconaFTRecordStore-" << _seq++;
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = cappedSizeBytes;
            options.cappedMaxDocs = cappedMaxDocs;
            Status status = _engine->createRecordStore(opCtx.get(), "ns", ident, options);
            invariant(status.isOK());

	    RecordStore* p = _engine->getRecordStore(opCtx.get(), "ns", ident, options);
            return std::unique_ptr<RecordStore>(p);
        }

	virtual RecoveryUnit* newRecoveryUnit() {
	    return _engine->newRecoveryUnit();
	}
        
        virtual bool supportsDocLocking() { return true; }
    private:
        std::auto_ptr<KVHarnessHelper> _kvHarness;
        KVEngine *_engine;
        int _seq;
    };

    std::unique_ptr<HarnessHelper> newHarnessHelper() {
        return std::unique_ptr<HarnessHelper>(new TokuFTRecordStoreHarnessHelper());
    }

} // namespace mongo
