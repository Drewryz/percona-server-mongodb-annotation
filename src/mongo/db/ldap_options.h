/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2019-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#pragma once

#include <atomic>
#include <string>

#include <boost/thread/synchronized_value.hpp>

#include "mongo/platform/atomic_word.h"

namespace mongo {

struct LDAPGlobalParams {
    boost::synchronized_value<std::string> ldapServers;
    std::string ldapTransportSecurity;
    std::string ldapBindMethod;
    std::string ldapBindSaslMechanisms;
    AtomicInt32 ldapTimeoutMS;
    boost::synchronized_value<std::string> ldapQueryUser;
    boost::synchronized_value<std::string> ldapQueryPassword;
    boost::synchronized_value<std::string> ldapUserToDNMapping;
    bool ldapUseConnectionPool{false}; // do not use AtomicBool because this is kStartupOnly parameter
    AtomicInt32 ldapUserCacheInvalidationInterval{30};
    // ldapQueryTemplate does not exist in mongos, so it should be handled differently
    boost::synchronized_value<std::string> ldapQueryTemplate;
};

extern LDAPGlobalParams ldapGlobalParams;

}  // namespace mongo
