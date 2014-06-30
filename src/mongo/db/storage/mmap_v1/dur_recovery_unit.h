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

#include <string>
#include <vector>

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/compiler.h"

#pragma once

namespace mongo {

    class OperationContext;

    /**
     * Just pass through to getDur().
     */
    class DurRecoveryUnit : public RecoveryUnit {
    public:
        DurRecoveryUnit(OperationContext* txn);

        virtual ~DurRecoveryUnit() { }

        virtual void beginUnitOfWork();
        virtual void commitUnitOfWork();
        virtual void endUnitOfWork();

        virtual bool awaitCommit();

        virtual bool commitIfNeeded(bool force = false);

        virtual bool isCommitNeeded() const;

        virtual void* writingPtr(void* data, size_t len);

        virtual void syncDataAndTruncateJournal();

    private:
        void recordPreimage(char* data, size_t len);
        void publishChanges();
        void rollbackInnermostChanges();

        bool inAUnitOfWork() const { return !_startOfUncommittedChangesForLevel.empty(); }

        bool inOutermostUnitOfWork() const {
            return _startOfUncommittedChangesForLevel.size() == 1;
        }

        bool haveUncommitedChangesAtCurrentLevel() const {
            return _changes.size() > _startOfUncommittedChangesForLevel.back();
        }

        // The parent operation context. This pointer is not owned and it's lifetime must extend
        // past that of the DurRecoveryUnit
        OperationContext* _txn;

        // State is only used for invariant checking today. It should be deleted once we get rid of
        // nesting.
        enum State {
            NORMAL, // anything is allowed
            MUST_COMMIT, // can't rollback (will go away once we have two-phase locking).
        };
        State _state;

        struct Change {
            char* base;
            std::string preimage; // TODO consider storing out-of-line
        };

        // Changes are ordered from oldest to newest. Overlapping and duplicate regions are allowed,
        // since rollback undoes changes in reverse order.
        // TODO compare performance against a data-structure that coalesces overlapping/adjacent
        // changes.
        typedef std::vector<Change> Changes;
        Changes _changes;

        // Index of the first uncommited change in _changes for each nesting level. Index 0 in this
        // vector is always the outermost transaction and back() is always the innermost. The size()
        // is the current nesting level.
        std::vector<size_t> _startOfUncommittedChangesForLevel;
    };

}  // namespace mongo
