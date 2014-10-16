// sorted_data_interface_test_rollback.cpp

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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // Insert multiple keys and verify that omitting the commit()
    // on the WriteUnitOfWork causes the changes to not become visible.
    TEST( SortedDataInterface, InsertWithoutCommit ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( true ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc1, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc2, false ) );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }
    }

    // Insert multiple keys, then unindex those same keys and verify that
    // omitting the commit() on the WriteUnitOfWork causes the changes to
    // not become visible.
    TEST( SortedDataInterface, UnindexWithoutCommit ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( sorted->unindex( opCtx.get(), key2, loc2, true ) );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( sorted->unindex( opCtx.get(), key1, loc1, true ) );
                ASSERT( sorted->unindex( opCtx.get(), key3, loc3, true ) );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }
    }

} // namespace mongo
