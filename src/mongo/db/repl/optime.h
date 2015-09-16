/**
*    Copyright (C) 2015 MongoDB Inc.
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

#include <tuple>

#include "mongo/bson/timestamp.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
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
    static const long long kInitialTerm = 0;

    // Default OpTime, also the smallest one.
    OpTime() = default;
    OpTime(Timestamp ts, long long term);

    Timestamp getTimestamp() const;

    long long getSecs() const;

    long long getTerm() const;

    /**
     * Serializes the contents of this optime to the specified builder in the form:
     *      subObjName : { ts: <timestamp>, t: <term> }
     */
    void append(BSONObjBuilder* builder, const std::string& subObjName) const;

    static StatusWith<OpTime> parseFromOplogEntry(const BSONObj& obj);

    std::string toString() const;

    // Returns true when this OpTime is not yet initialized.
    bool isNull() const;

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

private:
    Timestamp _timestamp;
    long long _term = kInitialTerm;
};

}  // namespace repl
}  // namespace mongo
