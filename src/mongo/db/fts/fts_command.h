// fts_command.h

/**
*    Copyright (C) 2012 10gen Inc.
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
#include <vector>

#include "mongo/db/commands.h"

// mongo::fts::FTSCommand is deprecated: the "text" command is deprecated in favor of the $text
// query operator.

namespace mongo {

    namespace fts {

        class FTSCommand : public Command {
        public:
            FTSCommand();

            bool slaveOk() const { return true; }
            bool slaveOverrideOk() const { return true; }

            virtual bool isWriteCommandForConfigServer() const { return false; }

            void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out);


            bool run(TransactionExperiment* txn, const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result,
                     bool fromRepl);

        protected:
            bool _run( const string& dbName,
                       BSONObj& cmdObj,
                       int cmdOptions,
                       const string& ns,
                       const string& searchString,
                       string language, // "" for not-set
                       int limit,
                       BSONObj& filter,
                       BSONObj& projection,
                       string& errmsg,
                       BSONObjBuilder& result );
        };

    }

}

