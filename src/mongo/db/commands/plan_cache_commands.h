/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/query/plan_cache.h"

namespace mongo {

    /**
     * DB commands for plan cache.
     * These are in a header to facilitate unit testing. See plan_cache_commands_test.cpp.
     */

    /**
     * PlanCacheCommand
     * Defines common attributes for all plan cache related commands
     * such as slaveOk and locktype.
     */
    class PlanCacheCommand : public Command {
    public:
        PlanCacheCommand(const std::string& name, const std::string& helpText,
                         ActionType actionType);

        /**
         * Entry point from command subsystem.
         * Implementation provides standardization of error handling
         * such as adding error code and message to BSON result.
         *
         * Do not override in derived classes.
         * Override runPlanCacheCommands instead to
         * implement plan cache command functionality.
         */

        bool run(const std::string& dbname, BSONObj& cmdObj, int options,
                 std::string& errmsg, BSONObjBuilder& result, bool fromRepl);

        /**
         * It's fine to return NONE here because plan cache commands
         * create explicit read context to access collection info cache.
         * Refer to dbcommands.cpp on how locktype() is handled.
         */
        virtual LockType locktype() const;

        virtual bool slaveOk() const;

        virtual void help(std::stringstream& ss) const;

        /**
         * Two action types defined for plan cache commands:
         * - planCacheRead
         * - planCacheWrite
         */
        virtual Status checkAuthForCommand(ClientBasic* client, const std::string& dbname,
                                           const BSONObj& cmdObj);
        /**
         * Subset of command arguments used by plan cache commands
         * Override to provide command functionality.
         * Should contain just enough logic to invoke run*Command() function
         * in plan_cache.h
         */
        virtual Status runPlanCacheCommand(const std::string& ns, BSONObj& cmdObj,
                                           BSONObjBuilder* bob) = 0;

        /**
         * Parses query shape from command object and returns plan cache key.
         */
        static Status makeCacheKey(const std::string& ns, const BSONObj& cmdObj,
                                   PlanCacheKey* keyOut);

    private:
        std::string helpText;
        ActionType actionType;
    };

    /**
     * planCacheListQueryShapes
     *
     * { planCacheListQueryShapes: <collection> }
     *
     */
    class PlanCacheListQueryShapes : public PlanCacheCommand {
    public:
        PlanCacheListQueryShapes();
        virtual Status runPlanCacheCommand(const std::string& ns, BSONObj& cmdObj, BSONObjBuilder* bob);

        /**
         * Looks up cache keys for collection's plan cache.
         * Inserts keys for query into BSON builder.
         */
        static Status list(const PlanCache& planCache, BSONObjBuilder* bob);
    };

    /**
     * planCacheClear
     *
     * { planCacheClear: <collection> }
     *
     */
    class PlanCacheClear : public PlanCacheCommand {
    public:
        PlanCacheClear();
        virtual Status runPlanCacheCommand(const std::string& ns, BSONObj& cmdObj, BSONObjBuilder* bob);

        /**
         * Clears collection's plan cache.
         */
        static Status clear(PlanCache* planCache);
    };

    /**
     * planCacheDrop
     *
     * { planCacheDrop: <collection>, key: <key> } }
     *
     */
    class PlanCacheDrop : public PlanCacheCommand {
    public:
        PlanCacheDrop();
        virtual Status runPlanCacheCommand(const std::string& ns, BSONObj& cmdObj,
                                           BSONObjBuilder* bob);

        /**
         * Drops using a cache key.
         */
        static Status drop(PlanCache* planCache,const std::string& ns,  const BSONObj& cmdObj);
    };

    /**
     * planCacheListPlans
     *
     * { planCacheListPlans: <collection>, key: <key> } }
     *
     */
    class PlanCacheListPlans : public PlanCacheCommand {
    public:
        PlanCacheListPlans();
        virtual Status runPlanCacheCommand(const std::string& ns, BSONObj& cmdObj,
                                           BSONObjBuilder* bob);

        /**
         * Displays the cached plans for a query shape.
         */
        static Status list(const PlanCache& planCache, const std::string& ns,
                           const BSONObj& cmdObj, BSONObjBuilder* bob);
    };

}  // namespace mongo
