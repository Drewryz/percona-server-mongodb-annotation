// heap1_recovery_unit.h

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

#pragma once

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

    class SortedDataInterface;

    class Heap1RecoveryUnit : public RecoveryUnit {
    public:
        Heap1RecoveryUnit() {
            _depth = 0;
            _rollbackPossible = true;
        }

        virtual ~Heap1RecoveryUnit();

        virtual void beginUnitOfWork();
        virtual void commitUnitOfWork();
        virtual void endUnitOfWork();

        virtual bool awaitCommit() {
            return true;
        }

        virtual void registerChange(Change* change) {
        }

        virtual void* writingPtr(void* data, size_t len) {
            return data;
        }

        virtual void syncDataAndTruncateJournal() {}

        // -------------

        void rollbackImpossible() { _rollbackPossible = false; }

        void notifyIndexInsert( SortedDataInterface* idx, const BSONObj& obj, const DiskLoc& loc );
        static void notifyIndexInsert( OperationContext* ctx, SortedDataInterface* idx,
                                       const BSONObj& obj, const DiskLoc& loc );

        void notifyIndexRemove( SortedDataInterface* idx, const BSONObj& obj, const DiskLoc& loc );
        static void notifyIndexRemove( OperationContext* ctx, SortedDataInterface* idx,
                                       const BSONObj& obj, const DiskLoc& loc );

    private:
        int _depth;
        bool _rollbackPossible;

        struct IndexInfo {
            SortedDataInterface* idx;
            BSONObj obj;
            DiskLoc loc;
        };
        std::vector<IndexInfo> _indexInserts;
        std::vector<IndexInfo> _indexRemoves;
    };

}
