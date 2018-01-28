/*
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/sasl_scram_server_conversation.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/text.h"

namespace mongo {

using std::unique_ptr;
using std::string;

StatusWith<bool> SaslSCRAMServerConversation::step(StringData inputData, std::string* outputData) {
    std::vector<std::string> input = StringSplitter::split(inputData.toString(), ",");
    _step++;

    if (_step > 3 || _step <= 0) {
        return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                mongoutils::str::stream() << "Invalid SCRAM authentication step: "
                                                          << _step);
    }
    if (_step == 1) {
        return _firstStep(input, outputData);
    }
    if (_step == 2) {
        return _secondStep(input, outputData);
    }

    *outputData = "";

    return StatusWith<bool>(true);
}

/*
 * RFC 5802 specifies that in SCRAM user names characters ',' and '=' are encoded as
 * =2C and =3D respectively.
 */
static void decodeSCRAMUsername(std::string& user) {
    boost::replace_all(user, "=2C", ",");
    boost::replace_all(user, "=3D", "=");
}

/*
 * Parse client-first-message of the form:
 * n,a=authzid,n=encoded-username,r=client-nonce
 *
 * Generate server-first-message on the form:
 * r=client-nonce|server-nonce,s=user-salt,i=iteration-count
 *
 * NOTE: we are ignoring the authorization ID part of the message
 */
StatusWith<bool> SaslSCRAMServerConversation::_firstStep(std::vector<string>& input,
                                                         std::string* outputData) {
    std::string authzId = "";

    if (input.size() == 4) {
        /* The second entry a=authzid is optional. If provided it will be
         * validated against the encoded username.
         *
         * The two allowed input forms are:
         * n,,n=encoded-username,r=client-nonce
         * n,a=authzid,n=encoded-username,r=client-nonce
         */
        if (!str::startsWith(input[1], "a=") || input[1].size() < 3) {
            return StatusWith<bool>(ErrorCodes::BadValue,
                                    mongoutils::str::stream() << "Incorrect SCRAM authzid: "
                                                              << input[1]);
        }
        authzId = input[1].substr(2);
        input.erase(input.begin() + 1);
    }

    if (input.size() != 3) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Incorrect number of arguments for first SCRAM client message, got "
                << input.size()
                << " expected 4");
    } else if (str::startsWith(input[0], "p=")) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream()
                                    << "Server does not support channel binding");
    } else if (input[0] != "n" && input[0] != "y") {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream() << "Incorrect SCRAM client message prefix: " << input[0]);
    } else if (!str::startsWith(input[1], "n=") || input[1].size() < 3) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM user name: "
                                                          << input[1]);
    } else if (!str::startsWith(input[2], "r=") || input[2].size() < 6) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM client nonce: "
                                                          << input[2]);
    }

    _user = input[1].substr(2);
    if (!authzId.empty() && _user != authzId) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "SCRAM user name " << _user
                                                          << " does not match authzid "
                                                          << authzId);
    }

    decodeSCRAMUsername(_user);

    // SERVER-16534, SCRAM-SHA-1 must be enabled for authenticating the internal user, so that
    // cluster members may communicate with each other. Hence ignore disabled auth mechanism
    // for the internal user.
    UserName user(_user, _saslAuthSession->getAuthenticationDatabase());
    if (!sequenceContains(saslGlobalParams.authenticationMechanisms, "SCRAM-SHA-1") &&
        user != internalSecurity.user->getName()) {
        return StatusWith<bool>(ErrorCodes::BadValue, "SCRAM-SHA-1 authentication is disabled");
    }

    // add client-first-message-bare to _authMessage
    _authMessage += input[1] + "," + input[2] + ",";

    std::string clientNonce = input[2].substr(2);

    // The authentication database is also the source database for the user.
    User* userObj;
    Status status =
        _saslAuthSession->getAuthorizationSession()->getAuthorizationManager().acquireUser(
            _saslAuthSession->getOpCtxt(), user, &userObj);

    if (!status.isOK()) {
        return StatusWith<bool>(status);
    }

    _creds = userObj->getCredentials();
    UserName userName = userObj->getName();

    _saslAuthSession->getAuthorizationSession()->getAuthorizationManager().releaseUser(userObj);

    if (!initAndValidateCredentials()) {
        // Check for authentication attempts of the __system user on
        // systems started without a keyfile.
        if (userName == internalSecurity.user->getName()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          "It is not possible to authenticate as the __system user "
                          "on servers started without a --keyFile parameter");
        } else {
            return Status(ErrorCodes::AuthenticationFailed,
                          "Unable to perform SCRAM authentication for a user with missing "
                          "or invalid SCRAM credentials");
        }
    }

    // Generate server-first-message
    // Create text-based nonce as base64 encoding of a binary blob of length multiple of 3
    const int nonceLenQWords = 3;
    uint64_t binaryNonce[nonceLenQWords];

    unique_ptr<SecureRandom> sr(SecureRandom::create());

    binaryNonce[0] = sr->nextInt64();
    binaryNonce[1] = sr->nextInt64();
    binaryNonce[2] = sr->nextInt64();

    _nonce =
        clientNonce + base64::encode(reinterpret_cast<char*>(binaryNonce), sizeof(binaryNonce));
    StringBuilder sb;
    sb << "r=" << _nonce << ",s=" << getSalt() << ",i=" << getIterationCount();
    *outputData = sb.str();

    // add server-first-message to authMessage
    _authMessage += *outputData + ",";

    return StatusWith<bool>(false);
}

/**
 * Parse client-final-message of the form:
 * c=channel-binding(base64),r=client-nonce|server-nonce,p=ClientProof
 *
 * Generate successful authentication server-final-message on the form:
 * v=ServerSignature
 *
 * or failed authentication server-final-message on the form:
 * e=message
 *
 * NOTE: we are ignoring the channel binding part of the message
**/
StatusWith<bool> SaslSCRAMServerConversation::_secondStep(const std::vector<string>& input,
                                                          std::string* outputData) {
    if (input.size() != 3) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Incorrect number of arguments for second SCRAM client message, got "
                << input.size()
                << " expected 3");
    } else if (!str::startsWith(input[0], "c=") || input[0].size() < 3) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM channel binding: "
                                                          << input[0]);
    } else if (!str::startsWith(input[1], "r=") || input[1].size() < 6) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM client|server nonce: "
                                                          << input[1]);
    } else if (!str::startsWith(input[2], "p=") || input[2].size() < 3) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM ClientProof: "
                                                          << input[2]);
    }

    // add client-final-message-without-proof to authMessage
    _authMessage += input[0] + "," + input[1];

    // Concatenated nonce sent by client should equal the one in server-first-message
    std::string nonce = input[1].substr(2);
    if (nonce != _nonce) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Unmatched SCRAM nonce received from client in second step, expected "
                << _nonce
                << " but received "
                << nonce);
    }

    std::string clientProof = input[2].substr(2);

    // Do server side computations, compare storedKeys and generate client-final-message
    // AuthMessage     := client-first-message-bare + "," +
    //                    server-first-message + "," +
    //                    client-final-message-without-proof
    // ClientSignature := HMAC(StoredKey, AuthMessage)
    // ClientKey := ClientSignature XOR ClientProof
    // ServerSignature := HMAC(ServerKey, AuthMessage)
    invariant(initAndValidateCredentials());

    if (!verifyClientProof(base64::decode(clientProof))) {
        return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                mongoutils::str::stream()
                                    << "SCRAM authentication failed, storedKey mismatch");
    }

    // ServerSignature := HMAC(ServerKey, AuthMessage)
    const auto serverSignature = generateServerSignature();

    StringBuilder sb;
    sb << "v=" << serverSignature;
    *outputData = sb.str();

    return StatusWith<bool>(false);
}
}  // namespace mongo
