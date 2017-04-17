/**
 *    Copyright (C) 2016 MongoDB Inc.
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

namespace mongo {
class OperationContext;
class Command;
class BSONObj;
class BSONObjBuilder;

namespace rpc {
class RequestInterface;
class ReplyBuilderInterface;
}  // namespace rpc


// Only one of these two functions will be implemented in a target binary.  They are provided in
// this header to facilitate being made friends of `Command` as their original implementations were
// both members, and defined to be the same symbol.

// Implemented in `src/mongo/s/s_only.cpp`.
void execCommandClient(OperationContext* txn,
                       Command* c,
                       int queryOptions,
                       const char* ns,
                       BSONObj& cmdObj,
                       BSONObjBuilder& result);

// Implemented in `src/mongo/db/commands/dbcommands.cpp`.
void execCommandDatabase(OperationContext* txn,
                         Command* command,
                         const rpc::RequestInterface& request,
                         rpc::ReplyBuilderInterface* replyBuilder);
}  // namespace mongo
