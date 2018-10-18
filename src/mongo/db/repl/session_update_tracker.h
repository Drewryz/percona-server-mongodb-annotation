/**
 *    Copyright (C) 2018 MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <map>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

/**
 * Keeps tracks of oplog operations that would require changes to the config.transactions and
 * provides functions to create oplog entries that would contain the writes to update
 * config.transactions.
 *
 * Assumption: it is not allowed to do transactions/retryable writes against config.transactions.
 */
class SessionUpdateTracker {
public:
    /**
     * Inspects the oplog entry and determines whether this needs to update the session info or
     * flush stored transaction information to oplog writes.
     */
    boost::optional<std::vector<OplogEntry>> updateOrFlush(const OplogEntry& entry);

    /**
     * Converts all stored transaction infos to oplog writes to config.transactions.
     * Can return an empty vector if there is nothing to flush.
     */
    std::vector<OplogEntry> flushAll();

private:
    /**
     * Analyzes the given oplog entry and determines which transactions stored so far needs to be
     * converted to oplog writes.
     *
     * Note: should only be called when oplog entry's ns target config.transactions or config.$cmd.
     */
    std::vector<OplogEntry> _flush(const OplogEntry& entry);

    /**
     * Converts stored transaction infos that has a matching transcation id with the given
     * query predicate. Can return an empty vector if there is nothing to flush.
     */
    std::vector<OplogEntry> _flushForQueryPredicate(const BSONObj& queryPredicate);

    /**
     * Extract transaction information from the oplog if any and records them internally.
     */
    void _updateSessionInfo(const OplogEntry& entry);

    std::map<UUID, OplogEntry> _sessionsToUpdate;
};

}  // namespace repl
}  // namespace mongo
