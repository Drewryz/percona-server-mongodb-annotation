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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    struct WorkingSetMember;

    typedef long WorkingSetID;

    /**
     * All data in use by a query.  Data is passed through the stage tree by referencing the ID of
     * an element of the working set.  Stages can add elements to the working set, delete elements
     * from the working set, or mutate elements in the working set.
     */
    class WorkingSet {
    public:
        static const WorkingSetID INVALID_ID;

        WorkingSet();
        ~WorkingSet();

        /**
         * Allocate a new query result and return the ID used to get and free it.
         */
        WorkingSetID allocate();

        /**
         * Get the i-th mutable query result.
         */
        WorkingSetMember* get(const WorkingSetID& i);

        /**
         * Unallocate the i-th query result and release its resouces.
         */
        void free(const WorkingSetID& i);

        /**
         * The DiskLoc in WSM 'i' was invalidated while being processed.  Any predicates over the
         * WSM could not be fully evaluated, so the WSM may or may not satisfy them.  As such, if we
         * wish to output the WSM, we must do some clean-up work later.  Adds the WSM with id 'i' to
         * the list of flagged WSIDs.
         *
         * The WSM must be in the state OWNED_OBJ.
         */
        void flagForReview(const WorkingSetID& i);

        /**
         * Return a set of all WSIDs passed to flagForReview.
         */
        const unordered_set<WorkingSetID>& getFlagged() const;

        /**
         * Return true if the provided ID is flagged.
         */
        bool isFlagged(WorkingSetID id) const;

    private:
        typedef unordered_map<WorkingSetID, WorkingSetMember*> DataMap;

        DataMap _data;

        // The WorkingSetID returned by the next call to allocate().  Should refer to the next valid
        // ID.  IDs allocated contiguously.  Should never point at an in-use ID.  
        WorkingSetID _nextId;

        // All WSIDs invalidated during evaluation of a predicate (AND).
        unordered_set<WorkingSetID> _flagged;
    };

    /**
     * The key data extracted from an index.  Keeps track of both the key (currently a BSONObj) and
     * the index that provided the key.  The index key pattern is required to correctly interpret
     * the key.
     */
    struct IndexKeyDatum {
        IndexKeyDatum(const BSONObj& keyPattern, const BSONObj& key) : indexKeyPattern(keyPattern),
                                                                       keyData(key) { }

        // This is not owned and points into the IndexDescriptor's data.
        BSONObj indexKeyPattern;

        // This is the BSONObj for the key that we put into the index.  Owned by us.
        BSONObj keyData;
    };

    /**
     * What types of computed data can we have?
     */
    enum WorkingSetComputedDataType {
        WSM_COMPUTED_TEXT_SCORE = 0,
        WSM_COMPUTED_GEO_DISTANCE = 1,
    };

    /**
     * Data that is a computed function of a WSM.
     */
    class WorkingSetComputedData {
    public:
        WorkingSetComputedData(const WorkingSetComputedDataType type) : _type(type) { }
        virtual ~WorkingSetComputedData() { }

        WorkingSetComputedDataType type() const { return _type; }

        virtual WorkingSetComputedData* clone() const = 0;

    private:
        WorkingSetComputedDataType _type;
    };

    /**
     * The type of the data passed between query stages.  In particular:
     *
     * Index scan stages return a WorkingSetMember in the LOC_AND_IDX state.
     *
     * Collection scan stages the LOC_AND_UNOWNED_OBJ state.
     *
     * A WorkingSetMember may have any of the data above.
     */
    struct WorkingSetMember {
    private:
        MONGO_DISALLOW_COPYING(WorkingSetMember);
    public:
        WorkingSetMember();
        ~WorkingSetMember();

        enum MemberState {
            // Initial state.
            INVALID,

            // Data is from 1 or more indices.
            LOC_AND_IDX,

            // Data is from a collection scan, or data is from an index scan and was fetched.
            LOC_AND_UNOWNED_OBJ,

            // DiskLoc has been invalidated, or the obj doesn't correspond to an on-disk document
            // anymore (e.g. is a computed expression).
            OWNED_OBJ,
        };

        //
        // Core attributes
        //

        DiskLoc loc;
        BSONObj obj;
        vector<IndexKeyDatum> keyData;
        MemberState state;

        bool hasLoc() const;
        bool hasObj() const;
        bool hasOwnedObj() const;
        bool hasUnownedObj() const;

        //
        // Computed data
        //

        bool hasComputed(const WorkingSetComputedDataType type) const;
        const WorkingSetComputedData* getComputed(const WorkingSetComputedDataType type) const;
        void addComputed(WorkingSetComputedData* data);

        /**
         * getFieldDotted uses its state (obj or index data) to produce the field with the provided
         * name.
         *
         * Returns true if there is the element is in an index key or in an (owned or unowned)
         * object.  *out is set to the element if so.
         *
         * Returns false otherwise.  Returning false indicates a query planning error.
         */
        bool getFieldDotted(const string& field, BSONElement* out) const;

    private:
        unordered_map<size_t, WorkingSetComputedData*> _computed;
    };

}  // namespace mongo
