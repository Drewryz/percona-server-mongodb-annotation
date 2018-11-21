
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <tuple>

#include "mongo/bson/timestamp.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class BSONObjBuilderValueStream;
template <typename T>
class StatusWith;

namespace repl {

/**
 * OpTime encompasses a Timestamp (which itself is composed of two 32-bit integers, which can
 * represent a time_t and a counter), and a 64-bit Term number.  OpTime can be used to
 * label every op in an oplog with a unique identifier.
 */

class OpTime {
public:
    static const char kTimestampFieldName[];
    static const char kTermFieldName[];

    // The term of an OpTime generated by old protocol version.
    static const long long kUninitializedTerm = -1;

    // The initial term after the first time upgrading from protocol version 0.
    //
    // This is also the initial term for nodes that were recently started up but have not
    // yet joined the cluster, all in protocol version 1.
    static const long long kInitialTerm;

    /**
     * Returns maximum OpTime value.
     */
    static OpTime max();

    // Default OpTime, also the smallest one.
    OpTime() : _timestamp(Timestamp(0, 0)), _term(kUninitializedTerm) {}
    OpTime(Timestamp ts, long long term) : _timestamp(std::move(ts)), _term(term) {}

    Timestamp getTimestamp() const {
        return _timestamp;
    }

    long long getSecs() const {
        return _timestamp.getSecs();
    }

    long long getTerm() const {
        return _term;
    }

    /**
     * Serializes the contents of this optime to the specified builder in the form:
     *      subObjName : { ts: <timestamp>, t: <term> }
     */
    void append(BSONObjBuilder* builder, const std::string& subObjName) const;
    BSONObj toBSON() const;

    static StatusWith<OpTime> parseFromOplogEntry(const BSONObj& obj);

    /**
     * Parses OpTime from a document in the form:
     *     { ts: <timestamp>, t: <term> }
     *
     * Throws an exception on error.
     */
    static OpTime parse(const BSONObj& obj);

    std::string toString() const;

    // Returns true when this OpTime is not yet initialized.
    bool isNull() const {
        return _timestamp.isNull();
    }

    inline bool operator==(const OpTime& rhs) const {
        // Only compare timestamp if either of the two OpTimes is generated by old protocol,
        // so that (Timestamp(), 0) == (Timestamp(), -1)
        if (_term == kUninitializedTerm || rhs._term == kUninitializedTerm) {
            return _timestamp == rhs._timestamp;
        }
        // Compare term first, then the timestamps.
        return std::tie(_term, _timestamp) == std::tie(rhs._term, rhs._timestamp);
    }

    // Since the term will be reset to 0 after upgrade protocol version -> downgrade
    // -> upgrade again, comparison of arbitrary OpTimes may not be safe. However it's safe
    // to compare OpTimes generated in same or successive replset configs.
    // Upgrade / downgrade process will make sure the last oplog entries on all nodes are from
    // the same protocol version to avoid problematic scenarios.
    inline bool operator<(const OpTime& rhs) const {
        // Only compare timestamp if either of the two OpTimes is generated by old protocol.
        if (_term == kUninitializedTerm || rhs._term == kUninitializedTerm) {
            return _timestamp < rhs._timestamp;
        }
        // Compare term first, then the timestamps.
        return std::tie(_term, _timestamp) < std::tie(rhs._term, rhs._timestamp);
    }

    inline bool operator!=(const OpTime& rhs) const {
        return !(*this == rhs);
    }

    inline bool operator<=(const OpTime& rhs) const {
        return *this < rhs || *this == rhs;
    }

    inline bool operator>(const OpTime& rhs) const {
        return !(*this <= rhs);
    }

    inline bool operator>=(const OpTime& rhs) const {
        return !(*this < rhs);
    }

    friend std::ostream& operator<<(std::ostream& out, const OpTime& opTime);

    void appendAsQuery(BSONObjBuilder* builder) const;
    BSONObj asQuery() const;

private:
    Timestamp _timestamp;
    long long _term = kInitialTerm;
};

}  // namespace repl

/**
 * Support BSONObjBuilder and BSONArrayBuilder "stream" API.
 */
BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const repl::OpTime& value);


}  // namespace mongo
