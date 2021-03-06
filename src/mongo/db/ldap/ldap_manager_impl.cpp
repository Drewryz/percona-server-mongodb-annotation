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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/db/ldap/ldap_manager_impl.h"

#include <regex>

#include <fmt/format.h>
#include <sasl/sasl.h>

#include "mongo/bson/json.h"
#include "mongo/db/ldap_options.h"
#include "mongo/logv2/log.h"
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
    const char* ldapprot = "ldaps";
    if (ldapGlobalParams.ldapTransportSecurity == "none")
        ldapprot = "ldap";
    auto uri = "{}://{}/"_format(ldapprot, ldapGlobalParams.ldapServers.get());
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

    return LDAPbind(_ldap,
                    ldapGlobalParams.ldapQueryUser.get(),
                    ldapGlobalParams.ldapQueryPassword.get());
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

    LOGV2_DEBUG(29051, 1, "Parsing LDAP URL: {ldapurl}; dn: {dn}; scope: {scope}; filter: {filter}",
            "ldapurl"_attr = ldapurl,
            "scope"_attr = ludp->lud_scope,
            "dn"_attr = ludp->lud_dn ? ludp->lud_dn : "nullptr",
            "filter"_attr = ludp->lud_filter ? ludp->lud_filter : "nullptr");
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

    //Parameter validator checks that mapping is valid array of objects
    //see validateLDAPUserToDNMapping function
    BSONArray bsonmapping{fromjson(mapping)};
    for (const auto& elt: bsonmapping) {
        auto step = elt.Obj();
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
            {
                std::regex rex{R"(\{(\d+)\})"};
                std::string ss;
                const std::string stempl = eltempl.str();
                ss.reserve(stempl.length() * 2);
                std::sregex_iterator it{stempl.begin(), stempl.end(), rex};
                std::sregex_iterator end;
                auto suffix_len = stempl.length();
                for (; it != end; ++it) {
                    ss += it->prefix();
                    ss += sm[std::stol((*it)[1].str()) + 1].str();
                    suffix_len = it->suffix().length();
                }
                ss += stempl.substr(stempl.length() - suffix_len);
                out = std::move(ss);
            }
            // in substitution mode we are done - just return 'out'
            if (substitution)
                return Status::OK();
            // in ldapQuery mode we need to execute query and make decision based on query result
            auto ldapurl = fmt::format("ldap://{Servers}/{Query}",
                fmt::arg("Servers", ldapGlobalParams.ldapServers.get()),
                fmt::arg("Query", out));
            std::vector<std::string> qresult;
            auto status = execQuery(ldapurl, qresult);
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

    const std::string providedUser{userName.getUser()};
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


extern "C" {

struct interactionParameters {
    const char* realm;
    const char* dn;
    const char* pw;
    const char* userid;
};

static int interaction(unsigned flags, sasl_interact_t *interact, void *defaults) {
    interactionParameters *params = (interactionParameters*)defaults;
    const char *dflt = interact->defresult;

    switch (interact->id) {
    case SASL_CB_GETREALM:
        dflt = params->realm;
        break;
    case SASL_CB_AUTHNAME:
        dflt = params->dn;
        break;
    case SASL_CB_PASS:
        dflt = params->pw;
        break;
    case SASL_CB_USER:
        dflt = params->userid;
        break;
    }

    if (dflt && !*dflt)
        dflt = NULL;

    if (flags != LDAP_SASL_INTERACTIVE &&
        (dflt || interact->id == SASL_CB_USER)) {
        goto use_default;
    }

    if( flags == LDAP_SASL_QUIET ) {
        /* don't prompt */
        return LDAP_OTHER;
    }


use_default:
    interact->result = (dflt && *dflt) ? dflt : "";
    interact->len = std::strlen( (char*)interact->result );

    return LDAP_SUCCESS;
}

static int interactProc(LDAP *ld, unsigned flags, void *defaults, void *in) {
    sasl_interact_t *interact = (sasl_interact_t*)in;

    if (ld == NULL)
        return LDAP_PARAM_ERROR;

    while (interact->id != SASL_CB_LIST_END) {
        int rc = interaction( flags, interact, defaults );
        if (rc)
            return rc;
        interact++;
    }
    
    return LDAP_SUCCESS;
}

} // extern "C"

Status LDAPbind(LDAP* ld, const char* usr, const char* psw) {
    if (ldapGlobalParams.ldapBindMethod == "simple") {
        // ldap_simple_bind_s was deprecated in favor of ldap_sasl_bind_s
        berval cred;
        cred.bv_val = (char*)psw;
        cred.bv_len = std::strlen(psw);
        auto res = ldap_sasl_bind_s(ld, usr, LDAP_SASL_SIMPLE, &cred,
                               nullptr, nullptr, nullptr);
        if (res != LDAP_SUCCESS) {
            return Status(ErrorCodes::LDAPLibraryError,
                          "Failed to authenticate '{}' using simple bind; LDAP error: {}"_format(
                              usr, ldap_err2string(res)));
        }
    } else if (ldapGlobalParams.ldapBindMethod == "sasl") {
        interactionParameters params;
        params.userid = usr;
        params.dn = usr;
        params.pw = psw;
        params.realm = nullptr;
        auto res = ldap_sasl_interactive_bind_s(
                ld,
                nullptr,
                ldapGlobalParams.ldapBindSaslMechanisms.c_str(),
                nullptr,
                nullptr,
                LDAP_SASL_QUIET,
                interactProc,
                &params);
        if (res != LDAP_SUCCESS) {
            return Status(ErrorCodes::LDAPLibraryError,
                          "Failed to authenticate '{}' using sasl bind; LDAP error: {}"_format(
                              usr, ldap_err2string(res)));
        }
    } else {
        return Status(ErrorCodes::OperationFailed,
                      "Unknown bind method: {}"_format(ldapGlobalParams.ldapBindMethod));
    }
    return Status::OK();
}

Status LDAPbind(LDAP* ld, const std::string& usr, const std::string& psw) {
    return LDAPbind(ld, usr.c_str(), psw.c_str());
}

}  // namespace mongo

