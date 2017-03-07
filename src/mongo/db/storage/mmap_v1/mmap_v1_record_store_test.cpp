// mmap_v1_record_store_test.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_test_help.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MyHarnessHelper : public RecordStoreHarnessHelper {
public:
    MyHarnessHelper() {}

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() {
        OperationContextNoop opCtx;
        auto md = stdx::make_unique<DummyRecordStoreV1MetaData>(false, 0);
        md->setUserFlag(&opCtx, CollectionOptions::Flag_NoPadding);
        return stdx::make_unique<SimpleRecordStoreV1>(&opCtx, "a.b", md.release(), &_em, false);
    }

    std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedMaxSize,
                                                      int64_t cappedMaxDocs) final {
        OperationContextNoop opCtx;
        auto md = stdx::make_unique<DummyRecordStoreV1MetaData>(true, 0);
        auto md_ptr = md.get();
        std::unique_ptr<RecordStore> rs = stdx::make_unique<CappedRecordStoreV1>(
            &opCtx, nullptr, "a.b", md.release(), &_em, false);

        LocAndSize records[] = {{}};
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        md->setCapExtent(&opCtx, DiskLoc(0, 0));
        md->setCapFirstNewRecord(&opCtx, DiskLoc().setInvalid());
        initializeV1RS(&opCtx, records, drecs, NULL, &_em, md_ptr);

        return rs;
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override {
        return stdx::make_unique<RecoveryUnitNoop>();
    }

    bool supportsDocLocking() final {
        return false;
    }

private:
    DummyExtentManager _em;
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return stdx::make_unique<MyHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace mongo
