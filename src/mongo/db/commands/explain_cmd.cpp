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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/explain_cmd.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/explain.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    static CmdExplain cmdExplain;

    Status CmdExplain::checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
        if (Object != cmdObj.firstElement().type()) {
            return Status(ErrorCodes::BadValue, "explain command requires a nested object");
        }

        BSONObj explainObj = cmdObj.firstElement().Obj();

        Command* commToExplain = Command::findCommand(explainObj.firstElementFieldName());
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "unknown command: " << explainObj.firstElementFieldName();
            return Status(ErrorCodes::CommandNotFound, ss);
        }

        return commToExplain->checkAuthForCommand(client, dbname, explainObj);
    }

    bool CmdExplain::run(OperationContext* txn,
                         const string& dbname,
                         BSONObj& cmdObj, int options,
                         string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl) {
        // Get the verbosity.
        Explain::Verbosity verbosity = Explain::QUERY_PLANNER;
        if (!cmdObj["verbosity"].eoo()) {
            const char* verbStr = cmdObj["verbosity"].valuestrsafe();
            if (mongoutils::str::equals(verbStr, "executionStats")) {
                verbosity = Explain::EXEC_STATS;
            }
            else if (mongoutils::str::equals(verbStr, "allPlansExecution")) {
                verbosity = Explain::EXEC_ALL_PLANS;
            }
            else if (mongoutils::str::equals(verbStr, "full")) {
                verbosity = Explain::FULL;
            }
            else if (!mongoutils::str::equals(verbStr, "queryPlanner")) {
               errmsg = "verbosity string must be one of "
                        "{'queryPlanner', 'executionStats', 'allPlansExecution'}";
               return false;
            }
        }

        if (Object != cmdObj.firstElement().type()) {
            errmsg = "explain command requires a nested object";
            return false;
        }

        // This is the nested command which we are explaining.
        BSONObj explainObj = cmdObj.firstElement().Obj();

        Command* commToExplain = Command::findCommand(explainObj.firstElementFieldName());
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "unknown command: " << explainObj.firstElementFieldName();
            Status explainStatus(ErrorCodes::CommandNotFound, ss);
            return appendCommandStatus(result, explainStatus);
        }

        // Actually call the nested command's explain(...) method.
        Status explainStatus = commToExplain->explain(txn, dbname, explainObj, verbosity, &result);
        if (!explainStatus.isOK()) {
            return appendCommandStatus(result, explainStatus);
        }

        return true;
    }

} // namespace mongo
