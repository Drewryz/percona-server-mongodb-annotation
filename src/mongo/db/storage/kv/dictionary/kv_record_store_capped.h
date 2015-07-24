// kv_record_store_capped.h

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

#include <boost/scoped_ptr.hpp>

#include "mongo/db/storage/kv/dictionary/kv_record_store.h"
#include "mongo/db/storage/kv/dictionary/visible_id_tracker.h"

namespace mongo {

    class KVSizeStorer;

    // Like a KVRecordStore, but size is capped and inserts
    // may truncate off old records from the beginning.
    class KVRecordStoreCapped : public KVRecordStore {
    public:
        // KVRecordStore takes ownership of db
        KVRecordStoreCapped( KVDictionary *db,
                             OperationContext* opCtx,
                             StringData ns,
                             StringData ident,
                             const CollectionOptions& options,
                             KVSizeStorer *sizeStorer,
                             bool engineSupportsDocLocking);

        virtual ~KVRecordStoreCapped() { }

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota );

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const RecordId& start = RecordId(),
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD ) const;

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        // KVRecordStore is not capped, KVRecordStoreCapped is capped
        virtual bool isCapped() const { return true; }

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              RecordId end,
                                              bool inclusive);

        virtual void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            _cappedDeleteCallback = cb;
        }

        virtual bool cappedMaxDocs() const { return _cappedMaxDocs; }

        virtual bool cappedMaxSize() const { return _cappedMaxSize; }

        virtual boost::optional<RecordId> oplogStartHack(OperationContext* txn,
                                                         const RecordId& startingPosition) const;

        virtual Status oplogDiskLocRegister(OperationContext* txn,
                                            const OpTime& opTime);

    private:
        bool needsDelete(OperationContext *txn) const;

        void deleteAsNeeded(OperationContext *txn);

        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxSizeSlack; // when to start applying backpressure
        const int64_t _cappedMaxDocs;
        RecordId _lastDeletedId;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;
        boost::mutex _cappedDeleteMutex;

        const bool _engineSupportsDocLocking;
        const bool _isOplog;
        boost::scoped_ptr<VisibleIdTracker> _idTracker;
    };

} // namespace mongo
