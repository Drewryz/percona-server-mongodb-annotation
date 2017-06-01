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

#include "mongo/db/audit.h"

#if PERCONA_AUDIT_ENABLED
#define MONGO_AUDIT_STUB ;
#else
#define MONGO_AUDIT_STUB \
    {}
#endif

namespace mongo {
namespace audit {

void logAuthentication(Client* client,
                       StringData mechanism,
                       const UserName& user,
                       ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logCommandAuthzCheck(Client* client,
                              const std::string& dbname,
                              const BSONObj& cmdObj,
                              Command* command,
                              ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logDeleteAuthzCheck(Client* client,
                             const NamespaceString& ns,
                             const BSONObj& pattern,
                             ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logGetMoreAuthzCheck(Client* client,
                              const NamespaceString& ns,
                              long long cursorId,
                              ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logInsertAuthzCheck(Client* client,
                             const NamespaceString& ns,
                             const BSONObj& insertedObj,
                             ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logKillCursorsAuthzCheck(Client* client,
                                  const NamespaceString& ns,
                                  long long cursorId,
                                  ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logQueryAuthzCheck(Client* client,
                            const NamespaceString& ns,
                            const BSONObj& query,
                            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logUpdateAuthzCheck(Client* client,
                             const NamespaceString& ns,
                             const BSONObj& query,
                             const BSONObj& updateObj,
                             bool isUpsert,
                             bool isMulti,
                             ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logCreateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>& roles) MONGO_AUDIT_STUB

    void logDropUser(Client* client, const UserName& username) MONGO_AUDIT_STUB

    void logDropAllUsersFromDatabase(Client* client, StringData dbname) MONGO_AUDIT_STUB

    void logUpdateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>* roles) MONGO_AUDIT_STUB

    void logGrantRolesToUser(Client* client,
                             const UserName& username,
                             const std::vector<RoleName>& roles) MONGO_AUDIT_STUB

    void logRevokeRolesFromUser(Client* client,
                                const UserName& username,
                                const std::vector<RoleName>& roles) MONGO_AUDIT_STUB

    void logCreateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>& roles,
                       const PrivilegeVector& privileges) MONGO_AUDIT_STUB

    void logUpdateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>* roles,
                       const PrivilegeVector* privileges) MONGO_AUDIT_STUB

    void logDropRole(Client* client, const RoleName& role) MONGO_AUDIT_STUB

    void logDropAllRolesFromDatabase(Client* client, StringData dbname) MONGO_AUDIT_STUB

    void logGrantRolesToRole(Client* client,
                             const RoleName& role,
                             const std::vector<RoleName>& roles) MONGO_AUDIT_STUB

    void logRevokeRolesFromRole(Client* client,
                                const RoleName& role,
                                const std::vector<RoleName>& roles) MONGO_AUDIT_STUB

    void logGrantPrivilegesToRole(Client* client,
                                  const RoleName& role,
                                  const PrivilegeVector& privileges) MONGO_AUDIT_STUB

    void logRevokePrivilegesFromRole(Client* client,
                                     const RoleName& role,
                                     const PrivilegeVector& privileges) MONGO_AUDIT_STUB

    void logReplSetReconfig(Client* client,
                            const BSONObj* oldConfig,
                            const BSONObj* newConfig) MONGO_AUDIT_STUB

    void logApplicationMessage(Client* client, StringData msg) MONGO_AUDIT_STUB

    void logShutdown(Client* client) MONGO_AUDIT_STUB

    void logCreateIndex(Client* client,
                        const BSONObj* indexSpec,
                        StringData indexname,
                        StringData nsname) MONGO_AUDIT_STUB

    void logCreateCollection(Client* client, StringData nsname) MONGO_AUDIT_STUB

    void logCreateDatabase(Client* client, StringData dbname) MONGO_AUDIT_STUB


    void logDropIndex(Client* client, StringData indexname, StringData nsname) MONGO_AUDIT_STUB

    void logDropCollection(Client* client, StringData nsname) MONGO_AUDIT_STUB

    void logDropDatabase(Client* client, StringData dbname) MONGO_AUDIT_STUB

    void logRenameCollection(Client* client, StringData source, StringData target) MONGO_AUDIT_STUB

    void logEnableSharding(Client* client, StringData dbname) MONGO_AUDIT_STUB

    void logAddShard(Client* client, StringData name, const std::string& servers, long long maxSize)
        MONGO_AUDIT_STUB

    void logRemoveShard(Client* client, StringData shardname) MONGO_AUDIT_STUB

    void logShardCollection(Client* client, StringData ns, const BSONObj& keyPattern, bool unique)
        MONGO_AUDIT_STUB

    void writeImpersonatedUsersToMetadata(OperationContext* opCtx,
                                          BSONObjBuilder* metadata) MONGO_AUDIT_STUB

    void parseAndRemoveImpersonatedUsersField(BSONObj cmdObj,
                                              std::vector<UserName>* parsedUserNames,
                                              bool* fieldIsPresent) MONGO_AUDIT_STUB

    void parseAndRemoveImpersonatedRolesField(BSONObj cmdObj,
                                              std::vector<RoleName>* parsedRoleNames,
                                              bool* fieldIsPresent) MONGO_AUDIT_STUB

}  // namespace audit
}  // namespace mongo
