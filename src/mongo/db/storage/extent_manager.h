// extent_manager.h

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

#pragma once

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/diskloc.h"

namespace mongo {

    class DataFile;
    class Record;
    class TransactionExperiment;

    struct Extent;

    /**
     * ExtentManager basics
     *  - one per database
     *  - responsible for managing <db>.# files
     *  - NOT responsible for .ns file
     *  - gives out extents
     *  - responsible for figuring out how to get a new extent
     *  - can use any method it wants to do so
     *  - this structure is NOT stored on disk
     *  - this class is NOT thread safe, locking should be above (for now)
     *
     * implementation:
     *  - ExtentManager holds a list of DataFile
     */
    class ExtentManager {
        MONGO_DISALLOW_COPYING( ExtentManager );

    public:
        ExtentManager(){}

        virtual ~ExtentManager(){}

        /**
         * deletes all state and puts back to original state
         */
        //void reset();

        /**
         * opens all current files
         */
        virtual Status init(TransactionExperiment* txn) = 0;

        virtual size_t numFiles() const = 0;
        virtual long long fileSize() const = 0;

        virtual void flushFiles( bool sync ) = 0;

        // must call Extent::reuse on the returned extent
        virtual DiskLoc allocateExtent( TransactionExperiment* txn,
                                        bool capped,
                                        int size,
                                        int quotaMax ) = 0;

        /**
         * firstExt has to be == lastExt or a chain
         */
        virtual void freeExtents( TransactionExperiment* txn,
                                  DiskLoc firstExt, DiskLoc lastExt ) = 0;

        /**
         * frees a single extent
         * ignores all fields in the Extent except: magic, myLoc, length
         */
        virtual void freeExtent( TransactionExperiment* txn, DiskLoc extent ) = 0;

        virtual void freeListStats( int* numExtents, int64_t* totalFreeSize ) const = 0;

        /**
         * @param loc - has to be for a specific Record
         * Note(erh): this sadly cannot be removed.
         * A Record DiskLoc has an offset from a file, while a RecordStore really wants an offset
         * from an extent.  This intrinsically links an original record store to the original extent
         * manager.
         */
        virtual Record* recordForV1( const DiskLoc& loc ) const = 0;

        /**
         * @param loc - has to be for a specific Record (not an Extent)
         * Note(erh) see comment on recordFor
         */
        virtual Extent* extentForV1( const DiskLoc& loc ) const = 0;

        /**
         * @param loc - has to be for a specific Record (not an Extent)
         * Note(erh) see comment on recordFor
         */
        virtual DiskLoc extentLocForV1( const DiskLoc& loc ) const = 0;

        /**
         * @param loc - has to be for a specific Extent
         */
        virtual Extent* getExtent( const DiskLoc& loc, bool doSanityCheck = true ) const = 0;

        virtual Extent* getNextExtent( Extent* ) const = 0;
        virtual Extent* getPrevExtent( Extent* ) const = 0;

        // TODO: remove
        DataFile* getFile( TransactionExperiment* txn,
                           int n,
                           int sizeNeeded = 0,
                           bool preallocateOnly = false );

        /**
         * quantizes extent size to >= min + page boundary
         */
        static int quantizeExtentSize( int size );
    };

}
