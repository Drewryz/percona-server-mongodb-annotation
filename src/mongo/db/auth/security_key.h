/**
*    Copyright (C) 2009 10gen Inc.
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

#include <string>

#include "mongo/client/dbclientinterface.h"

namespace mongo {
    /**
     * @return true if internal authentication parameters has been set up
     */
    extern bool isInternalAuthSet();

    /**
     * This method initializes the internalSecurity object with authentication
     * credentials to be used by authenticateInternalUser. 
     */
    extern void setInternalUserAuthParams(const BSONObj& authParamsIn);

    /**
     * This method authenticates to another cluster member using appropriate
     * authentication data
     * @return true if the authentication was succesful
     */
    extern bool authenticateInternalUser(DBClientWithCommands* conn);

    /**
     * This method checks the validity of filename as a security key, hashes its
     * contents, and stores it in the internalSecurity variable.  Prints an
     * error message to the logs if there's an error.
     * @param filename the file containing the key
     * @return if the key was successfully stored
     */
    bool setUpSecurityKey(const std::string& filename);

} // namespace mongo
