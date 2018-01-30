/**
 *    Copyright (C) 2009-2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/string_map.h"

namespace mongo {

class OperationContext;
class Timer;

namespace mutablebson {
class Document;
}  // namespace mutablebson

class Command;

// Various helpers unrelated to any single command or to the command registry.
// Would be a namespace, but want to keep it closed rather than open.
// Some of these may move to the BasicCommand shim if they are only for legacy implementations.
struct CommandHelpers {
    // The type of the first field in 'cmdObj' must be mongo::String. The first field is
    // interpreted as a collection name.
    static std::string parseNsFullyQualified(const std::string& dbname, const BSONObj& cmdObj);

    // The type of the first field in 'cmdObj' must be mongo::String or Symbol.
    // The first field is interpreted as a collection name.
    static NamespaceString parseNsCollectionRequired(const std::string& dbname,
                                                     const BSONObj& cmdObj);

    static NamespaceString parseNsOrUUID(OperationContext* opCtx,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj);

    static Command* findCommand(StringData name);

    // Helper for setting errmsg and ok field in command result object.
    static void appendCommandStatus(BSONObjBuilder& result,
                                    bool ok,
                                    const std::string& errmsg = {});
    // @return s.isOK()
    static bool appendCommandStatus(BSONObjBuilder& result, const Status& status);
    /**
     * Helper for setting a writeConcernError field in the command result object if
     * a writeConcern error occurs.
     *
     * @param result is the BSONObjBuilder for the command response. This function creates the
     *               writeConcernError field for the response.
     * @param awaitReplicationStatus is the status received from awaitReplication.
     * @param wcResult is the writeConcernResult object that holds other write concern information.
     *      This is primarily used for populating errInfo when a timeout occurs, and is populated
     *      by waitForWriteConcern.
     */
    static void appendCommandWCStatus(BSONObjBuilder& result,
                                      const Status& awaitReplicationStatus,
                                      const WriteConcernResult& wcResult = WriteConcernResult());
    /**
     * Appends passthrough fields from a cmdObj to a given request.
     */
    static BSONObj appendPassthroughFields(const BSONObj& cmdObjWithPassthroughFields,
                                           const BSONObj& request);
    /**
     * Returns a copy of 'cmdObj' with a majority writeConcern appended.
     */
    static BSONObj appendMajorityWriteConcern(const BSONObj& cmdObj);
    /**
     * Returns true if the provided argument is one that is handled by the command processing layer
     * and should generally be ignored by individual command implementations. In particular,
     * commands that fail on unrecognized arguments must not fail for any of these.
     */
    static bool isGenericArgument(StringData arg) {
        // Not including "help" since we don't pass help requests through to the command parser.
        // If that changes, it should be added. When you add to this list, consider whether you
        // should also change the filterCommandRequestForPassthrough() function.
        return arg == "$audit" ||                        //
            arg == "$client" ||                          //
            arg == "$configServerState" ||               //
            arg == "$db" ||                              //
            arg == "allowImplicitCollectionCreation" ||  //
            arg == "$oplogQueryData" ||                  //
            arg == "$queryOptions" ||                    //
            arg == "$readPreference" ||                  //
            arg == "$replData" ||                        //
            arg == "$clusterTime" ||                     //
            arg == "maxTimeMS" ||                        //
            arg == "readConcern" ||                      //
            arg == "shardVersion" ||                     //
            arg == "tracking_info" ||                    //
            arg == "writeConcern" ||                     //
            arg == "lsid" ||                             //
            arg == "txnNumber" ||                        //
            false;  // These comments tell clang-format to keep this line-oriented.
    }

    /**
     * This function checks if a command is a user management command by name.
     */
    static bool isUserManagementCommand(const std::string& name);

    /**
     * Rewrites cmdObj into a format safe to blindly forward to shards.
     *
     * This performs 2 transformations:
     * 1) $readPreference fields are moved into a subobject called $queryOptions. This matches the
     *    "wrapped" format historically used internally by mongos. Moving off of that format will be
     *    done as SERVER-29091.
     *
     * 2) Filter out generic arguments that shouldn't be blindly passed to the shards.  This is
     *    necessary because many mongos implementations of Command::run() just pass cmdObj through
     *    directly to the shards. However, some of the generic arguments fields are automatically
     *    appended in the egress layer. Removing them here ensures that they don't get duplicated.
     *
     * Ideally this function can be deleted once mongos run() implementations are more careful about
     * what they send to the shards.
     */
    static BSONObj filterCommandRequestForPassthrough(const BSONObj& cmdObj);
    static void filterCommandReplyForPassthrough(const BSONObj& reply, BSONObjBuilder* output);

    /**
     * Rewrites reply into a format safe to blindly forward from shards to clients.
     *
     * Ideally this function can be deleted once mongos run() implementations are more careful about
     * what they return from the shards.
     */
    static BSONObj filterCommandReplyForPassthrough(const BSONObj& reply);

    /**
     * Returns true if this a request for the 'help' information associated with the command.
     */
    static bool isHelpRequest(const BSONElement& helpElem);

    /**
     * Runs a command directly and returns the result. Does not do any other work normally handled
     * by command dispatch, such as checking auth, dealing with CurOp or waiting for write concern.
     * It is illegal to call this if the command does not exist.
     */
    static BSONObj runCommandDirectly(OperationContext* opCtx, const OpMsgRequest& request);

    static constexpr StringData kHelpFieldName = "help"_sd;
};

/**
 * Serves as a base for server commands. See the constructor for more details.
 */
class Command {
public:
    using CommandMap = StringMap<Command*>;
    enum class AllowedOnSecondary { kAlways, kNever, kOptIn };

    /**
     * Constructs a new command and causes it to be registered with the global commands list. It is
     * not safe to construct commands other than when the server is starting up.
     *
     * @param oldName an optional old, deprecated name for the command
     */
    Command(StringData name, StringData oldName = StringData());

    // Do not remove or relocate the definition of this "key function".
    // See https://gcc.gnu.org/wiki/VerboseDiagnostics#missing_vtable
    virtual ~Command();

    /**
     * Returns the command's name. This value never changes for the lifetime of the command.
     */
    const std::string& getName() const {
        return _name;
    }

    /**
     * Return the namespace for the command. If the first field in 'cmdObj' is of type
     * mongo::String, then that field is interpreted as the collection name, and is
     * appended to 'dbname' after a '.' character. If the first field is not of type
     * mongo::String, then 'dbname' is returned unmodified.
     */
    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const;

    /**
     * Utility that returns a ResourcePattern for the namespace returned from
     * parseNs(dbname, cmdObj).  This will be either an exact namespace resource pattern
     * or a database resource pattern, depending on whether parseNs returns a fully qualifed
     * collection name or just a database name.
     */
    virtual ResourcePattern parseResourcePattern(const std::string& dbname,
                                                 const BSONObj& cmdObj) const;

    /**
     * Used by command implementations to hint to the rpc system how much space they will need in
     * their replies.
     */
    virtual std::size_t reserveBytesForReply() const {
        return 0u;
    }

    /**
     * supportsWriteConcern returns true if this command should be parsed for a writeConcern
     * field and wait for that write concern to be satisfied after the command runs.
     *
     * @param cmd is a BSONObj representation of the command that is used to determine if the
     *            the command supports a write concern. Ex. aggregate only supports write concern
     *            when $out is provided.
     */
    virtual bool supportsWriteConcern(const BSONObj& cmd) const = 0;


    /**
     * Return true if only the admin ns has privileges to run this command.
     */
    virtual bool adminOnly() const {
        return false;
    }

    /**
     * Like adminOnly, but even stricter: we must either be authenticated for admin db,
     * or, if running without auth, on the local interface.  Used for things which
     * are so major that remote invocation may not make sense (e.g., shutdownServer).
     *
     * When localHostOnlyIfNoAuth() is true, adminOnly() must also be true.
     */
    virtual bool localHostOnlyIfNoAuth() const {
        return false;
    }

    virtual AllowedOnSecondary secondaryAllowed() const = 0;

    /**
     * Override and return fales if the command opcounters should not be incremented on
     * behalf of this command.
     */
    virtual bool shouldAffectCommandCounter() const {
        return true;
    }

    /**
     * Return true if the command requires auth.
    */
    virtual bool requiresAuth() const {
        return true;
    }

    /**
     * Generates help text for this command.
     */
    virtual std::string help() const {
        return "no help defined";
    }

    /**
     * Commands which can be explained override this method. Any operation which has a query
     * part and executes as a tree of execution stages can be explained. A command should
     * implement explain by:
     *
     *   1) Calling its custom parse function in order to parse the command. The output of
     *   this function should be a CanonicalQuery (representing the query part of the
     *   operation), and a PlanExecutor which wraps the tree of execution stages.
     *
     *   2) Calling Explain::explainStages(...) on the PlanExecutor. This is the function
     *   which knows how to convert an execution stage tree into explain output.
     */
    virtual Status explain(OperationContext* opCtx,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainOptions::Verbosity verbosity,
                           BSONObjBuilder* out) const;
    /**
     * Checks if the client associated with the given OperationContext is authorized to run this
     * command.
     */
    virtual Status checkAuthForRequest(OperationContext* opCtx, const OpMsgRequest& request) = 0;

    /**
     * Redacts "cmdObj" in-place to a form suitable for writing to logs.
     *
     * The default implementation does nothing.
     */
    virtual void redactForLogging(mutablebson::Document* cmdObj) const {}

    /**
     * Return true if a replica set secondary should go into "recovering"
     * (unreadable) state while running this command.
     */
    virtual bool maintenanceMode() const {
        return false;
    }

    /**
     * Return true if command should be permitted when a replica set secondary is in "recovering"
     * (unreadable) state.
     */
    virtual bool maintenanceOk() const {
        return true; /* assumed true prior to commit */
    }

    /**
     * Returns true if this Command supports the given readConcern level. Takes the command object
     * and the name of the database on which it was invoked as arguments, so that readConcern can be
     * conditionally rejected based on the command's parameters and/or namespace.
     *
     * If a readConcern level argument is sent to a command that returns false the command processor
     * will reject the command, returning an appropriate error message.
     *
     * Note that this is never called on mongos. Sharded commands are responsible for forwarding
     * the option to the shards as needed. We rely on the shards to fail the commands in the
     * cases where it isn't supported.
     */
    virtual bool supportsReadConcern(const std::string& dbName,
                                     const BSONObj& cmdObj,
                                     repl::ReadConcernLevel level) const {
        return level == repl::ReadConcernLevel::kLocalReadConcern;
    }

    /**
     * Returns true if command allows afterClusterTime in its readConcern. The command may not allow
     * it if it is specifically intended not to take any LockManager locks. Waiting for
     * afterClusterTime takes the MODE_IS lock.
     */
    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const {
        return true;
    }

    /**
     * Returns LogicalOp for this command.
     */
    virtual LogicalOp getLogicalOp() const {
        return LogicalOp::opCommand;
    }

    /**
     * Returns whether this operation is a read, write, or command.
     *
     * Commands which implement database read or write logic should override this to return kRead
     * or kWrite as appropriate.
     */
    enum class ReadWriteType { kCommand, kRead, kWrite };
    virtual ReadWriteType getReadWriteType() const {
        return ReadWriteType::kCommand;
    }

    /**
     * Increment counter for how many times this command has executed.
     */
    void incrementCommandsExecuted() {
        _commandsExecuted.increment();
    }

    /**
     * Increment counter for how many times this command has failed.
     */
    void incrementCommandsFailed() {
        _commandsFailed.increment();
    }

    /**
     * Runs the command.
     *
     * Forwards to enhancedRun, but additionally runs audit checks if run throws unauthorized.
     */
    bool publicRun(OperationContext* opCtx, const OpMsgRequest& request, BSONObjBuilder& result);

    /**
     * Generates a reply from the 'help' information associated with a command. The state of
     * the passed ReplyBuilder will be in kOutputDocs after calling this method.
     */
    static void generateHelpResponse(OperationContext* opCtx,
                                     rpc::ReplyBuilderInterface* replyBuilder,
                                     const Command& command);

    /**
     * Checks to see if the client executing "opCtx" is authorized to run the given command with the
     * given parameters on the given named database.
     *
     * Returns Status::OK() if the command is authorized.  Most likely returns
     * ErrorCodes::Unauthorized otherwise, but any return other than Status::OK implies not
     * authorized.
     */
    static Status checkAuthorization(Command* c,
                                     OperationContext* opCtx,
                                     const OpMsgRequest& request);

    /**
     * If true, then testing commands are available. Defaults to false.
     *
     * Testing commands should conditionally register themselves by consulting this flag:
     *
     *     MONGO_INITIALIZER(RegisterMyTestCommand)(InitializerContext* context) {
     *         if (Command::testCommandsEnabled) {
     *             // Leaked intentionally: a Command registers itself when constructed.
     *             new MyTestCommand();
     *         }
     *         return Status::OK();
     *     }
     *
     * To make testing commands available by default, change the value to true before running any
     * mongo initializers:
     *
     *     int myMain(int argc, char** argv, char** envp) {
     *         Command::testCommandsEnabled = true;
     *         ...
     *         runGlobalInitializersOrDie(argc, argv, envp);
     *         ...
     *     }
     *
     * Note: variable is defined in test_commands_enabled.cpp as a dependency hack.
     */
    static bool testCommandsEnabled;

private:
    /**
     * Runs the command.
     *
     * The default implementation verifies that request has no document sections then forwards to
     * BasicCommand::run().
     *
     * For now commands should only implement if they need access to OP_MSG-specific functionality.
     */
    virtual bool enhancedRun(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             BSONObjBuilder& result) = 0;

    // Counters for how many times this command has been executed and failed
    Counter64 _commandsExecuted;
    Counter64 _commandsFailed;

    // The full name of the command
    const std::string _name;

    // Pointers to hold the metrics tree references
    ServerStatusMetricField<Counter64> _commandsExecutedMetric;
    ServerStatusMetricField<Counter64> _commandsFailedMetric;
};

/**
 * A subclass of Command that only cares about the BSONObj body and doesn't need access to document
 * sequences.
 */
class BasicCommand : public Command {
public:
    using Command::Command;

    //
    // Interface for subclasses to implement
    //

    /**
     * run the given command
     * implement this...
     *
     * return value is true if succeeded.  if false, set errmsg text.
     */
    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) = 0;

    /**
     * Checks if the client associated with the given OperationContext is authorized to run this
     * command. Default implementation defers to checkAuthForCommand.
     */
    virtual Status checkAuthForOperation(OperationContext* opCtx,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj);

private:
    //
    // Deprecated virtual methods.
    //

    /**
     * Checks if the given client is authorized to run this command on database "dbname"
     * with the invocation described by "cmdObj".
     *
     * NOTE: Implement checkAuthForOperation that takes an OperationContext* instead.
     */
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj);

    /**
     * Appends to "*out" the privileges required to run this command on database "dbname" with
     * the invocation described by "cmdObj".  New commands shouldn't implement this, they should
     * implement checkAuthForOperation (which takes an OperationContext*), instead.
     */
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // The default implementation of addRequiredPrivileges should never be hit.
        fassertFailed(16940);
    }

    //
    // Methods provided for subclasses if they implement above interface.
    //

    /**
     * Calls run().
     */
    bool enhancedRun(OperationContext* opCtx,
                     const OpMsgRequest& request,
                     BSONObjBuilder& result) final;

    /**
     * Calls checkAuthForOperation.
     */
    Status checkAuthForRequest(OperationContext* opCtx, const OpMsgRequest& request) final;

    void uassertNoDocumentSequences(const OpMsgRequest& request);
};

/**
 * Deprecated. Do not add new subclasses.
 */
class ErrmsgCommandDeprecated : public BasicCommand {
    using BasicCommand::BasicCommand;
    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

    virtual bool errmsgRun(OperationContext* opCtx,
                           const std::string& db,
                           const BSONObj& cmdObj,
                           std::string& errmsg,
                           BSONObjBuilder& result) = 0;
};

// See the 'globalCommandRegistry()' singleton accessor.
class CommandRegistry {
public:
    using CommandMap = Command::CommandMap;

    CommandRegistry() : _unknownsMetricField("commands.<UNKNOWN>", &_unknowns) {}

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    const CommandMap& allCommands() const {
        return _commands;
    }

    void registerCommand(Command* command, StringData name, StringData oldName);

    Command* findCommand(StringData name) const;

    void incrementUnknownCommands() {
        _unknowns.increment();
    }

private:
    Counter64 _unknowns;
    ServerStatusMetricField<Counter64> _unknownsMetricField;

    CommandMap _commands;
};

// Accessor to the command registry, an always-valid singleton.
CommandRegistry* globalCommandRegistry();

}  // namespace mongo
