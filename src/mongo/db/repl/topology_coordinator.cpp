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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication
#define LOG_FOR_ELECTION(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kReplicationElection)
#define LOG_FOR_HEARTBEATS(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kReplicationHeartbeats)

#include "mongo/platform/basic.h"

#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/topology_coordinator_gen.h"

#include <limits>
#include <string>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/client.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(forceSyncSourceCandidate);

// If this fail point is enabled, TopologyCoordinator::shouldChangeSyncSource() will ignore
// the option TopologyCoordinator::Options::maxSyncSourceLagSecs. The sync source will not be
// re-evaluated if it lags behind another node by more than 'maxSyncSourceLagSecs' seconds.
MONGO_FAIL_POINT_DEFINE(disableMaxSyncSourceLagSecs);

constexpr Milliseconds TopologyCoordinator::PingStats::UninitializedPingTime;

std::string TopologyCoordinator::roleToString(TopologyCoordinator::Role role) {
    switch (role) {
        case TopologyCoordinator::Role::kLeader:
            return "leader";
        case TopologyCoordinator::Role::kFollower:
            return "follower";
        case TopologyCoordinator::Role::kCandidate:
            return "candidate";
    }
    MONGO_UNREACHABLE;
}

TopologyCoordinator::~TopologyCoordinator() {}

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role) {
    return os << TopologyCoordinator::roleToString(role);
}

std::ostream& operator<<(std::ostream& os,
                         TopologyCoordinator::PrepareFreezeResponseResult result) {
    switch (result) {
        case TopologyCoordinator::PrepareFreezeResponseResult::kNoAction:
            return os << "no action";
        case TopologyCoordinator::PrepareFreezeResponseResult::kSingleNodeSelfElect:
            return os << "single node self elect";
    }
    MONGO_UNREACHABLE;
}

namespace {
template <typename T>
int indexOfIterator(const std::vector<T>& vec, typename std::vector<T>::const_iterator& it) {
    return static_cast<int>(it - vec.begin());
}

/**
 * Returns true if the only up heartbeats are auth errors.
 */
bool _hasOnlyAuthErrorUpHeartbeats(const std::vector<MemberData>& hbdata, const int selfIndex) {
    bool foundAuthError = false;
    for (std::vector<MemberData>::const_iterator it = hbdata.begin(); it != hbdata.end(); ++it) {
        if (indexOfIterator(hbdata, it) == selfIndex) {
            continue;
        }

        if (it->up()) {
            return false;
        }

        if (it->hasAuthIssue()) {
            foundAuthError = true;
        }
    }

    return foundAuthError;
}

void appendOpTime(BSONObjBuilder* bob, const char* elemName, const OpTime& opTime) {
    opTime.append(bob, elemName);
}

void appendIP(BSONObjBuilder* bob, const char* elemName, const HostAndPort& hostAndPort) {
    auto ip = hostbyname(hostAndPort.host().c_str());
    if (ip == "") {
        bob->appendNull("ip");
    } else {
        bob->append("ip", ip);
    }
}
}  // namespace

void TopologyCoordinator::PingStats::start(Date_t now) {
    _lastHeartbeatStartDate = now;
    _numFailuresSinceLastStart = 0;
    _state = HeartbeatState::TRYING;
}

void TopologyCoordinator::PingStats::hit(Milliseconds millis) {
    _state = HeartbeatState::SUCCEEDED;
    ++hitCount;

    averagePingTimeMs = averagePingTimeMs == UninitializedPingTime
        ? millis
        : Milliseconds((averagePingTimeMs * 4 + millis) / 5);
}

void TopologyCoordinator::PingStats::miss() {
    ++_numFailuresSinceLastStart;
    // Transition to 'FAILED' state if this was our last retry.
    if (_numFailuresSinceLastStart > kMaxHeartbeatRetries) {
        _state = PingStats::HeartbeatState::FAILED;
    }
}

TopologyCoordinator::TopologyCoordinator(Options options)
    : _role(Role::kFollower),
      _term(OpTime::kUninitializedTerm),
      _currentPrimaryIndex(-1),
      _forceSyncSourceIndex(-1),
      _options(std::move(options)),
      _selfIndex(-1),
      _maintenanceModeCalls(0),
      _followerMode(MemberState::RS_STARTUP2) {
    invariant(getMemberState() == MemberState::RS_STARTUP);
    // Need an entry for self in the memberHearbeatData.
    _memberData.emplace_back();
    _memberData.back().setIsSelf(true);
}

TopologyCoordinator::Role TopologyCoordinator::getRole() const {
    return _role;
}

void TopologyCoordinator::setForceSyncSourceIndex(int index) {
    invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
    _forceSyncSourceIndex = index;
}

HostAndPort TopologyCoordinator::getSyncSourceAddress() const {
    return _syncSource;
}

HostAndPort TopologyCoordinator::chooseNewSyncSource(Date_t now,
                                                     const OpTime& lastOpTimeFetched,
                                                     ChainingPreference chainingPreference) {
    // If we are not a member of the current replica set configuration, no sync source is valid.
    if (_selfIndex == -1) {
        LOG(1) << "Cannot sync from any members because we are not in the replica set config";
        return HostAndPort();
    }

    MONGO_FAIL_POINT_BLOCK(forceSyncSourceCandidate, customArgs) {
        const auto& data = customArgs.getData();
        const auto hostAndPortElem = data["hostAndPort"];
        if (!hostAndPortElem) {
            severe() << "'forceSyncSoureCandidate' parameter set with invalid host and port: "
                     << data;
            fassertFailed(50835);
        }

        const auto hostAndPort = HostAndPort(hostAndPortElem.checkAndGetStringData());
        const int syncSourceIndex = _rsConfig.findMemberIndexByHostAndPort(hostAndPort);
        if (syncSourceIndex < 0) {
            log() << "'forceSyncSourceCandidate' failed due to host and port not in "
                     "replica set config: "
                  << hostAndPort.toString();
            fassertFailed(50836);
        }


        if (_memberIsBlacklisted(_rsConfig.getMemberAt(syncSourceIndex), now)) {
            log() << "Cannot select a sync source because forced candidate is blacklisted: "
                  << hostAndPort.toString();
            _syncSource = HostAndPort();
            return _syncSource;
        }

        _syncSource = _rsConfig.getMemberAt(syncSourceIndex).getHostAndPort();
        log() << "choosing sync source candidate due to 'forceSyncSourceCandidate' parameter: "
              << _syncSource;
        std::string msg(str::stream() << "syncing from: " << _syncSource.toString()
                                      << " by 'forceSyncSourceCandidate' parameter");
        setMyHeartbeatMessage(now, msg);
        return _syncSource;
    }

    // if we have a target we've requested to sync from, use it
    if (_forceSyncSourceIndex != -1) {
        invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
        _syncSource = _rsConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort();
        _forceSyncSourceIndex = -1;
        log() << "choosing sync source candidate by request: " << _syncSource;
        std::string msg(str::stream() << "syncing from: " << _syncSource.toString()
                                      << " by request");
        setMyHeartbeatMessage(now, msg);
        return _syncSource;
    }

    // wait for 2N pings (not counting ourselves) before choosing a sync target
    int needMorePings = (_memberData.size() - 1) * 2 - _getTotalPings();

    if (needMorePings > 0) {
        static Occasionally sampler;
        if (sampler.tick()) {
            log() << "waiting for " << needMorePings << " pings from other members before syncing";
        }
        _syncSource = HostAndPort();
        return _syncSource;
    }

    // If we are only allowed to sync from the primary, set that
    if (chainingPreference == ChainingPreference::kUseConfiguration &&
        !_rsConfig.isChainingAllowed()) {
        if (_currentPrimaryIndex == -1) {
            LOG(1) << "Cannot select a sync source because chaining is"
                      " not allowed and primary is unknown/down";
            _syncSource = HostAndPort();
            return _syncSource;
        } else if (_memberIsBlacklisted(*_currentPrimaryMember(), now)) {
            LOG(1) << "Cannot select a sync source because chaining is not allowed and primary "
                      "member is blacklisted: "
                   << _currentPrimaryMember()->getHostAndPort();
            _syncSource = HostAndPort();
            return _syncSource;
        } else if (_currentPrimaryIndex == _selfIndex) {
            LOG(1)
                << "Cannot select a sync source because chaining is not allowed and we are primary";
            _syncSource = HostAndPort();
            return _syncSource;
        } else {
            _syncSource = _currentPrimaryMember()->getHostAndPort();
            log() << "chaining not allowed, choosing primary as sync source candidate: "
                  << _syncSource;
            std::string msg(str::stream() << "syncing from primary: " << _syncSource.toString());
            setMyHeartbeatMessage(now, msg);
            return _syncSource;
        }
    }

    // find the member with the lowest ping time that is ahead of me

    // choose a time that will exclude no candidates by default, in case we don't see a primary
    OpTime oldestSyncOpTime;

    // Find primary's oplog time. Reject sync candidates that are more than
    // _options.maxSyncSourceLagSecs seconds behind.
    if (_currentPrimaryIndex != -1) {
        OpTime primaryOpTime = _memberData.at(_currentPrimaryIndex).getHeartbeatAppliedOpTime();

        // Check if primaryOpTime is still close to 0 because we haven't received
        // our first heartbeat from a new primary yet.
        unsigned int maxLag =
            static_cast<unsigned int>(durationCount<Seconds>(_options.maxSyncSourceLagSecs));
        if (primaryOpTime.getSecs() >= maxLag) {
            oldestSyncOpTime =
                OpTime(Timestamp(primaryOpTime.getSecs() - maxLag, 0), primaryOpTime.getTerm());
        }
    }

    int closestIndex = -1;

    // Make two attempts, with less restrictive rules the second time.
    //
    // During the first attempt, we ignore those nodes that have a larger slave
    // delay, hidden nodes or non-voting, and nodes that are excessively behind.
    //
    // For the second attempt include those nodes, in case those are the only ones we can reach.
    //
    // This loop attempts to set 'closestIndex', to select a viable candidate.
    for (int attempts = 0; attempts < 2; ++attempts) {
        for (std::vector<MemberData>::const_iterator it = _memberData.begin();
             it != _memberData.end();
             ++it) {
            const int itIndex = indexOfIterator(_memberData, it);
            // Don't consider ourselves.
            if (itIndex == _selfIndex) {
                continue;
            }

            const MemberConfig& itMemberConfig(_rsConfig.getMemberAt(itIndex));

            // Candidate must be up to be considered.
            if (!it->up()) {
                LOG(2) << "Cannot select sync source because it is not up: "
                       << itMemberConfig.getHostAndPort();
                continue;
            }
            // Candidate must be PRIMARY or SECONDARY state to be considered.
            if (!it->getState().readable()) {
                LOG(2) << "Cannot select sync source because it is not readable: "
                       << itMemberConfig.getHostAndPort();
                continue;
            }

            // On the first attempt, we skip candidates that do not match these criteria.
            if (attempts == 0) {
                // Candidate must be a voter if we are a voter.
                if (_selfConfig().isVoter() && !itMemberConfig.isVoter()) {
                    LOG(2) << "Cannot select sync source because we are a voter and it is not: "
                           << itMemberConfig.getHostAndPort();
                    continue;
                }
                // Candidates must not be hidden.
                if (itMemberConfig.isHidden()) {
                    LOG(2) << "Cannot select sync source because it is hidden: "
                           << itMemberConfig.getHostAndPort();
                    continue;
                }
                // Candidates cannot be excessively behind.
                if (it->getHeartbeatAppliedOpTime() < oldestSyncOpTime) {
                    LOG(2) << "Cannot select sync source because it is too far behind."
                           << "Latest optime of sync candidate " << itMemberConfig.getHostAndPort()
                           << ": " << it->getHeartbeatAppliedOpTime()
                           << ", oldest acceptable optime: " << oldestSyncOpTime;
                    continue;
                }
                // Candidate must not have a configured delay larger than ours.
                if (_selfConfig().getSlaveDelay() < itMemberConfig.getSlaveDelay()) {
                    LOG(2) << "Cannot select sync source with larger slaveDelay than ours: "
                           << itMemberConfig.getHostAndPort();
                    continue;
                }
            }
            // Candidate must build indexes if we build indexes, to be considered.
            if (_selfConfig().shouldBuildIndexes()) {
                if (!itMemberConfig.shouldBuildIndexes()) {
                    LOG(2) << "Cannot select sync source with shouldBuildIndex differences: "
                           << itMemberConfig.getHostAndPort();
                    continue;
                }
            }
            // Only select a candidate that is ahead of me.
            if (it->getHeartbeatAppliedOpTime() <= lastOpTimeFetched) {
                LOG(1) << "Cannot select this sync source. Sync source must be ahead of me. "
                       << "Sync candidate: " << itMemberConfig.getHostAndPort()
                       << ", my last fetched oplog optime: " << lastOpTimeFetched.toBSON()
                       << ", latest oplog optime of sync candidate: "
                       << it->getHeartbeatAppliedOpTime().toBSON();
                continue;
            }
            // Candidate cannot be more latent than anything we've already considered.
            if ((closestIndex != -1) &&
                (_getPing(itMemberConfig.getHostAndPort()) >
                 _getPing(_rsConfig.getMemberAt(closestIndex).getHostAndPort()))) {
                LOG(2) << "Cannot select sync source with higher latency than the best candidate: "
                       << itMemberConfig.getHostAndPort();

                continue;
            }
            // Candidate cannot be blacklisted.
            if (_memberIsBlacklisted(itMemberConfig, now)) {
                LOG(1) << "Cannot select sync source which is blacklisted: "
                       << itMemberConfig.getHostAndPort();

                continue;
            }
            // This candidate has passed all tests; set 'closestIndex'
            closestIndex = itIndex;
        }
        if (closestIndex != -1)
            break;  // no need for second attempt
    }

    if (closestIndex == -1) {
        // Did not find any members to sync from
        std::string msg("could not find member to sync from");
        // Only log when we had a valid sync source before
        if (!_syncSource.empty()) {
            log() << msg << rsLog;
        }
        setMyHeartbeatMessage(now, msg);

        _syncSource = HostAndPort();
        return _syncSource;
    }
    _syncSource = _rsConfig.getMemberAt(closestIndex).getHostAndPort();
    log() << "sync source candidate: " << _syncSource;
    std::string msg(str::stream() << "syncing from: " << _syncSource.toString(), 0);
    setMyHeartbeatMessage(now, msg);
    return _syncSource;
}

bool TopologyCoordinator::_memberIsBlacklisted(const MemberConfig& memberConfig, Date_t now) const {
    std::map<HostAndPort, Date_t>::const_iterator blacklisted =
        _syncSourceBlacklist.find(memberConfig.getHostAndPort());
    if (blacklisted != _syncSourceBlacklist.end()) {
        if (blacklisted->second > now) {
            return true;
        }
    }
    return false;
}

void TopologyCoordinator::blacklistSyncSource(const HostAndPort& host, Date_t until) {
    LOG(2) << "blacklisting " << host << " until " << until.toString();
    _syncSourceBlacklist[host] = until;
}

void TopologyCoordinator::unblacklistSyncSource(const HostAndPort& host, Date_t now) {
    std::map<HostAndPort, Date_t>::iterator hostItr = _syncSourceBlacklist.find(host);
    if (hostItr != _syncSourceBlacklist.end() && now >= hostItr->second) {
        LOG(2) << "unblacklisting " << host;
        _syncSourceBlacklist.erase(hostItr);
    }
}

void TopologyCoordinator::clearSyncSourceBlacklist() {
    _syncSourceBlacklist.clear();
}

void TopologyCoordinator::prepareSyncFromResponse(const HostAndPort& target,
                                                  BSONObjBuilder* response,
                                                  Status* result) {
    response->append("syncFromRequested", target.toString());

    if (_selfIndex == -1) {
        *result = Status(ErrorCodes::NotSecondary, "Removed and uninitialized nodes do not sync");
        return;
    }

    const MemberConfig& selfConfig = _selfConfig();
    if (selfConfig.isArbiter()) {
        *result = Status(ErrorCodes::NotSecondary, "arbiters don't sync");
        return;
    }
    if (_selfIndex == _currentPrimaryIndex) {
        *result = Status(ErrorCodes::NotSecondary, "primaries don't sync");
        return;
    }

    ReplSetConfig::MemberIterator targetConfig = _rsConfig.membersEnd();
    int targetIndex = 0;
    for (ReplSetConfig::MemberIterator it = _rsConfig.membersBegin(); it != _rsConfig.membersEnd();
         ++it) {
        if (it->getHostAndPort() == target) {
            targetConfig = it;
            break;
        }
        ++targetIndex;
    }
    if (targetConfig == _rsConfig.membersEnd()) {
        *result = Status(ErrorCodes::NodeNotFound,
                         str::stream() << "Could not find member \"" << target.toString()
                                       << "\" in replica set");
        return;
    }
    if (targetIndex == _selfIndex) {
        *result = Status(ErrorCodes::InvalidOptions, "I cannot sync from myself");
        return;
    }
    if (targetConfig->isArbiter()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it is an arbiter");
        return;
    }
    if (!targetConfig->shouldBuildIndexes() && selfConfig.shouldBuildIndexes()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it does not build indexes");
        return;
    }

    if (selfConfig.isVoter() && !targetConfig->isVoter()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it is not a voter");
        return;
    }

    const MemberData& hbdata = _memberData.at(targetIndex);
    if (hbdata.hasAuthIssue()) {
        *result =
            Status(ErrorCodes::Unauthorized,
                   str::stream() << "not authorized to communicate with " << target.toString());
        return;
    }
    if (hbdata.getHealth() == 0) {
        *result =
            Status(ErrorCodes::HostUnreachable,
                   str::stream() << "I cannot reach the requested member: " << target.toString());
        return;
    }
    const OpTime lastOpApplied = getMyLastAppliedOpTime();
    if (hbdata.getHeartbeatAppliedOpTime().getSecs() + 10 < lastOpApplied.getSecs()) {
        warning() << "attempting to sync from " << target << ", but its latest opTime is "
                  << hbdata.getHeartbeatAppliedOpTime().getSecs() << " and ours is "
                  << lastOpApplied.getSecs() << " so this may not work";
        response->append("warning",
                         str::stream() << "requested member \"" << target.toString()
                                       << "\" is more than 10 seconds behind us");
        // not returning bad Status, just warning
    }

    HostAndPort prevSyncSource = getSyncSourceAddress();
    if (!prevSyncSource.empty()) {
        response->append("prevSyncTarget", prevSyncSource.toString());
    }

    setForceSyncSourceIndex(targetIndex);
    *result = Status::OK();
}

// produce a reply to a heartbeat
Status TopologyCoordinator::prepareHeartbeatResponseV1(Date_t now,
                                                       const ReplSetHeartbeatArgsV1& args,
                                                       const std::string& ourSetName,
                                                       ReplSetHeartbeatResponse* response) {
    // Verify that replica set names match
    const std::string rshb = args.getSetName();
    if (ourSetName != rshb) {
        log() << "replSet set names do not match, ours: " << ourSetName
              << "; remote node's: " << rshb;
        return Status(ErrorCodes::InconsistentReplicaSetNames,
                      str::stream() << "Our set name of " << ourSetName << " does not match name "
                                    << rshb
                                    << " reported by remote node");
    }

    const MemberState myState = getMemberState();
    if (_selfIndex == -1) {
        if (myState.removed()) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          "Our replica set configuration is invalid or does not include us");
        }
    } else {
        if (args.getSenderId() == _selfConfig().getId().getData()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Received heartbeat from member with the same "
                                           "member ID as ourself: "
                                        << args.getSenderId());
        }
    }

    response->setSetName(ourSetName);

    response->setState(myState.s);

    if (myState.primary()) {
        response->setElectionTime(_electionTime);
    }

    const OpTimeAndWallTime lastOpApplied = getMyLastAppliedOpTimeAndWallTime();
    const OpTimeAndWallTime lastOpDurable = getMyLastDurableOpTimeAndWallTime();
    response->setAppliedOpTimeAndWallTime(lastOpApplied);
    response->setDurableOpTimeAndWallTime(lastOpDurable);

    if (_currentPrimaryIndex != -1) {
        response->setPrimaryId(_rsConfig.getMemberAt(_currentPrimaryIndex).getId().getData());
    }

    response->setTerm(_term);

    if (!_syncSource.empty()) {
        response->setSyncingTo(_syncSource);
    }

    if (!_rsConfig.isInitialized()) {
        response->setConfigVersion(-2);
        return Status::OK();
    }

    const long long v = _rsConfig.getConfigVersion();
    response->setConfigVersion(v);
    // Deliver new config if caller's version is older than ours
    if (v > args.getConfigVersion()) {
        response->setConfig(_rsConfig);
    }

    // Resolve the caller's id in our Member list
    int from = -1;
    if (v == args.getConfigVersion() && args.getSenderId() != -1) {
        from = _getMemberIndex(args.getSenderId());
    }
    if (from == -1) {
        return Status::OK();
    }
    invariant(from != _selfIndex);

    auto& fromNodeData = _memberData.at(from);
    // note that we got a heartbeat from this node
    fromNodeData.setLastHeartbeatRecv(now);
    // Update liveness for sending node.
    fromNodeData.updateLiveness(now);
    return Status::OK();
}

int TopologyCoordinator::_getMemberIndex(int id) const {
    int index = 0;
    for (ReplSetConfig::MemberIterator it = _rsConfig.membersBegin(); it != _rsConfig.membersEnd();
         ++it, ++index) {
        if (it->getId() == MemberId(id)) {
            return index;
        }
    }
    return -1;
}

std::pair<ReplSetHeartbeatArgsV1, Milliseconds> TopologyCoordinator::prepareHeartbeatRequestV1(
    Date_t now, const std::string& ourSetName, const HostAndPort& target) {
    PingStats& hbStats = _pings[target];
    Milliseconds alreadyElapsed(now.asInt64() - hbStats.getLastHeartbeatStartDate().asInt64());
    if ((!_rsConfig.isInitialized()) || !hbStats.trying() ||
        (alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriodMillis())) {
        // This is either the first request ever for "target", or the heartbeat timeout has
        // passed, so we're starting a "new" heartbeat.
        hbStats.start(now);
        alreadyElapsed = Milliseconds(0);
    }
    ReplSetHeartbeatArgsV1 hbArgs;
    if (_rsConfig.isInitialized()) {
        hbArgs.setSetName(_rsConfig.getReplSetName());
        hbArgs.setConfigVersion(_rsConfig.getConfigVersion());
        if (_selfIndex >= 0) {
            const MemberConfig& me = _selfConfig();
            hbArgs.setSenderId(me.getId().getData());
            hbArgs.setSenderHost(me.getHostAndPort());
        }
        hbArgs.setTerm(_term);
    } else {
        hbArgs.setSetName(ourSetName);
        // Config version -2 is for uninitialized config.
        hbArgs.setConfigVersion(-2);
        hbArgs.setTerm(OpTime::kInitialTerm);
    }
    hbArgs.setHeartbeatVersion(1);

    const Milliseconds timeoutPeriod(
        _rsConfig.isInitialized() ? _rsConfig.getHeartbeatTimeoutPeriodMillis()
                                  : Milliseconds{ReplSetConfig::kDefaultHeartbeatTimeoutPeriod});
    const Milliseconds timeout(timeoutPeriod - alreadyElapsed);
    return std::make_pair(hbArgs, timeout);
}

HeartbeatResponseAction TopologyCoordinator::processHeartbeatResponse(
    Date_t now,
    Milliseconds networkRoundTripTime,
    const HostAndPort& target,
    const StatusWith<ReplSetHeartbeatResponse>& hbResponse) {
    const MemberState originalState = getMemberState();
    PingStats& hbStats = _pings[target];
    invariant(hbStats.getLastHeartbeatStartDate() != Date_t());
    const bool isUnauthorized = (hbResponse.getStatus().code() == ErrorCodes::Unauthorized) ||
        (hbResponse.getStatus().code() == ErrorCodes::AuthenticationFailed);
    if (hbResponse.isOK() || isUnauthorized) {
        hbStats.hit(networkRoundTripTime);
    } else {
        hbStats.miss();
    }

    // If a node is not PRIMARY and has no sync source, we increase the heartbeat rate in order
    // to help it find a sync source more quickly, which helps ensure the PRIMARY will continue to
    // see the majority of the cluster.
    //
    // Arbiters also decrease their heartbeat interval to at most half the election timeout period.
    Milliseconds heartbeatInterval = _rsConfig.getHeartbeatInterval();
    if (getMemberState().arbiter()) {
        heartbeatInterval =
            std::min(_rsConfig.getElectionTimeoutPeriod() / 2, _rsConfig.getHeartbeatInterval());
    } else if (getSyncSourceAddress().empty() && !_iAmPrimary()) {
        heartbeatInterval = std::min(_rsConfig.getElectionTimeoutPeriod() / 2,
                                     _rsConfig.getHeartbeatInterval() / 4);
    }

    const Milliseconds alreadyElapsed = now - hbStats.getLastHeartbeatStartDate();
    Date_t nextHeartbeatStartDate;
    // Determine the next heartbeat start time. If a heartbeat has not succeeded or failed, and we
    // have not used up the timeout period, we should retry.
    if (hbStats.trying() && (alreadyElapsed < _rsConfig.getHeartbeatTimeoutPeriod())) {
        // There are still retries left, let's use one.
        nextHeartbeatStartDate = now;
    } else {
        nextHeartbeatStartDate = now + heartbeatInterval;
    }

    if (hbStats.failed()) {
        LOG_FOR_HEARTBEATS(0) << "Heartbeat to " << target << " failed after "
                              << kMaxHeartbeatRetries
                              << " retries, response status: " << hbResponse.getStatus();
    }

    if (hbResponse.isOK() && hbResponse.getValue().hasConfig()) {
        const long long currentConfigVersion =
            _rsConfig.isInitialized() ? _rsConfig.getConfigVersion() : -2;
        const ReplSetConfig& newConfig = hbResponse.getValue().getConfig();
        if (newConfig.getConfigVersion() > currentConfigVersion) {
            HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeReconfigAction();
            nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
            return nextAction;
        } else {
            // Could be we got the newer version before we got the response, or the
            // target erroneously sent us one, even through it isn't newer.
            if (newConfig.getConfigVersion() < currentConfigVersion) {
                LOG(1) << "Config version from heartbeat was older than ours.";
            } else {
                LOG(2) << "Config from heartbeat response was same as ours.";
            }
            if (logger::globalLogDomain()->shouldLog(MongoLogDefaultComponent_component,
                                                     ::mongo::LogstreamBuilder::severityCast(2))) {
                LogstreamBuilder lsb = log();
                if (_rsConfig.isInitialized()) {
                    lsb << "Current config: " << _rsConfig.toBSON() << "; ";
                }
                lsb << "Config in heartbeat: " << newConfig.toBSON();
            }
        }
    }

    // Check if the heartbeat target is in our config.  If it isn't, there's nothing left to do,
    // so return early.
    if (!_rsConfig.isInitialized()) {
        HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }
    // If we're not in the config, we don't need to respond to heartbeats.
    if (_selfIndex == -1) {
        LOG(1) << "Could not find ourself in current config so ignoring heartbeat from " << target
               << " -- current config: " << _rsConfig.toBSON();
        HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }
    const int memberIndex = _rsConfig.findMemberIndexByHostAndPort(target);
    if (memberIndex == -1) {
        LOG(1) << "Could not find " << target << " in current config so ignoring --"
                                                 " current config: "
               << _rsConfig.toBSON();
        HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }

    invariant(memberIndex != _selfIndex);

    MemberData& hbData = _memberData.at(memberIndex);
    const MemberConfig member = _rsConfig.getMemberAt(memberIndex);
    bool advancedOpTime = false;
    if (!hbResponse.isOK()) {
        if (isUnauthorized) {
            hbData.setAuthIssue(now);
        }
        // If the heartbeat has failed i.e. used up all retries, then we mark the target node as
        // down.
        else if (hbStats.failed() || (alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriod())) {
            hbData.setDownValues(now, hbResponse.getStatus().reason());
        } else {
            LOG(3) << "Bad heartbeat response from " << target
                   << "; trying again; Retries left: " << (hbStats.retriesLeft()) << "; "
                   << alreadyElapsed << " have already elapsed";
        }
    } else {
        ReplSetHeartbeatResponse hbr = std::move(hbResponse.getValue());
        LOG(3) << "setUpValues: heartbeat response good for member _id:" << member.getId();
        advancedOpTime = hbData.setUpValues(now, std::move(hbr));
    }

    HeartbeatResponseAction nextAction;
    nextAction = _updatePrimaryFromHBDataV1(memberIndex, originalState, now);

    nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
    nextAction.setAdvancedOpTime(advancedOpTime);
    return nextAction;
}

bool TopologyCoordinator::haveNumNodesReachedOpTime(const OpTime& targetOpTime,
                                                    int numNodes,
                                                    bool durablyWritten) {
    // Replication progress that is for some reason ahead of us should not allow us to
    // satisfy a write concern if we aren't caught up ourselves.
    OpTime myOpTime = durablyWritten ? getMyLastDurableOpTime() : getMyLastAppliedOpTime();
    if (myOpTime < targetOpTime) {
        return false;
    }

    for (auto&& memberData : _memberData) {
        const auto isArbiter = _rsConfig.getMemberAt(memberData.getConfigIndex()).isArbiter();

        // We do not count arbiters towards the write concern.
        if (isArbiter) {
            continue;
        }

        const OpTime& memberOpTime =
            durablyWritten ? memberData.getLastDurableOpTime() : memberData.getLastAppliedOpTime();

        if (memberOpTime >= targetOpTime) {
            --numNodes;
        }

        if (numNodes <= 0) {
            return true;
        }
    }
    return false;
}

bool TopologyCoordinator::haveTaggedNodesReachedOpTime(const OpTime& opTime,
                                                       const ReplSetTagPattern& tagPattern,
                                                       bool durablyWritten) {
    ReplSetTagMatch matcher(tagPattern);
    for (auto&& memberData : _memberData) {
        const OpTime& memberOpTime =
            durablyWritten ? memberData.getLastDurableOpTime() : memberData.getLastAppliedOpTime();
        if (memberOpTime >= opTime) {
            // This node has reached the desired optime, now we need to check if it is a part
            // of the tagPattern.
            int memberIndex = memberData.getConfigIndex();
            invariant(memberIndex >= 0);
            const MemberConfig& memberConfig = _rsConfig.getMemberAt(memberIndex);
            for (MemberConfig::TagIterator it = memberConfig.tagsBegin();
                 it != memberConfig.tagsEnd();
                 ++it) {
                if (matcher.update(*it)) {
                    return true;
                }
            }
        }
    }
    return false;
}

HeartbeatResponseAction TopologyCoordinator::checkMemberTimeouts(Date_t now) {
    bool stepdown = false;
    for (int memberIndex = 0; memberIndex < static_cast<int>(_memberData.size()); memberIndex++) {
        auto& memberData = _memberData[memberIndex];
        if (!memberData.isSelf() && !memberData.lastUpdateStale() &&
            now - memberData.getLastUpdate() >= _rsConfig.getElectionTimeoutPeriod()) {
            memberData.markLastUpdateStale();
            if (_iAmPrimary()) {
                stepdown = stepdown || setMemberAsDown(now, memberIndex);
            }
        }
    }
    if (stepdown) {
        log() << "can't see a majority of the set, relinquishing primary";
        return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
    }
    return HeartbeatResponseAction::makeNoAction();
}

std::vector<HostAndPort> TopologyCoordinator::getHostsWrittenTo(const OpTime& op,
                                                                bool durablyWritten) {
    std::vector<HostAndPort> hosts;
    for (const auto& memberData : _memberData) {
        if (durablyWritten) {
            if (memberData.getLastDurableOpTime() < op) {
                continue;
            }
        } else if (memberData.getLastAppliedOpTime() < op) {
            continue;
        }

        hosts.push_back(memberData.getHostAndPort());
    }
    return hosts;
}

bool TopologyCoordinator::setMemberAsDown(Date_t now, const int memberIndex) {
    invariant(memberIndex != _selfIndex);
    invariant(memberIndex != -1);
    invariant(_currentPrimaryIndex == _selfIndex);
    MemberData& hbData = _memberData.at(memberIndex);
    hbData.setDownValues(now, "no response within election timeout period");

    if (CannotSeeMajority & _getMyUnelectableReason(now, StartElectionReason::kElectionTimeout)) {
        return true;
    }

    return false;
}

std::pair<MemberId, Date_t> TopologyCoordinator::getStalestLiveMember() const {
    Date_t earliestDate = Date_t::max();
    MemberId earliestMemberId;
    for (const auto& memberData : _memberData) {
        if (memberData.isSelf()) {
            continue;
        }
        if (memberData.lastUpdateStale()) {
            // Already stale.
            continue;
        }
        LOG(3) << "memberData lastupdate is: " << memberData.getLastUpdate();
        if (earliestDate > memberData.getLastUpdate()) {
            earliestDate = memberData.getLastUpdate();
            earliestMemberId = memberData.getMemberId();
        }
    }
    LOG(3) << "stalest member " << earliestMemberId << " date: " << earliestDate;
    return std::make_pair(earliestMemberId, earliestDate);
}

void TopologyCoordinator::resetAllMemberTimeouts(Date_t now) {
    for (auto&& memberData : _memberData)
        memberData.updateLiveness(now);
}

void TopologyCoordinator::resetMemberTimeouts(Date_t now,
                                              const stdx::unordered_set<HostAndPort>& member_set) {
    for (auto&& memberData : _memberData) {
        if (member_set.count(memberData.getHostAndPort()))
            memberData.updateLiveness(now);
    }
}

OpTime TopologyCoordinator::getMyLastAppliedOpTime() const {
    return _selfMemberData().getLastAppliedOpTime();
}

OpTimeAndWallTime TopologyCoordinator::getMyLastAppliedOpTimeAndWallTime() const {
    return {_selfMemberData().getLastAppliedOpTime(), _selfMemberData().getLastAppliedWallTime()};
}

void TopologyCoordinator::setMyLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                                            Date_t now,
                                                            bool isRollbackAllowed) {
    auto opTime = opTimeAndWallTime.opTime;
    auto& myMemberData = _selfMemberData();
    auto myLastAppliedOpTime = myMemberData.getLastAppliedOpTime();

    if (!(isRollbackAllowed || opTime == myLastAppliedOpTime)) {
        invariant(opTime > myLastAppliedOpTime);
        // In pv1, oplog entries are ordered by non-decreasing term and strictly increasing
        // timestamp. So, in pv1, its not possible for us to get opTime with higher term and
        // timestamp lesser than or equal to our current lastAppliedOptime.
        invariant(opTime.getTerm() == OpTime::kUninitializedTerm ||
                  myLastAppliedOpTime.getTerm() == OpTime::kUninitializedTerm ||
                  opTime.getTimestamp() > myLastAppliedOpTime.getTimestamp());
    }

    myMemberData.setLastAppliedOpTimeAndWallTime(opTimeAndWallTime, now);
}

OpTime TopologyCoordinator::getMyLastDurableOpTime() const {
    return _selfMemberData().getLastDurableOpTime();
}

OpTimeAndWallTime TopologyCoordinator::getMyLastDurableOpTimeAndWallTime() const {
    return {_selfMemberData().getLastDurableOpTime(), _selfMemberData().getLastDurableWallTime()};
}

void TopologyCoordinator::setMyLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                                            Date_t now,
                                                            bool isRollbackAllowed) {
    auto opTime = opTimeAndWallTime.opTime;
    auto& myMemberData = _selfMemberData();
    invariant(isRollbackAllowed || opTime >= myMemberData.getLastDurableOpTime());
    myMemberData.setLastDurableOpTimeAndWallTime(opTimeAndWallTime, now);
}

StatusWith<bool> TopologyCoordinator::setLastOptime(const UpdatePositionArgs::UpdateInfo& args,
                                                    Date_t now,
                                                    long long* configVersion) {
    if (_selfIndex == -1) {
        // Ignore updates when we're in state REMOVED.
        return Status(ErrorCodes::NotMasterOrSecondary,
                      "Received replSetUpdatePosition command but we are in state REMOVED");
    }
    invariant(_rsConfig.isInitialized());  // Can only use setLastOptime in replSet mode.

    MemberId memberId;
    try {
        memberId = MemberId(args.memberId);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (memberId == _rsConfig.getMemberAt(_selfIndex).getId()) {
        // Do not let remote nodes tell us what our optime is.
        return false;
    }

    LOG(2) << "received notification that node with memberID " << memberId
           << " in config with version " << args.cfgver
           << " has reached optime: " << args.appliedOpTime
           << " and is durable through: " << args.durableOpTime;

    if (args.cfgver != _rsConfig.getConfigVersion()) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << memberId
            << " whose config version of " << args.cfgver << " doesn't match our config version of "
            << _rsConfig.getConfigVersion();
        LOG(1) << errmsg;
        *configVersion = _rsConfig.getConfigVersion();
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    auto* memberData = _findMemberDataByMemberId(memberId.getData());
    if (!memberData) {
        invariant(!_rsConfig.findMemberByID(memberId.getData()));

        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << memberId
            << " which doesn't exist in our config";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    invariant(memberId == memberData->getMemberId());

    LOG(3) << "Node with memberID " << memberId << " currently has optime "
           << memberData->getLastAppliedOpTime() << " durable through "
           << memberData->getLastDurableOpTime() << "; updating to optime " << args.appliedOpTime
           << " and durable through " << args.durableOpTime;

    bool advancedOpTime = memberData->advanceLastAppliedOpTimeAndWallTime(
        {args.appliedOpTime, args.appliedWallTime}, now);
    advancedOpTime = memberData->advanceLastDurableOpTimeAndWallTime(
                         {args.durableOpTime, args.durableWallTime}, now) ||
        advancedOpTime;
    return advancedOpTime;
}

MemberData* TopologyCoordinator::_findMemberDataByMemberId(const int memberId) {
    const int memberIndex = _getMemberIndex(memberId);
    if (memberIndex >= 0)
        return &_memberData[memberIndex];
    return nullptr;
}

HeartbeatResponseAction TopologyCoordinator::_updatePrimaryFromHBDataV1(
    int updatedConfigIndex, const MemberState& originalState, Date_t now) {
    //
    // Updates the local notion of which remote node, if any is primary.
    // Start the priority takeover process if we are eligible.
    //

    invariant(updatedConfigIndex != _selfIndex);

    // If we are missing from the config, do not participate in primary maintenance or election.
    if (_selfIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }
    // If we are the primary, there must be no other primary, otherwise its higher term would
    // have already made us step down.
    if (_currentPrimaryIndex == _selfIndex) {
        return HeartbeatResponseAction::makeNoAction();
    }

    // Scan the member list's heartbeat data for who is primary, and update _currentPrimaryIndex.
    int primaryIndex = -1;
    for (size_t i = 0; i < _memberData.size(); i++) {
        const MemberData& member = _memberData.at(i);
        if (member.getState().primary() && member.up()) {
            if (primaryIndex == -1 || _memberData.at(primaryIndex).getTerm() < member.getTerm()) {
                primaryIndex = i;
            }
        }
    }
    _currentPrimaryIndex = primaryIndex;
    if (_currentPrimaryIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }

    // Clear last heartbeat message on ourselves.
    setMyHeartbeatMessage(now, "");

    // Takeover when the replset is stable.
    //
    // Take over the primary only if the remote primary is in the latest term I know.
    // This is done only when we get a heartbeat response from the primary.
    // Otherwise, there must be an outstanding election, which may succeed or not, but
    // the remote primary will become aware of that election eventually and step down.
    if (_memberData.at(primaryIndex).getTerm() == _term && updatedConfigIndex == primaryIndex) {

        // Don't schedule catchup takeover if catchup takeover or primary catchup is disabled.
        bool catchupTakeoverDisabled =
            ReplSetConfig::kCatchUpDisabled == _rsConfig.getCatchUpTimeoutPeriod() ||
            ReplSetConfig::kCatchUpTakeoverDisabled == _rsConfig.getCatchUpTakeoverDelay();

        bool scheduleCatchupTakeover = false;
        bool schedulePriorityTakeover = false;

        if (!catchupTakeoverDisabled && (_memberData.at(primaryIndex).getLastAppliedOpTime() <
                                         _memberData.at(_selfIndex).getLastAppliedOpTime())) {
            LOG_FOR_ELECTION(2) << "I can take over the primary due to fresher data."
                                << " Current primary index: " << primaryIndex << " in term "
                                << _memberData.at(primaryIndex).getTerm() << "."
                                << " Current primary optime: "
                                << _memberData.at(primaryIndex).getLastAppliedOpTime()
                                << " My optime: "
                                << _memberData.at(_selfIndex).getLastAppliedOpTime();
            LOG_FOR_ELECTION(4) << _getReplSetStatusString();

            scheduleCatchupTakeover = true;
        }

        if (_rsConfig.getMemberAt(primaryIndex).getPriority() <
            _rsConfig.getMemberAt(_selfIndex).getPriority()) {
            LOG_FOR_ELECTION(2) << "I can take over the primary due to higher priority."
                                << " Current primary index: " << primaryIndex << " in term "
                                << _memberData.at(primaryIndex).getTerm();
            LOG_FOR_ELECTION(4) << _getReplSetStatusString();

            schedulePriorityTakeover = true;
        }

        // Calculate rank of current node. A rank of 0 indicates that it has the highest priority.
        auto currentNodePriority = _rsConfig.getMemberAt(_selfIndex).getPriority();

        // Schedule a priority takeover early only if we know that the current node has the highest
        // priority in the replica set, has a higher priority than the primary, and is the most
        // up to date node.
        // Otherwise, prefer to schedule a catchup takeover over a priority takeover
        if (scheduleCatchupTakeover && schedulePriorityTakeover &&
            _rsConfig.calculatePriorityRank(currentNodePriority) == 0) {
            LOG_FOR_ELECTION(2)
                << "I can take over the primary because I have a higher priority, the highest "
                << "priority in the replica set, and fresher data."
                << " Current primary index: " << primaryIndex << " in term "
                << _memberData.at(primaryIndex).getTerm();
            return HeartbeatResponseAction::makePriorityTakeoverAction();
        }
        if (scheduleCatchupTakeover) {
            return HeartbeatResponseAction::makeCatchupTakeoverAction();
        }
        if (schedulePriorityTakeover) {
            return HeartbeatResponseAction::makePriorityTakeoverAction();
        }
    }
    return HeartbeatResponseAction::makeNoAction();
}

bool TopologyCoordinator::_aMajoritySeemsToBeUp() const {
    int vUp = 0;
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex || it->up()) {
            vUp += _rsConfig.getMemberAt(itIndex).getNumVotes();
        }
    }

    return vUp * 2 > _rsConfig.getTotalVotingMembers();
}

int TopologyCoordinator::_findHealthyPrimaryOfEqualOrGreaterPriority(
    const int candidateIndex) const {
    const double candidatePriority = _rsConfig.getMemberAt(candidateIndex).getPriority();
    for (auto it = _memberData.begin(); it != _memberData.end(); ++it) {
        if (!it->up() || it->getState() != MemberState::RS_PRIMARY) {
            continue;
        }
        const int itIndex = indexOfIterator(_memberData, it);
        const double priority = _rsConfig.getMemberAt(itIndex).getPriority();
        if (itIndex != candidateIndex && priority >= candidatePriority) {
            return itIndex;
        }
    }

    return -1;
}

bool TopologyCoordinator::_amIFreshEnoughForPriorityTakeover() const {
    const OpTime latestKnownOpTime = _latestKnownOpTime();

    // Rules are:
    // - If the terms don't match, we don't call for priority takeover.
    // - If our optime and the latest optime happen in different seconds, our optime must be within
    // at least priorityTakeoverFreshnessWindowSeconds seconds of the latest optime.
    // - If our optime and the latest optime happen in the same second, our optime must be within
    // at least 1000 oplog entries of the latest optime (i.e. the increment portion of the timestamp
    // must be within 1000).  This is to handle the case where a primary had its clock set far into
    // the future, took some writes, then had its clock set back.  In that case the timestamp
    // component of all future oplog entries generated will be the same, until real world time
    // passes the timestamp component of the last oplog entry.

    const OpTime ourLastOpApplied = getMyLastAppliedOpTime();
    if (ourLastOpApplied.getTerm() != latestKnownOpTime.getTerm()) {
        return false;
    }

    if (ourLastOpApplied.getTimestamp().getSecs() != latestKnownOpTime.getTimestamp().getSecs()) {
        return ourLastOpApplied.getTimestamp().getSecs() +
            gPriorityTakeoverFreshnessWindowSeconds >=
            latestKnownOpTime.getTimestamp().getSecs();
    } else {
        return ourLastOpApplied.getTimestamp().getInc() + 1000 >=
            latestKnownOpTime.getTimestamp().getInc();
    }
}

bool TopologyCoordinator::_amIFreshEnoughForCatchupTakeover() const {

    const OpTime latestKnownOpTime = _latestKnownOpTime();

    // Rules are:
    // - We must have the freshest optime of all the up nodes.
    // - We must specifically have a fresher optime than the primary (can't be equal).
    // - The term of our last applied op must be less than the current term. This ensures that no
    // writes have happened since the most recent election and that the primary is still in
    // catchup mode.

    // There is no point to a catchup takeover if we aren't the freshest node because
    // another node would immediately perform another catchup takeover when we become primary.
    const OpTime ourLastOpApplied = getMyLastAppliedOpTime();
    if (ourLastOpApplied < latestKnownOpTime) {
        return false;
    }

    if (_currentPrimaryIndex == -1) {
        return false;
    }

    // If we aren't ahead of the primary, there is no point to having a catchup takeover.
    const OpTime primaryLastOpApplied = _memberData[_currentPrimaryIndex].getLastAppliedOpTime();

    if (ourLastOpApplied <= primaryLastOpApplied) {
        return false;
    }

    // If the term of our last applied op is less than the current term, the primary didn't write
    // anything and it is still in catchup mode.
    return ourLastOpApplied.getTerm() < _term;
}

bool TopologyCoordinator::_iAmPrimary() const {
    if (_role == Role::kLeader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        invariant(_leaderMode != LeaderMode::kNotLeader);
        return true;
    }
    return false;
}

OpTime TopologyCoordinator::_latestKnownOpTime() const {
    OpTime latest = getMyLastAppliedOpTime();
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        // Ignore self
        // TODO(russotto): Simplify when heartbeat and spanning tree times are combined.
        if (it->isSelf()) {
            continue;
        }
        // Ignore down members
        if (!it->up()) {
            continue;
        }
        // Ignore removed nodes (not in config, so not valid).
        if (it->getState().removed()) {
            continue;
        }

        OpTime optime = it->getHeartbeatAppliedOpTime();

        if (optime > latest) {
            latest = optime;
        }
    }

    return latest;
}

bool TopologyCoordinator::prepareForUnconditionalStepDown() {
    if (_leaderMode == LeaderMode::kSteppingDown) {
        // Can only be processing one required stepdown at a time.
        return false;
    }
    // Heartbeat and reconfig (via cmd or heartbeat) initiated stepdowns take precedence over
    // stepdown command initiated stepdowns, so it's safe to transition from kAttemptingStepDown
    // to kSteppingDown.
    _setLeaderMode(LeaderMode::kSteppingDown);
    return true;
}

StatusWith<TopologyCoordinator::StepDownAttemptAbortFn>
TopologyCoordinator::prepareForStepDownAttempt() {
    if (_leaderMode == LeaderMode::kSteppingDown ||
        _leaderMode == LeaderMode::kAttemptingStepDown) {
        return Status{ErrorCodes::ConflictingOperationInProgress,
                      "This node is already in the process of stepping down"};
    }

    if (_leaderMode == LeaderMode::kNotLeader) {
        return Status{ErrorCodes::NotMaster, "This node is not a primary."};
    }

    invariant(_leaderMode == LeaderMode::kMaster || _leaderMode == LeaderMode::kLeaderElect);
    const auto previousLeaderMode = _leaderMode;
    _setLeaderMode(LeaderMode::kAttemptingStepDown);

    return {[this, previousLeaderMode] {
        if (_leaderMode == TopologyCoordinator::LeaderMode::kAttemptingStepDown) {
            _setLeaderMode(previousLeaderMode);
        }
    }};
}

void TopologyCoordinator::changeMemberState_forTest(const MemberState& newMemberState,
                                                    const Timestamp& electionTime) {
    invariant(_selfIndex != -1);
    if (newMemberState == getMemberState())
        return;
    switch (newMemberState.s) {
        case MemberState::RS_PRIMARY:
            _role = Role::kCandidate;
            processWinElection(OID(), electionTime);
            invariant(_role == Role::kLeader);
            break;
        case MemberState::RS_SECONDARY:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_RECOVERING:
        case MemberState::RS_STARTUP2:
            _role = Role::kFollower;
            _followerMode = newMemberState.s;
            if (_currentPrimaryIndex == _selfIndex) {
                _currentPrimaryIndex = -1;
                _setLeaderMode(LeaderMode::kNotLeader);
            }
            break;
        case MemberState::RS_STARTUP:
            updateConfig(ReplSetConfig(), -1, Date_t());
            break;
        default:
            severe() << "Cannot switch to state " << newMemberState;
            MONGO_UNREACHABLE;
    }
    if (getMemberState() != newMemberState.s) {
        severe() << "Expected to enter state " << newMemberState << " but am now in "
                 << getMemberState();
        MONGO_UNREACHABLE;
    }
    log() << newMemberState;
}

void TopologyCoordinator::setCurrentPrimary_forTest(int primaryIndex,
                                                    const Timestamp& electionTime) {
    if (primaryIndex == _selfIndex) {
        changeMemberState_forTest(MemberState::RS_PRIMARY);
    } else {
        if (_iAmPrimary()) {
            changeMemberState_forTest(MemberState::RS_SECONDARY);
        }
        if (primaryIndex != -1) {
            ReplSetHeartbeatResponse hbResponse;
            hbResponse.setState(MemberState::RS_PRIMARY);
            hbResponse.setElectionTime(electionTime);
            hbResponse.setAppliedOpTimeAndWallTime(
                {_memberData.at(primaryIndex).getHeartbeatAppliedOpTime(), Date_t() + Seconds(1)});
            hbResponse.setSyncingTo(HostAndPort());
            _memberData.at(primaryIndex)
                .setUpValues(_memberData.at(primaryIndex).getLastHeartbeat(),
                             std::move(hbResponse));
        }
        _currentPrimaryIndex = primaryIndex;
    }
}

const MemberConfig* TopologyCoordinator::_currentPrimaryMember() const {
    if (_currentPrimaryIndex == -1)
        return NULL;

    return &(_rsConfig.getMemberAt(_currentPrimaryIndex));
}

std::string TopologyCoordinator::_getReplSetStatusString() {
    // Construct a ReplSetStatusArgs using default parameters. Missing parameters will not be
    // included in the status string.
    ReplSetStatusArgs rsStatusArgs{Date_t::now(), 0U, OpTimeAndWallTime(), BSONObj(), boost::none};
    BSONObjBuilder builder;
    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    prepareStatusResponse(rsStatusArgs, &builder, &result);
    if (!result.isOK()) {
        return str::stream() << "Could not get replSetGetStatus output: " << result.toString();
    }
    return str::stream() << "Current replSetGetStatus output: " << builder.done().toString();
}

void TopologyCoordinator::prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                                                BSONObjBuilder* response,
                                                Status* result) {
    // output for each member
    std::vector<BSONObj> membersOut;
    const MemberState myState = getMemberState();
    const Date_t now = rsStatusArgs.now;
    const OpTime lastOpApplied = getMyLastAppliedOpTime();
    const Date_t lastOpAppliedWall = getMyLastAppliedOpTimeAndWallTime().wallTime;
    const OpTime lastOpDurable = getMyLastDurableOpTime();
    const Date_t lastOpDurableWall = getMyLastDurableOpTimeAndWallTime().wallTime;
    const BSONObj& initialSyncStatus = rsStatusArgs.initialSyncStatus;
    const boost::optional<Timestamp>& lastStableRecoveryTimestamp =
        rsStatusArgs.lastStableRecoveryTimestamp;
    const boost::optional<Timestamp>& lastStableCheckpointTimestampDeprecated =
        rsStatusArgs.lastStableCheckpointTimestampDeprecated;

    if (_selfIndex == -1) {
        // We're REMOVED or have an invalid config
        response->append("state", static_cast<int>(myState.s));
        response->append("stateStr", myState.toString());
        response->append("uptime", rsStatusArgs.selfUptime);

        appendOpTime(response, "optime", lastOpApplied);

        response->appendDate("optimeDate",
                             Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
        if (_maintenanceModeCalls) {
            response->append("maintenanceMode", _maintenanceModeCalls);
        }
        response->append("lastHeartbeatMessage", "");
        response->append("syncingTo", "");
        response->append("syncSourceHost", "");
        response->append("syncSourceId", -1);

        response->append("infoMessage", _getHbmsg(now));
        *result = Status(ErrorCodes::InvalidReplicaSetConfig,
                         "Our replica set config is invalid or we are not a member of it");
        return;
    }

    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex) {
            // add self
            BSONObjBuilder bb;
            bb.append("_id", _selfConfig().getId().getData());
            bb.append("name", _selfConfig().getHostAndPort().toString());
            appendIP(&bb, "ip", _selfConfig().getHostAndPort());
            bb.append("health", 1.0);
            bb.append("state", static_cast<int>(myState.s));
            bb.append("stateStr", myState.toString());
            bb.append("uptime", rsStatusArgs.selfUptime);
            if (!_selfConfig().isArbiter()) {
                appendOpTime(&bb, "optime", lastOpApplied);
                bb.appendDate("optimeDate",
                              Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
            }

            if (!_syncSource.empty() && !_iAmPrimary()) {
                bb.append("syncingTo", _syncSource.toString());
                bb.append("syncSourceHost", _syncSource.toString());
                const MemberConfig* member = _rsConfig.findMemberByHostAndPort(_syncSource);
                bb.append("syncSourceId", member ? member->getId().getData() : -1);
            } else {
                bb.append("syncingTo", "");
                bb.append("syncSourceHost", "");
                bb.append("syncSourceId", -1);
            }

            if (_maintenanceModeCalls) {
                bb.append("maintenanceMode", _maintenanceModeCalls);
            }

            bb.append("infoMessage", _getHbmsg(now));

            if (myState.primary()) {
                bb.append("electionTime", _electionTime);
                bb.appendDate("electionDate",
                              Date_t::fromDurationSinceEpoch(Seconds(_electionTime.getSecs())));
            }
            bb.appendIntOrLL("configVersion", _rsConfig.getConfigVersion());
            bb.append("self", true);
            bb.append("lastHeartbeatMessage", "");
            membersOut.push_back(bb.obj());
        } else {
            // add non-self member
            const MemberConfig& itConfig = _rsConfig.getMemberAt(itIndex);
            BSONObjBuilder bb;
            bb.append("_id", itConfig.getId().getData());
            bb.append("name", itConfig.getHostAndPort().toString());
            appendIP(&bb, "ip", itConfig.getHostAndPort());
            double h = it->getHealth();
            bb.append("health", h);
            const MemberState state = it->getState();
            bb.append("state", static_cast<int>(state.s));
            if (h == 0) {
                // if we can't connect the state info is from the past
                // and could be confusing to show
                bb.append("stateStr", "(not reachable/healthy)");
            } else {
                bb.append("stateStr", it->getState().toString());
            }

            const unsigned int uptime = static_cast<unsigned int>((
                it->getUpSince() != Date_t() ? durationCount<Seconds>(now - it->getUpSince()) : 0));
            bb.append("uptime", uptime);
            if (!itConfig.isArbiter()) {
                appendOpTime(&bb, "optime", it->getHeartbeatAppliedOpTime());
                appendOpTime(&bb, "optimeDurable", it->getHeartbeatDurableOpTime());

                bb.appendDate("optimeDate",
                              Date_t::fromDurationSinceEpoch(
                                  Seconds(it->getHeartbeatAppliedOpTime().getSecs())));
                bb.appendDate("optimeDurableDate",
                              Date_t::fromDurationSinceEpoch(
                                  Seconds(it->getHeartbeatDurableOpTime().getSecs())));
            }
            bb.appendDate("lastHeartbeat", it->getLastHeartbeat());
            bb.appendDate("lastHeartbeatRecv", it->getLastHeartbeatRecv());
            Milliseconds ping = _getPing(itConfig.getHostAndPort());
            bb.append("pingMs", durationCount<Milliseconds>(ping));
            bb.append("lastHeartbeatMessage", it->getLastHeartbeatMsg());
            if (it->hasAuthIssue()) {
                bb.append("authenticated", false);
            }
            const HostAndPort& syncSource = it->getSyncSource();
            if (!syncSource.empty() && !state.primary()) {
                bb.append("syncingTo", syncSource.toString());
                bb.append("syncSourceHost", syncSource.toString());
                const MemberConfig* member = _rsConfig.findMemberByHostAndPort(syncSource);
                bb.append("syncSourceId", member ? member->getId().getData() : -1);
            } else {
                bb.append("syncingTo", "");
                bb.append("syncSourceHost", "");
                bb.append("syncSourceId", -1);
            }

            bb.append("infoMessage", "");

            if (state == MemberState::RS_PRIMARY) {
                bb.append("electionTime", it->getElectionTime());
                bb.appendDate(
                    "electionDate",
                    Date_t::fromDurationSinceEpoch(Seconds(it->getElectionTime().getSecs())));
            }
            bb.appendIntOrLL("configVersion", it->getConfigVersion());
            membersOut.push_back(bb.obj());
        }
    }

    // sort members bson
    sort(membersOut.begin(), membersOut.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    response->append("set", _rsConfig.isInitialized() ? _rsConfig.getReplSetName() : "");
    response->append("date", now);
    response->append("myState", myState.s);
    response->append("term", _term);

    // Add sync source info
    if (!_syncSource.empty() && !myState.primary() && !myState.removed()) {
        response->append("syncingTo", _syncSource.toString());
        response->append("syncSourceHost", _syncSource.toString());
        const MemberConfig* member = _rsConfig.findMemberByHostAndPort(_syncSource);
        response->append("syncSourceId", member ? member->getId().getData() : -1);
    } else {
        response->append("syncingTo", "");
        response->append("syncSourceHost", "");
        response->append("syncSourceId", -1);
    }

    if (_rsConfig.isConfigServer()) {
        response->append("configsvr", true);
    }

    response->append("heartbeatIntervalMillis",
                     durationCount<Milliseconds>(_rsConfig.getHeartbeatInterval()));

    // New optimes, to hold them all.
    BSONObjBuilder optimes;
    _lastCommittedOpTimeAndWallTime.opTime.append(&optimes, "lastCommittedOpTime");

    optimes.appendDate("lastCommittedWallTime", _lastCommittedOpTimeAndWallTime.wallTime);

    if (!rsStatusArgs.readConcernMajorityOpTime.opTime.isNull()) {
        rsStatusArgs.readConcernMajorityOpTime.opTime.append(&optimes, "readConcernMajorityOpTime");
        optimes.appendDate("readConcernMajorityWallTime",
                           rsStatusArgs.readConcernMajorityOpTime.wallTime);
    }

    appendOpTime(&optimes, "appliedOpTime", lastOpApplied);
    appendOpTime(&optimes, "durableOpTime", lastOpDurable);

    optimes.appendDate("lastAppliedWallTime", lastOpAppliedWall);
    optimes.appendDate("lastDurableWallTime", lastOpDurableWall);

    response->append("optimes", optimes.obj());
    if (lastStableRecoveryTimestamp) {
        // Only include this field if the storage engine supports RTT.
        response->append("lastStableRecoveryTimestamp", *lastStableRecoveryTimestamp);
    }
    if (lastStableCheckpointTimestampDeprecated) {
        // Only include this field if the storage engine supports RTT and persists data.
        response->append("lastStableCheckpointTimestamp", *lastStableCheckpointTimestampDeprecated);
    }

    if (!initialSyncStatus.isEmpty()) {
        response->append("initialSyncStatus", initialSyncStatus);
    }

    response->append("members", membersOut);
    *result = Status::OK();
}

StatusWith<BSONObj> TopologyCoordinator::prepareReplSetUpdatePositionCommand(
    OpTime currentCommittedSnapshotOpTime) const {
    BSONObjBuilder cmdBuilder;
    invariant(_rsConfig.isInitialized());
    // Do not send updates if we have been removed from the config.
    if (_selfIndex == -1) {
        return Status(ErrorCodes::NodeNotFound,
                      "This node is not in the current replset configuration.");
    }
    cmdBuilder.append(UpdatePositionArgs::kCommandFieldName, 1);
    // Create an array containing objects each live member connected to us and for ourself.
    BSONArrayBuilder arrayBuilder(cmdBuilder.subarrayStart("optimes"));
    for (const auto& memberData : _memberData) {
        if (memberData.getLastAppliedOpTime().isNull()) {
            // Don't include info on members we haven't heard from yet.
            continue;
        }
        // Don't include members we think are down.
        if (!memberData.isSelf() && memberData.lastUpdateStale()) {
            continue;
        }

        BSONObjBuilder entry(arrayBuilder.subobjStart());
        memberData.getLastDurableOpTime().append(&entry,
                                                 UpdatePositionArgs::kDurableOpTimeFieldName);
        entry.appendDate(UpdatePositionArgs::kDurableWallTimeFieldName,
                         memberData.getLastDurableWallTime());
        memberData.getLastAppliedOpTime().append(&entry,
                                                 UpdatePositionArgs::kAppliedOpTimeFieldName);
        entry.appendDate(UpdatePositionArgs::kAppliedWallTimeFieldName,
                         memberData.getLastAppliedWallTime());
        entry.append(UpdatePositionArgs::kMemberIdFieldName, memberData.getMemberId().getData());
        entry.append(UpdatePositionArgs::kConfigVersionFieldName, _rsConfig.getConfigVersion());
    }
    arrayBuilder.done();

    // Add metadata to command
    prepareReplSetMetadata(currentCommittedSnapshotOpTime)
        .writeToMetadata(&cmdBuilder)
        .transitional_ignore();
    return cmdBuilder.obj();
}

void TopologyCoordinator::fillMemberData(BSONObjBuilder* result) {
    BSONArrayBuilder replicationProgress(result->subarrayStart("replicationProgress"));
    {
        for (const auto& memberData : _memberData) {
            BSONObjBuilder entry(replicationProgress.subobjStart());
            entry.append("host", memberData.getHostAndPort().toString());

            const auto lastDurableOpTime = memberData.getLastDurableOpTime();
            entry.append("optime", lastDurableOpTime.toBSON());

            const auto lastAppliedOpTime = memberData.getLastAppliedOpTime();
            entry.append("lastAppliedOpTime", lastAppliedOpTime.toBSON());

            const auto heartbeatAppliedOpTime = memberData.getHeartbeatAppliedOpTime();
            entry.append("heartbeatAppliedOpTime", heartbeatAppliedOpTime.toBSON());

            const auto heartbeatDurableOpTime = memberData.getHeartbeatDurableOpTime();
            entry.append("heartbeatDurableOpTime", heartbeatDurableOpTime.toBSON());

            if (_selfIndex >= 0) {
                entry.append("memberId", memberData.getMemberId().getData());
            }
        }
    }
}

void TopologyCoordinator::fillIsMasterForReplSet(IsMasterResponse* const response,
                                                 const SplitHorizon::Parameters& horizonParams) {
    const MemberState myState = getMemberState();
    if (!_rsConfig.isInitialized()) {
        response->markAsNoConfig();
        return;
    }

    response->setReplSetName(_rsConfig.getReplSetName());
    if (myState.removed()) {
        response->markAsNoConfig();
        return;
    }

    invariant(!_rsConfig.members().empty());

    const auto& self = _rsConfig.members()[_selfIndex];

    const auto horizon = self.determineHorizon(horizonParams);

    for (const auto& member : _rsConfig.members()) {
        if (member.isHidden() || member.getSlaveDelay() > Seconds{0}) {
            continue;
        }
        auto hostView = member.getHostAndPort(horizon);

        if (member.isElectable()) {
            response->addHost(std::move(hostView));
        } else if (member.isArbiter()) {
            response->addArbiter(std::move(hostView));
        } else {
            response->addPassive(std::move(hostView));
        }
    }

    response->setReplSetVersion(_rsConfig.getConfigVersion());
    // "ismaster" is false if we are not primary. If we're stepping down, we're waiting for the
    // Replication State Transition Lock before we can change to secondary, but we should report
    // "ismaster" false to indicate that we can't accept new writes.
    response->setIsMaster(myState.primary() && !isSteppingDown());
    response->setIsSecondary(myState.secondary());

    const MemberConfig* curPrimary = _currentPrimaryMember();
    if (curPrimary) {
        response->setPrimary(curPrimary->getHostAndPort(horizon));
    }

    const MemberConfig& selfConfig = _rsConfig.getMemberAt(_selfIndex);
    if (selfConfig.isArbiter()) {
        response->setIsArbiterOnly(true);
    } else if (selfConfig.getPriority() == 0) {
        response->setIsPassive(true);
    }
    if (selfConfig.getSlaveDelay() > Seconds(0)) {
        response->setSlaveDelay(selfConfig.getSlaveDelay());
    }
    if (selfConfig.isHidden()) {
        response->setIsHidden(true);
    }
    if (!selfConfig.shouldBuildIndexes()) {
        response->setShouldBuildIndexes(false);
    }
    const ReplSetTagConfig tagConfig = _rsConfig.getTagConfig();
    if (selfConfig.hasTags(tagConfig)) {
        for (MemberConfig::TagIterator tag = selfConfig.tagsBegin(); tag != selfConfig.tagsEnd();
             ++tag) {
            std::string tagKey = tagConfig.getTagKey(*tag);
            if (tagKey[0] == '$') {
                // Filter out internal tags
                continue;
            }
            response->addTag(tagKey, tagConfig.getTagValue(*tag));
        }
    }
    response->setMe(selfConfig.getHostAndPort());
    if (_iAmPrimary()) {
        response->setElectionId(_electionId);
    }
}

StatusWith<TopologyCoordinator::PrepareFreezeResponseResult>
TopologyCoordinator::prepareFreezeResponse(Date_t now, int secs, BSONObjBuilder* response) {
    if (_role != TopologyCoordinator::Role::kFollower) {
        std::string msg = str::stream()
            << "cannot freeze node when primary or running for election. state: "
            << (_role == TopologyCoordinator::Role::kLeader ? "Primary" : "Running-Election");
        log() << msg;
        return Status(ErrorCodes::NotSecondary, msg);
    }

    if (secs == 0) {
        _stepDownUntil = now;
        log() << "'unfreezing'";
        response->append("info", "unfreezing");
        return PrepareFreezeResponseResult::kSingleNodeSelfElect;
    } else {
        if (secs == 1)
            response->append("warning", "you really want to freeze for only 1 second?");

        _stepDownUntil = std::max(_stepDownUntil, now + Seconds(secs));
        log() << "'freezing' for " << secs << " seconds";
    }

    return PrepareFreezeResponseResult::kNoAction;
}

Timestamp TopologyCoordinator::getElectionTime() const {
    return _electionTime;
}

OID TopologyCoordinator::getElectionId() const {
    return _electionId;
}

int TopologyCoordinator::getCurrentPrimaryIndex() const {
    return _currentPrimaryIndex;
}

Date_t TopologyCoordinator::getStepDownTime() const {
    return _stepDownUntil;
}

void TopologyCoordinator::_updateHeartbeatDataForReconfig(const ReplSetConfig& newConfig,
                                                          int selfIndex,
                                                          Date_t now) {
    std::vector<MemberData> oldHeartbeats;
    _memberData.swap(oldHeartbeats);

    int index = 0;
    for (ReplSetConfig::MemberIterator it = newConfig.membersBegin(); it != newConfig.membersEnd();
         ++it, ++index) {
        const MemberConfig& newMemberConfig = *it;
        MemberData newHeartbeatData;
        for (auto&& oldMemberData : oldHeartbeats) {
            if ((oldMemberData.getMemberId() == newMemberConfig.getId() &&
                 oldMemberData.getHostAndPort() == newMemberConfig.getHostAndPort()) ||
                (index == selfIndex && oldMemberData.isSelf())) {
                // This member existed in the old config with the same member ID and
                // HostAndPort, so copy its heartbeat data over.
                newHeartbeatData = oldMemberData;
                break;
            }
        }
        newHeartbeatData.setConfigIndex(index);
        newHeartbeatData.setIsSelf(index == selfIndex);
        newHeartbeatData.setHostAndPort(newMemberConfig.getHostAndPort());
        newHeartbeatData.setMemberId(newMemberConfig.getId());
        _memberData.push_back(newHeartbeatData);
    }
    if (selfIndex < 0) {
        // It's necessary to have self member data even if self isn't in the configuration.
        // We don't need data for the other nodes (which no longer know about us, or soon won't)
        _memberData.clear();
        // We're not in the config, we can't sync any more.
        _syncSource = HostAndPort();
        MemberData newHeartbeatData;
        for (auto&& oldMemberData : oldHeartbeats) {
            if (oldMemberData.isSelf()) {
                newHeartbeatData = oldMemberData;
                break;
            }
        }
        newHeartbeatData.setConfigIndex(-1);
        newHeartbeatData.setIsSelf(true);
        _memberData.push_back(newHeartbeatData);
    }
}

// This function installs a new config object and recreates MemberData objects
// that reflect the new config.
void TopologyCoordinator::updateConfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now) {
    invariant(_role != Role::kCandidate);
    invariant(selfIndex < newConfig.getNumMembers());

    // Reset term on startup.
    if (!_rsConfig.isInitialized()) {
        _term = OpTime::kInitialTerm;
        LOG(1) << "Updated term in topology coordinator to " << _term << " due to new config";
    }

    _updateHeartbeatDataForReconfig(newConfig, selfIndex, now);
    _rsConfig = newConfig;
    _selfIndex = selfIndex;
    _forceSyncSourceIndex = -1;

    if (_role == Role::kLeader) {
        if (_selfIndex == -1) {
            log() << "Could not remain primary because no longer a member of the replica set";
        } else if (!_selfConfig().isElectable()) {
            log() << " Could not remain primary because no longer electable";
        } else {
            // Don't stepdown if you don't have to.
            _currentPrimaryIndex = _selfIndex;
            return;
        }
        _role = Role::kFollower;
        _setLeaderMode(LeaderMode::kNotLeader);
    }

    // By this point we know we are in Role::kFollower
    _currentPrimaryIndex = -1;  // force secondaries to re-detect who the primary is

    if (_isElectableNodeInSingleNodeReplicaSet()) {
        // If the new config describes a one-node replica set, we're the one member,
        // we're electable, we're not in maintenance mode and we are currently in followerMode
        // SECONDARY, we must transition to candidate, in leiu of heartbeats.
        _role = Role::kCandidate;
    }
}
std::string TopologyCoordinator::_getHbmsg(Date_t now) const {
    // ignore messages over 2 minutes old
    if ((now - _hbmsgTime) > Seconds{120}) {
        return "";
    }
    return _hbmsg;
}

void TopologyCoordinator::setMyHeartbeatMessage(const Date_t now, const std::string& message) {
    _hbmsgTime = now;
    _hbmsg = message;
}

const MemberConfig& TopologyCoordinator::_selfConfig() const {
    return _rsConfig.getMemberAt(_selfIndex);
}

const MemberData& TopologyCoordinator::_selfMemberData() const {
    return _memberData[_selfMemberDataIndex()];
}

MemberData& TopologyCoordinator::_selfMemberData() {
    return _memberData[_selfMemberDataIndex()];
}

const int TopologyCoordinator::_selfMemberDataIndex() const {
    invariant(!_memberData.empty());
    if (_selfIndex >= 0)
        return _selfIndex;
    // If there is no config or we're not in the config, the first-and-only entry should be for
    // self.
    return 0;
}

TopologyCoordinator::UnelectableReasonMask TopologyCoordinator::_getUnelectableReason(
    int index) const {
    invariant(index != _selfIndex);
    const MemberConfig& memberConfig = _rsConfig.getMemberAt(index);
    const MemberData& hbData = _memberData.at(index);
    UnelectableReasonMask result = None;
    if (memberConfig.isArbiter()) {
        result |= ArbiterIAm;
    }
    if (memberConfig.getPriority() <= 0) {
        result |= NoPriority;
    }
    if (hbData.getState() != MemberState::RS_SECONDARY) {
        result |= NotSecondary;
    }
    invariant(result || memberConfig.isElectable());
    return result;
}

TopologyCoordinator::UnelectableReasonMask TopologyCoordinator::_getMyUnelectableReason(
    const Date_t now, StartElectionReason reason) const {
    UnelectableReasonMask result = None;
    const OpTime lastApplied = getMyLastAppliedOpTime();
    if (lastApplied.isNull()) {
        result |= NoData;
    }
    if (!_aMajoritySeemsToBeUp()) {
        result |= CannotSeeMajority;
    }
    if (_selfIndex == -1) {
        result |= NotInitialized;
        return result;
    }
    if (_selfConfig().isArbiter()) {
        result |= ArbiterIAm;
    }
    if (_selfConfig().getPriority() <= 0) {
        result |= NoPriority;
    }
    if (_stepDownUntil > now) {
        result |= StepDownPeriodActive;
    }

    // Cannot be electable unless secondary or already primary
    if (!getMemberState().secondary() && !_iAmPrimary()) {
        result |= NotSecondary;
    }

    if (reason == StartElectionReason::kPriorityTakeover && !_amIFreshEnoughForPriorityTakeover()) {
        result |= NotCloseEnoughToLatestForPriorityTakeover;
    }

    if (reason == StartElectionReason::kCatchupTakeover && !_amIFreshEnoughForCatchupTakeover()) {
        result |= NotFreshEnoughForCatchupTakeover;
    }
    return result;
}

std::string TopologyCoordinator::_getUnelectableReasonString(const UnelectableReasonMask ur) const {
    invariant(ur);
    str::stream ss;
    bool hasWrittenToStream = false;
    if (ur & NoData) {
        ss << "node has no applied oplog entries";
        hasWrittenToStream = true;
    }
    if (ur & CannotSeeMajority) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "I cannot see a majority";
    }
    if (ur & ArbiterIAm) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is an arbiter";
    }
    if (ur & NoPriority) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member has zero priority";
    }
    if (ur & StepDownPeriodActive) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "I am still waiting for stepdown period to end at "
           << dateToISOStringLocal(_stepDownUntil);
    }
    if (ur & NotSecondary) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is not currently a secondary";
    }
    if (ur & NotCloseEnoughToLatestForPriorityTakeover) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is not caught up enough to the most up-to-date member to call for priority "
              "takeover - must be within "
           << gPriorityTakeoverFreshnessWindowSeconds << " seconds";
    }
    if (ur & NotFreshEnoughForCatchupTakeover) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is either not the most up-to-date member or not ahead of the primary, and "
              "therefore cannot call for catchup takeover";
    }
    if (ur & NotInitialized) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "node is not a member of a valid replica set configuration";
    }
    if (!hasWrittenToStream) {
        severe() << "Invalid UnelectableReasonMask value 0x" << integerToHex(ur);
        fassertFailed(26011);
    }
    ss << " (mask 0x" << integerToHex(ur) << ")";
    return ss;
}

Milliseconds TopologyCoordinator::_getPing(const HostAndPort& host) {
    return _pings[host].getMillis();
}

void TopologyCoordinator::_setElectionTime(const Timestamp& newElectionTime) {
    _electionTime = newElectionTime;
}

int TopologyCoordinator::_getTotalPings() {
    PingMap::iterator it = _pings.begin();
    PingMap::iterator end = _pings.end();
    int totalPings = 0;
    while (it != end) {
        totalPings += it->second.getCount();
        it++;
    }
    return totalPings;
}

bool TopologyCoordinator::isSteppingDownUnconditionally() const {
    return _leaderMode == LeaderMode::kSteppingDown;
}

bool TopologyCoordinator::isSteppingDown() const {
    return isSteppingDownUnconditionally() || _leaderMode == LeaderMode::kAttemptingStepDown;
}

void TopologyCoordinator::_setLeaderMode(TopologyCoordinator::LeaderMode newMode) {
    // Invariants for valid state transitions.
    switch (_leaderMode) {
        case LeaderMode::kNotLeader:
            invariant(newMode == LeaderMode::kLeaderElect);
            break;
        case LeaderMode::kLeaderElect:
            invariant(newMode == LeaderMode::kNotLeader ||  // TODO(SERVER-30852): remove this case
                      newMode == LeaderMode::kMaster ||
                      newMode == LeaderMode::kAttemptingStepDown ||
                      newMode == LeaderMode::kSteppingDown);
            break;
        case LeaderMode::kMaster:
            invariant(newMode == LeaderMode::kNotLeader ||  // TODO(SERVER-30852): remove this case
                      newMode == LeaderMode::kAttemptingStepDown ||
                      newMode == LeaderMode::kSteppingDown);
            break;
        case LeaderMode::kAttemptingStepDown:
            invariant(newMode == LeaderMode::kNotLeader || newMode == LeaderMode::kMaster ||
                      newMode == LeaderMode::kSteppingDown || newMode == LeaderMode::kLeaderElect);
            break;
        case LeaderMode::kSteppingDown:
            invariant(newMode == LeaderMode::kNotLeader);
            break;
    }
    _leaderMode = std::move(newMode);
}

MemberState TopologyCoordinator::getMemberState() const {
    if (_selfIndex == -1) {
        if (_rsConfig.isInitialized()) {
            return MemberState::RS_REMOVED;
        }
        return MemberState::RS_STARTUP;
    }

    if (_rsConfig.isConfigServer()) {
        if (_options.clusterRole != ClusterRole::ConfigServer && !skipShardingConfigurationChecks) {
            return MemberState::RS_REMOVED;
        } else {
            invariant(_storageEngineSupportsReadCommitted != ReadCommittedSupport::kUnknown);
            if (_storageEngineSupportsReadCommitted == ReadCommittedSupport::kNo) {
                return MemberState::RS_REMOVED;
            }
        }
    } else {
        if (_options.clusterRole == ClusterRole::ConfigServer && !skipShardingConfigurationChecks) {
            return MemberState::RS_REMOVED;
        }
    }

    if (_role == Role::kLeader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        invariant(_leaderMode != LeaderMode::kNotLeader);
        return MemberState::RS_PRIMARY;
    }
    const MemberConfig& myConfig = _selfConfig();
    if (myConfig.isArbiter()) {
        return MemberState::RS_ARBITER;
    }
    if (((_maintenanceModeCalls > 0) || (_hasOnlyAuthErrorUpHeartbeats(_memberData, _selfIndex))) &&
        (_followerMode == MemberState::RS_SECONDARY)) {
        return MemberState::RS_RECOVERING;
    }
    return _followerMode;
}

std::vector<MemberData> TopologyCoordinator::getMemberData() const {
    return _memberData;
}

bool TopologyCoordinator::canAcceptWrites() const {
    return _leaderMode == LeaderMode::kMaster;
}

void TopologyCoordinator::setElectionInfo(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::kLeader);
    _electionTime = electionOpTime;
    _electionId = electionId;
}

void TopologyCoordinator::processWinElection(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::kCandidate);
    invariant(_leaderMode == LeaderMode::kNotLeader);
    _role = Role::kLeader;
    _setLeaderMode(LeaderMode::kLeaderElect);
    setElectionInfo(electionId, electionOpTime);
    _currentPrimaryIndex = _selfIndex;
    _syncSource = HostAndPort();
    _forceSyncSourceIndex = -1;
    // Prevent last committed optime from updating until we finish draining.
    _firstOpTimeOfMyTerm =
        OpTime(Timestamp(std::numeric_limits<int>::max(), 0), std::numeric_limits<int>::max());
}

void TopologyCoordinator::processLoseElection() {
    invariant(_role == Role::kCandidate);
    invariant(_leaderMode == LeaderMode::kNotLeader);
    const HostAndPort syncSourceAddress = getSyncSourceAddress();
    _electionTime = Timestamp(0, 0);
    _electionId = OID();
    _role = Role::kFollower;
}

bool TopologyCoordinator::attemptStepDown(
    long long termAtStart, Date_t now, Date_t waitUntil, Date_t stepDownUntil, bool force) {

    if (_role != Role::kLeader || _leaderMode == LeaderMode::kSteppingDown ||
        _term != termAtStart) {
        uasserted(ErrorCodes::PrimarySteppedDown,
                  "While waiting for secondaries to catch up before stepping down, "
                  "this node decided to step down for other reasons");
    }
    invariant(_leaderMode == LeaderMode::kAttemptingStepDown);

    if (now >= stepDownUntil) {
        uasserted(ErrorCodes::ExceededTimeLimit,
                  "By the time we were ready to step down, we were already past the "
                  "time we were supposed to step down until");
    }

    if (!_canCompleteStepDownAttempt(now, waitUntil, force)) {
        // Stepdown attempt failed.

        // Check waitUntil after at least one stepdown attempt, so that stepdown could succeed even
        // if secondaryCatchUpPeriodSecs == 0.
        if (now >= waitUntil) {
            uasserted(ErrorCodes::ExceededTimeLimit,
                      str::stream() << "No electable secondaries caught up as of "
                                    << dateToISOStringLocal(now)
                                    << ". Please use the replSetStepDown command with the argument "
                                    << "{force: true} to force node to step down.");
        }

        // Stepdown attempt failed, but in a way that can be retried
        return false;
    }

    // Stepdown attempt success!
    _stepDownUntil = stepDownUntil;
    _stepDownSelfAndReplaceWith(-1);
    return true;
}

bool TopologyCoordinator::_canCompleteStepDownAttempt(Date_t now, Date_t waitUntil, bool force) {
    const bool forceNow = force && (now >= waitUntil);
    if (forceNow) {
        return true;
    }

    return isSafeToStepDown();
}

bool TopologyCoordinator::_isCaughtUpAndElectable(int memberIndex, OpTime lastApplied) {
    if (_getUnelectableReason(memberIndex)) {
        return false;
    }

    return (_memberData.at(memberIndex).getLastAppliedOpTime() >= lastApplied);
}

bool TopologyCoordinator::isSafeToStepDown() {
    if (!_rsConfig.isInitialized() || _selfIndex < 0) {
        return false;
    }

    OpTime lastApplied = getMyLastAppliedOpTime();

    auto tagStatus = _rsConfig.findCustomWriteMode(ReplSetConfig::kMajorityWriteConcernModeName);
    invariant(tagStatus.isOK());

    // Check if a majority of nodes have reached the last applied optime.
    if (!haveTaggedNodesReachedOpTime(lastApplied, tagStatus.getValue(), false)) {
        return false;
    }

    // Now check that we also have at least one caught up node that is electable.
    for (int memberIndex = 0; memberIndex < _rsConfig.getNumMembers(); memberIndex++) {
        // ignore your self
        if (memberIndex == _selfIndex) {
            continue;
        }

        if (_isCaughtUpAndElectable(memberIndex, lastApplied)) {
            return true;
        }
    }

    return false;
}

int TopologyCoordinator::chooseElectionHandoffCandidate() {

    OpTime lastApplied = getMyLastAppliedOpTime();

    int bestCandidateIndex = -1;
    int highestPriority = -1;

    for (int memberIndex = 0; memberIndex < _rsConfig.getNumMembers(); memberIndex++) {

        // Skip your own member index.
        if (memberIndex == _selfIndex) {
            continue;
        }

        // Skip this node if it is not eligible to become primary. This includes nodes with
        // priority 0.
        if (!_isCaughtUpAndElectable(memberIndex, lastApplied)) {
            continue;
        }

        // Only update best if priority is strictly greater. This guarantees that
        // we will pick the member with the lowest index in case of a tie. Note that
        // member priority is always a non-negative number.
        auto memberPriority = _rsConfig.getMemberAt(memberIndex).getPriority();
        if (memberPriority > highestPriority) {
            bestCandidateIndex = memberIndex;
            highestPriority = memberPriority;
        }
    }

    // This is the most suitable node.
    return bestCandidateIndex;
}

void TopologyCoordinator::setFollowerMode(MemberState::MS newMode) {
    invariant(_role == Role::kFollower);
    switch (newMode) {
        case MemberState::RS_RECOVERING:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_SECONDARY:
        case MemberState::RS_STARTUP2:
            _followerMode = newMode;
            break;
        default:
            MONGO_UNREACHABLE;
    }

    if (_followerMode != MemberState::RS_SECONDARY) {
        return;
    }

    // When a single node replica set transitions to SECONDARY, we must check if we should
    // be a candidate here.  This is necessary because a single node replica set has no
    // heartbeats that would normally change the role to candidate.

    if (_isElectableNodeInSingleNodeReplicaSet()) {
        _role = Role::kCandidate;
    }
}

bool TopologyCoordinator::_isElectableNodeInSingleNodeReplicaSet() const {
    return _followerMode == MemberState::RS_SECONDARY && _rsConfig.getNumMembers() == 1 &&
        _selfIndex == 0 && _rsConfig.getMemberAt(_selfIndex).isElectable() &&
        _maintenanceModeCalls == 0;
}

void TopologyCoordinator::finishUnconditionalStepDown() {
    invariant(_leaderMode == LeaderMode::kSteppingDown);

    int remotePrimaryIndex = -1;
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex) {
            continue;
        }

        if (it->getState().primary() && it->up()) {
            if (remotePrimaryIndex != -1) {
                // two other nodes think they are primary (asynchronously polled)
                // -- wait for things to settle down.
                remotePrimaryIndex = -1;
                warning() << "two remote primaries (transiently)";
                break;
            }
            remotePrimaryIndex = itIndex;
        }
    }
    _stepDownSelfAndReplaceWith(remotePrimaryIndex);
}

void TopologyCoordinator::_stepDownSelfAndReplaceWith(int newPrimary) {
    invariant(_role == Role::kLeader);
    invariant(_selfIndex != -1);
    invariant(_selfIndex != newPrimary);
    invariant(_selfIndex == _currentPrimaryIndex);
    _currentPrimaryIndex = newPrimary;
    _role = Role::kFollower;
    _setLeaderMode(LeaderMode::kNotLeader);
}

bool TopologyCoordinator::updateLastCommittedOpTimeAndWallTime() {
    // If we're not primary or we're stepping down due to learning of a new term then we must not
    // advance the commit point.  If we are stepping down due to a user request, however, then it
    // is safe to advance the commit point, and in fact we must since the stepdown request may be
    // waiting for the commit point to advance enough to be able to safely complete the step down.
    if (!_iAmPrimary() || _leaderMode == LeaderMode::kSteppingDown) {
        return false;
    }

    // Whether we use the applied or durable OpTime for the commit point is decided here.
    const bool useDurableOpTime = _rsConfig.getWriteConcernMajorityShouldJournal();

    std::vector<OpTimeAndWallTime> votingNodesOpTimesAndWallTimes;
    for (const auto& memberData : _memberData) {
        int memberIndex = memberData.getConfigIndex();
        invariant(memberIndex >= 0);
        const auto& memberConfig = _rsConfig.getMemberAt(memberIndex);
        if (memberConfig.isVoter()) {
            const OpTimeAndWallTime durableOpTime = {memberData.getLastDurableOpTime(),
                                                     memberData.getLastDurableWallTime()};
            const OpTimeAndWallTime appliedOpTime = {memberData.getLastAppliedOpTime(),
                                                     memberData.getLastAppliedWallTime()};
            const OpTimeAndWallTime opTime = useDurableOpTime ? durableOpTime : appliedOpTime;
            votingNodesOpTimesAndWallTimes.push_back(opTime);
        }
    }

    invariant(votingNodesOpTimesAndWallTimes.size() > 0);
    if (votingNodesOpTimesAndWallTimes.size() <
        static_cast<unsigned long>(_rsConfig.getWriteMajority())) {
        return false;
    }
    std::sort(votingNodesOpTimesAndWallTimes.begin(), votingNodesOpTimesAndWallTimes.end());

    // need the majority to have this OpTime
    OpTimeAndWallTime committedOpTime =
        votingNodesOpTimesAndWallTimes[votingNodesOpTimesAndWallTimes.size() -
                                       _rsConfig.getWriteMajority()];

    const bool fromSyncSource = false;
    return advanceLastCommittedOpTimeAndWallTime(committedOpTime, fromSyncSource);
}

bool TopologyCoordinator::advanceLastCommittedOpTimeAndWallTime(OpTimeAndWallTime committedOpTime,
                                                                bool fromSyncSource) {
    if (_selfIndex == -1) {
        // The config hasn't been installed or we are not in the config. This could happen
        // on heartbeats before installing a config.
        return false;
    }

    // This check is performed to ensure primaries do not commit an OpTime from a previous term.
    if (_iAmPrimary() && committedOpTime.opTime < _firstOpTimeOfMyTerm) {
        LOG(1) << "Ignoring older committed snapshot from before I became primary, optime: "
               << committedOpTime.opTime << ", firstOpTimeOfMyTerm: " << _firstOpTimeOfMyTerm;
        return false;
    }

    // Arbiters don't have data so they always advance their commit point via heartbeats.
    if (!_selfConfig().isArbiter() &&
        getMyLastAppliedOpTime().getTerm() != committedOpTime.opTime.getTerm()) {
        if (fromSyncSource) {
            committedOpTime = std::min(committedOpTime, getMyLastAppliedOpTimeAndWallTime());
        } else {
            LOG(1) << "Ignoring commit point with different term than my lastApplied, since it "
                      "may "
                      "not be on the same oplog branch as mine. optime: "
                   << committedOpTime
                   << ", my last applied: " << getMyLastAppliedOpTimeAndWallTime();
            return false;
        }
    }

    if (committedOpTime.opTime == _lastCommittedOpTimeAndWallTime.opTime) {
        return false;  // Hasn't changed, so ignore it.
    }

    if (committedOpTime.opTime < _lastCommittedOpTimeAndWallTime.opTime) {
        LOG(1) << "Ignoring older committed snapshot optime: " << committedOpTime
               << ", currentCommittedOpTime: " << _lastCommittedOpTimeAndWallTime;
        return false;
    }

    LOG(2) << "Updating _lastCommittedOpTimeAndWallTime to " << committedOpTime;
    _lastCommittedOpTimeAndWallTime = committedOpTime;
    return true;
}

OpTime TopologyCoordinator::getLastCommittedOpTime() const {
    return _lastCommittedOpTimeAndWallTime.opTime;
}

OpTimeAndWallTime TopologyCoordinator::getLastCommittedOpTimeAndWallTime() const {
    return _lastCommittedOpTimeAndWallTime;
}

bool TopologyCoordinator::canCompleteTransitionToPrimary(long long termWhenDrainCompleted) const {

    if (termWhenDrainCompleted != _term) {
        return false;
    }
    // Allow completing the transition to primary even when in the middle of a stepdown attempt,
    // in case the stepdown attempt fails.
    if (_leaderMode != LeaderMode::kLeaderElect && _leaderMode != LeaderMode::kAttemptingStepDown) {
        return false;
    }

    return true;
}

Status TopologyCoordinator::completeTransitionToPrimary(const OpTime& firstOpTimeOfTerm) {
    if (!canCompleteTransitionToPrimary(firstOpTimeOfTerm.getTerm())) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      "By the time this node was ready to complete its transition to PRIMARY it "
                      "was no longer eligible to do so");
    }
    if (_leaderMode == LeaderMode::kLeaderElect) {
        _setLeaderMode(LeaderMode::kMaster);
    }
    _firstOpTimeOfMyTerm = firstOpTimeOfTerm;
    return Status::OK();
}

void TopologyCoordinator::adjustMaintenanceCountBy(int inc) {
    invariant(_role == Role::kFollower);
    _maintenanceModeCalls += inc;
    invariant(_maintenanceModeCalls >= 0);
}

int TopologyCoordinator::getMaintenanceCount() const {
    return _maintenanceModeCalls;
}

TopologyCoordinator::UpdateTermResult TopologyCoordinator::updateTerm(long long term, Date_t now) {
    if (term <= _term) {
        return TopologyCoordinator::UpdateTermResult::kAlreadyUpToDate;
    }
    // Don't run election if we just stood up or learned about a new term.
    _electionSleepUntil = now + _rsConfig.getElectionTimeoutPeriod();

    // Don't update the term just yet if we are going to step down, as we don't want to report
    // that we are primary in the new term.
    if (_iAmPrimary()) {
        return TopologyCoordinator::UpdateTermResult::kTriggerStepDown;
    }
    LOG(1) << "Updating term from " << _term << " to " << term;
    _term = term;
    return TopologyCoordinator::UpdateTermResult::kUpdatedTerm;
}


long long TopologyCoordinator::getTerm() const {
    return _term;
}

// TODO(siyuan): Merge _hddata into _slaveInfo, so that we have a single view of the
// replset. Passing metadata is unnecessary.
bool TopologyCoordinator::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata,
    Date_t now) const {
    // Methodology:
    // If there exists a viable sync source member other than currentSource, whose oplog has
    // reached an optime greater than _options.maxSyncSourceLagSecs later than currentSource's,
    // return true.
    // If the currentSource has the same replication progress as we do and has no source for further
    // progress, return true.

    if (_selfIndex == -1) {
        log() << "Not choosing new sync source because we are not in the config.";
        return false;
    }

    // If the user requested a sync source change, return true.
    if (_forceSyncSourceIndex != -1) {
        log() << "Choosing new sync source because the user has requested to use "
              << _rsConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort()
              << " as a sync source";
        return true;
    }

    if (replMetadata.getConfigVersion() != _rsConfig.getConfigVersion()) {
        log() << "Choosing new sync source because the config version supplied by " << currentSource
              << ", " << replMetadata.getConfigVersion() << ", does not match ours, "
              << _rsConfig.getConfigVersion();
        return true;
    }

    const int currentSourceIndex = _rsConfig.findMemberIndexByHostAndPort(currentSource);
    if (currentSourceIndex == -1) {
        log() << "Choosing new sync source because " << currentSource.toString()
              << " is not in our config";
        return true;
    }

    invariant(currentSourceIndex != _selfIndex);

    // If OplogQueryMetadata was provided, use its values, otherwise use the ones in
    // ReplSetMetadata.
    OpTime currentSourceOpTime;
    int syncSourceIndex = -1;
    int primaryIndex = -1;
    if (oqMetadata) {
        currentSourceOpTime =
            std::max(oqMetadata->getLastOpApplied(),
                     _memberData.at(currentSourceIndex).getHeartbeatAppliedOpTime());
        syncSourceIndex = oqMetadata->getSyncSourceIndex();
        primaryIndex = oqMetadata->getPrimaryIndex();
    } else {
        currentSourceOpTime =
            std::max(replMetadata.getLastOpVisible(),
                     _memberData.at(currentSourceIndex).getHeartbeatAppliedOpTime());
        syncSourceIndex = replMetadata.getSyncSourceIndex();
        primaryIndex = replMetadata.getPrimaryIndex();
    }

    if (currentSourceOpTime.isNull()) {
        // Haven't received a heartbeat from the sync source yet, so can't tell if we should
        // change.
        return false;
    }

    // Change sync source if they are not ahead of us, and don't have a sync source,
    // unless they are primary.
    const OpTime myLastOpTime = getMyLastAppliedOpTime();
    if (syncSourceIndex == -1 && currentSourceOpTime <= myLastOpTime &&
        primaryIndex != currentSourceIndex) {
        std::stringstream logMessage;

        logMessage << "Choosing new sync source. Our current sync source is not primary and does "
                      "not have a sync source, so we require that it is ahead of us. "
                   << "Current sync source: " << currentSource.toString()
                   << ", my last fetched oplog optime: " << myLastOpTime
                   << ", latest oplog optime of sync source: " << currentSourceOpTime;

        if (primaryIndex >= 0) {
            logMessage << " (" << _rsConfig.getMemberAt(primaryIndex).getHostAndPort() << " is)";
        } else {
            logMessage << " (sync source does not know the primary)";
        }
        log() << logMessage.str();
        return true;
    }

    if (MONGO_FAIL_POINT(disableMaxSyncSourceLagSecs)) {
        log() << "disableMaxSyncSourceLagSecs fail point enabled - not checking the most recent "
                 "OpTime, "
              << currentSourceOpTime.toString() << ", of our current sync source, " << currentSource
              << ", against the OpTimes of the other nodes in this replica set.";
    } else {
        unsigned int currentSecs = currentSourceOpTime.getSecs();
        unsigned int goalSecs = currentSecs + durationCount<Seconds>(_options.maxSyncSourceLagSecs);

        for (std::vector<MemberData>::const_iterator it = _memberData.begin();
             it != _memberData.end();
             ++it) {
            const int itIndex = indexOfIterator(_memberData, it);
            const MemberConfig& candidateConfig = _rsConfig.getMemberAt(itIndex);
            if (it->up() && (candidateConfig.isVoter() || !_selfConfig().isVoter()) &&
                (candidateConfig.shouldBuildIndexes() || !_selfConfig().shouldBuildIndexes()) &&
                it->getState().readable() && !_memberIsBlacklisted(candidateConfig, now) &&
                goalSecs < it->getHeartbeatAppliedOpTime().getSecs()) {
                log() << "Choosing new sync source because the most recent OpTime of our sync "
                         "source, "
                      << currentSource << ", is " << currentSourceOpTime.toString()
                      << " which is more than " << _options.maxSyncSourceLagSecs
                      << " behind member " << candidateConfig.getHostAndPort().toString()
                      << " whose most recent OpTime is "
                      << it->getHeartbeatAppliedOpTime().toString();
                invariant(itIndex != _selfIndex);
                return true;
            }
        }
    }

    return false;
}

rpc::ReplSetMetadata TopologyCoordinator::prepareReplSetMetadata(
    const OpTime& lastVisibleOpTime) const {
    return rpc::ReplSetMetadata(_term,
                                _lastCommittedOpTimeAndWallTime,
                                lastVisibleOpTime,
                                _rsConfig.getConfigVersion(),
                                _rsConfig.getReplicaSetId(),
                                _currentPrimaryIndex,
                                _rsConfig.findMemberIndexByHostAndPort(getSyncSourceAddress()));
}

rpc::OplogQueryMetadata TopologyCoordinator::prepareOplogQueryMetadata(int rbid) const {
    return rpc::OplogQueryMetadata(_lastCommittedOpTimeAndWallTime,
                                   getMyLastAppliedOpTime(),
                                   rbid,
                                   _currentPrimaryIndex,
                                   _rsConfig.findMemberIndexByHostAndPort(getSyncSourceAddress()));
}

void TopologyCoordinator::processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                                     ReplSetRequestVotesResponse* response) {
    response->setTerm(_term);

    if (args.getTerm() < _term) {
        response->setVoteGranted(false);
        response->setReason(str::stream() << "candidate's term (" << args.getTerm()
                                          << ") is lower than mine ("
                                          << _term
                                          << ")");
    } else if (args.getConfigVersion() != _rsConfig.getConfigVersion()) {
        response->setVoteGranted(false);
        response->setReason(str::stream() << "candidate's config version ("
                                          << args.getConfigVersion()
                                          << ") differs from mine ("
                                          << _rsConfig.getConfigVersion()
                                          << ")");
    } else if (args.getSetName() != _rsConfig.getReplSetName()) {
        response->setVoteGranted(false);
        response->setReason(str::stream() << "candidate's set name (" << args.getSetName()
                                          << ") differs from mine ("
                                          << _rsConfig.getReplSetName()
                                          << ")");
    } else if (args.getLastDurableOpTime() < getMyLastAppliedOpTime()) {
        response->setVoteGranted(false);
        response
            ->setReason(str::stream()
                        << "candidate's data is staler than mine. candidate's last applied OpTime: "
                        << args.getLastDurableOpTime().toString()
                        << ", my last applied OpTime: "
                        << getMyLastAppliedOpTime().toString());
    } else if (!args.isADryRun() && _lastVote.getTerm() == args.getTerm()) {
        response->setVoteGranted(false);
        response->setReason(str::stream()
                            << "already voted for another candidate ("
                            << _rsConfig.getMemberAt(_lastVote.getCandidateIndex()).getHostAndPort()
                            << ") this term ("
                            << _lastVote.getTerm()
                            << ")");
    } else {
        int betterPrimary = _findHealthyPrimaryOfEqualOrGreaterPriority(args.getCandidateIndex());
        if (_selfConfig().isArbiter() && betterPrimary >= 0) {
            response->setVoteGranted(false);
            response->setReason(str::stream()
                                << "can see a healthy primary ("
                                << _rsConfig.getMemberAt(betterPrimary).getHostAndPort()
                                << ") of equal or greater priority");
        } else {
            if (!args.isADryRun()) {
                _lastVote.setTerm(args.getTerm());
                _lastVote.setCandidateIndex(args.getCandidateIndex());
            }
            response->setVoteGranted(true);
        }
    }

    LOG_FOR_ELECTION(0) << "Received vote request: " << args.toString();
    LOG_FOR_ELECTION(0) << "Sending vote response: " << response->toString();
    LOG_FOR_ELECTION(4) << _getReplSetStatusString();
}

void TopologyCoordinator::loadLastVote(const LastVote& lastVote) {
    _lastVote = lastVote;
}

void TopologyCoordinator::voteForMyselfV1() {
    _lastVote.setTerm(_term);
    _lastVote.setCandidateIndex(_selfIndex);
}

void TopologyCoordinator::setPrimaryIndex(long long primaryIndex) {
    _currentPrimaryIndex = primaryIndex;
}

Status TopologyCoordinator::becomeCandidateIfElectable(const Date_t now,
                                                       StartElectionReason reason) {
    if (_role == Role::kLeader) {
        return {ErrorCodes::NodeNotElectable, "Not standing for election again; already primary"};
    }

    if (_role == Role::kCandidate) {
        return {ErrorCodes::NodeNotElectable, "Not standing for election again; already candidate"};
    }

    const UnelectableReasonMask unelectableReason = _getMyUnelectableReason(now, reason);
    if (unelectableReason) {
        return {ErrorCodes::NodeNotElectable,
                str::stream() << "Not standing for election because "
                              << _getUnelectableReasonString(unelectableReason)};
    }

    // All checks passed, become a candidate and start election proceedings.
    _role = Role::kCandidate;

    return Status::OK();
}

void TopologyCoordinator::setStorageEngineSupportsReadCommitted(bool supported) {
    _storageEngineSupportsReadCommitted =
        supported ? ReadCommittedSupport::kYes : ReadCommittedSupport::kNo;
}

void TopologyCoordinator::restartHeartbeats() {
    for (auto& hb : _memberData) {
        hb.restart();
    }
}

boost::optional<OpTime> TopologyCoordinator::latestKnownOpTimeSinceHeartbeatRestart() const {
    // The smallest OpTime in PV1.
    OpTime latest(Timestamp(0, 0), 0);
    for (size_t i = 0; i < _memberData.size(); i++) {
        auto& peer = _memberData[i];

        if (static_cast<int>(i) == _selfIndex) {
            continue;
        }
        // If any heartbeat is not fresh enough, return none.
        if (!peer.isUpdatedSinceRestart()) {
            return boost::none;
        }
        // Ignore down members
        if (!peer.up()) {
            continue;
        }
        if (peer.getHeartbeatAppliedOpTime() > latest) {
            latest = peer.getHeartbeatAppliedOpTime();
        }
    }
    return latest;
}

std::map<MemberId, boost::optional<OpTime>>
TopologyCoordinator::latestKnownOpTimeSinceHeartbeatRestartPerMember() const {
    std::map<MemberId, boost::optional<OpTime>> opTimesPerMember;
    for (size_t i = 0; i < _memberData.size(); i++) {
        auto& member = _memberData[i];
        MemberId memberId = _rsConfig.getMemberAt(i).getId();

        if (!member.isUpdatedSinceRestart()) {
            opTimesPerMember[memberId] = boost::none;
            continue;
        }

        if (!member.up()) {
            opTimesPerMember[memberId] = boost::none;
            continue;
        }

        opTimesPerMember[memberId] = member.getHeartbeatAppliedOpTime();
    }
    return opTimesPerMember;
}

bool TopologyCoordinator::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<MemberConfig>& members) const {
    if (!commitQuorum.mode.empty() && commitQuorum.mode != CommitQuorumOptions::kMajority) {
        StatusWith<ReplSetTagPattern> tagPatternStatus =
            _rsConfig.findCustomWriteMode(commitQuorum.mode);
        if (!tagPatternStatus.isOK()) {
            return false;
        }

        ReplSetTagMatch matcher(tagPatternStatus.getValue());
        for (auto&& member : members) {
            for (MemberConfig::TagIterator it = member.tagsBegin(); it != member.tagsEnd(); ++it) {
                if (matcher.update(*it)) {
                    return true;
                }
            }
        }

        // Even if all the nodes in the set had a given write it still would not satisfy this
        // commit quorum.
        return false;
    } else {
        int nodesRemaining = 0;
        if (!commitQuorum.mode.empty()) {
            invariant(commitQuorum.mode == CommitQuorumOptions::kMajority);
            nodesRemaining = _rsConfig.getWriteMajority();
        } else {
            nodesRemaining = commitQuorum.numNodes;
        }

        for (auto&& member : members) {
            if (!member.isArbiter()) {  // Only count data-bearing nodes
                --nodesRemaining;
                if (nodesRemaining <= 0) {
                    return true;
                }
            }
        }
        return false;
    }
}

bool TopologyCoordinator::checkIfCommitQuorumIsSatisfied(
    const CommitQuorumOptions& commitQuorum,
    const std::vector<HostAndPort>& commitReadyMembers) const {
    std::vector<MemberConfig> commitReadyMemberConfigs;
    for (auto& commitReadyMember : commitReadyMembers) {
        const MemberConfig* memberConfig = _rsConfig.findMemberByHostAndPort(commitReadyMember);

        invariant(memberConfig);
        commitReadyMemberConfigs.push_back(*memberConfig);
    }

    // Calling this with commit ready members only is the same as checking if the commit quorum is
    // satisfied. Because the 'commitQuorum' is based on the participation of all the replica set
    // members, and if the 'commitQuorum' can be satisfied with all the commit ready members, then
    // the commit quorum is satisfied in this replica set configuration.
    return checkIfCommitQuorumCanBeSatisfied(commitQuorum, commitReadyMemberConfigs);
}

}  // namespace repl
}  // namespace mongo
