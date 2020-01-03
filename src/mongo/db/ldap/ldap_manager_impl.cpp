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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/ldap/ldap_manager_impl.h"

#include <regex>

#include <fmt/format.h>

#include "mongo/bson/json.h"
#include "mongo/db/ldap_options.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using namespace fmt::literals;

LDAPManagerImpl::LDAPManagerImpl() = default;

LDAPManagerImpl::~LDAPManagerImpl() {
    if (_ldap) {
        ldap_unbind_ext(_ldap, nullptr, nullptr);
        _ldap = nullptr;
    }
}

Status LDAPManagerImpl::initialize() {
    int res = LDAP_OTHER;
    auto uri = "ldap://{}/"_format(ldapGlobalParams.ldapServers.get());
    res = ldap_initialize(&_ldap, uri.c_str());
    if (res != LDAP_SUCCESS) {
        return Status(ErrorCodes::LDAPLibraryError,
                      "Cannot initialize LDAP structure for {}; LDAP error: {}"_format(
                          uri, ldap_err2string(res)));
    }
    const int ldap_version = LDAP_VERSION3;
    res = ldap_set_option(_ldap, LDAP_OPT_PROTOCOL_VERSION, &ldap_version);
    if (res != LDAP_OPT_SUCCESS) {
        return Status(ErrorCodes::LDAPLibraryError,
                      "Cannot set LDAP version option; LDAP error: {}"_format(
                          ldap_err2string(res)));
    }
    auto qusr = ldapGlobalParams.ldapQueryUser.get();
    auto qpsw = ldapGlobalParams.ldapQueryPassword.get();
    berval cred;
    cred.bv_len = qpsw.length();
    cred.bv_val = (char*)qpsw.c_str();
    res = ldap_sasl_bind_s(_ldap, qusr.c_str(), LDAP_SASL_SIMPLE, &cred,
                           nullptr, nullptr, nullptr);
    if (res != LDAP_SUCCESS) {
        return Status(ErrorCodes::LDAPLibraryError,
                      "Cannot bind to LDAP server; LDAP error: {}"_format(
                          ldap_err2string(res)));
    }
    return Status::OK();
}

Status LDAPManagerImpl::execQuery(std::string& ldapurl, std::vector<std::string>& results) {
    timeval tv;
    LDAPMessage*answer = nullptr;
    LDAPURLDesc *ludp{nullptr};
    int res = ldap_url_parse(ldapurl.c_str(), &ludp);
    ON_BLOCK_EXIT([&] { ldap_free_urldesc(ludp); });
    if (res != LDAP_SUCCESS) {
        return Status(ErrorCodes::LDAPLibraryError,
                      "Cannot parse LDAP URL: {}"_format(
                          ldap_err2string(res)));
    }

    // if attributes are not specified assume query returns set of entities (groups)
    const bool entitiesonly = !ludp->lud_attrs || !ludp->lud_attrs[0];

    LOG(1) << fmt::format("Parsing LDAP URL: {ldapurl}; dn: {dn}; scope: {scope}; filter: {filter}",
            fmt::arg("ldapurl", ldapurl),
            fmt::arg("scope", ludp->lud_scope),
            fmt::arg("dn", ludp->lud_dn ? ludp->lud_dn : "nullptr"),
            fmt::arg("filter", ludp->lud_filter ? ludp->lud_filter : "nullptr"));
    res = ldap_search_ext_s(_ldap,
            ludp->lud_dn,
            ludp->lud_scope,
            ludp->lud_filter,
            ludp->lud_attrs,
            0, // attrsonly (0 => attrs and values)
            nullptr, nullptr, &tv, 0, &answer);
    ON_BLOCK_EXIT([&] { ldap_msgfree(answer); });
    if (res != LDAP_SUCCESS) {
        return Status(ErrorCodes::LDAPLibraryError,
                      "LDAP search failed with error: {}"_format(
                          ldap_err2string(res)));
    }

    auto entry = ldap_first_entry(_ldap, answer);
    while (entry) {
        if (entitiesonly) {
            auto dn = ldap_get_dn(_ldap, entry);
            ON_BLOCK_EXIT([&] { ldap_memfree(dn); });
            if (!dn) {
                int ld_errno = 0;
                ldap_get_option(_ldap, LDAP_OPT_RESULT_CODE, &ld_errno);
                return Status(ErrorCodes::LDAPLibraryError,
                              "Failed to get DN from LDAP query result: {}"_format(
                                  ldap_err2string(ld_errno)));
            }
            results.emplace_back(dn);
        } else {
            BerElement *ber = nullptr;
            auto attribute = ldap_first_attribute(_ldap, entry, &ber);
            ON_BLOCK_EXIT([&] { ber_free(ber, 0); });
            while (attribute) {
                ON_BLOCK_EXIT([&] { ldap_memfree(attribute); });

                auto const values = ldap_get_values_len(_ldap, entry, attribute);
                ON_BLOCK_EXIT([&] { ldap_value_free_len(values); });
                if (values) {
                    auto curval = values;
                    while (*curval) {
                        results.emplace_back((*curval)->bv_val, (*curval)->bv_len);
                        ++curval;
                    }
                }
                attribute = ldap_next_attribute(_ldap, entry, ber);
            }
        }
        entry = ldap_next_entry(_ldap, entry);
    }
    return Status::OK();
}

Status LDAPManagerImpl::mapUserToDN(const std::string& user, std::string& out) {
    //TODO: keep BSONArray somewhere is ldapGlobalParams (but consider multithreaded access)
    std::string mapping = ldapGlobalParams.ldapUserToDNMapping.get();

    //TODO: this check should be part of userToDNMapping validation
    if (!isArray(mapping))
        return Status(ErrorCodes::BadValue,
                      "User to DN mapping must be json array of objects");

    BSONArray bsonmapping{fromjson(mapping)};
    for (const auto& elt: bsonmapping) {
        auto step = elt.Obj();
        log() << "UserToDN step: " << step.jsonString();
        for (const auto& fld: step) {
            log() << "UserToDN mapping: " << fld.fieldName() << " = " << fld.valueStringData();
        }
        std::smatch sm;
        std::regex rex{step["match"].str()};
        if (std::regex_match(user, sm, rex)) {
            // user matched current regex
            BSONElement eltempl = step["substitution"];
            bool substitution = true;
            if (!eltempl) {
                // ldapQuery mode
                eltempl = step["ldapQuery"];
                substitution = false;
            }
            // format template
            std::vector<std::string> strs;
            std::vector<fmt::basic_format_arg<fmt::format_context>> args;
            if (rex.mark_count() > 0) {
                // skip first submatch since it is the whole thing
                for (auto it = sm.cbegin() + 1; it != sm.cend(); ++it) {
                    strs.push_back(it->str());
                    args.push_back(fmt::internal::make_arg<fmt::format_context>(strs.back()));
                }
            }
            out = fmt::vformat(eltempl.str(), fmt::format_args{args.data(), args.size()});
            // in substitution mode we are done - just return 'out'
            if (substitution)
                return Status::OK();
            // in ldapQuery mode we need to execute query and make decision based on query result
            std::vector<std::string> qresult;
            auto status = execQuery(out, qresult);
            if (!status.isOK())
                return status;
            // query succeeded only if we have single result
            // otherwise continue search
            if (qresult.size() == 1) {
                out = qresult[0];
                return Status::OK();
            }
        }
    }
    // we have no successful transformations, return error
    return Status(ErrorCodes::BadValue,
                  "Failed to map user '{}' to LDAP DN"_format(user));
}

Status LDAPManagerImpl::queryUserRoles(const UserName& userName, stdx::unordered_set<RoleName>& roles) {
    constexpr auto kAdmin = "admin"_sd;

    const std::string providedUser{userName.getUser().toString()};
    std::string mappedUser;
    {
        auto mapRes = mapUserToDN(providedUser, mappedUser);
        if (!mapRes.isOK())
            return mapRes;
    }

    auto ldapurl = fmt::format("ldap://{Servers}/{Query}",
            fmt::arg("Servers", ldapGlobalParams.ldapServers.get()),
            fmt::arg("Query", ldapGlobalParams.ldapQueryTemplate.get()));
    ldapurl = fmt::format(ldapurl,
            fmt::arg("USER", mappedUser),
            fmt::arg("PROVIDED_USER", providedUser));

    std::vector<std::string> qresult;
    auto status = execQuery(ldapurl, qresult);
    if (status.isOK()) {
        for (auto& dn: qresult) {
            roles.insert(RoleName{dn, kAdmin});
        }
    }
    return status;
}

}  // namespace mongo

