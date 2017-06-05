/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <memory>
#include <timelib.h>

#include "mongo/db/query/datetime/date_time_support.h"

#include "mongo/base/init.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
const auto getDateTimeSupport =
    ServiceContext::declareDecoration<std::unique_ptr<DateTimeSupport>>();
}  // namespace

const DateTimeSupport* DateTimeSupport::get(ServiceContext* serviceContext) {
    invariant(getDateTimeSupport(serviceContext));
    return getDateTimeSupport(serviceContext).get();
}

void DateTimeSupport::set(ServiceContext* serviceContext,
                          std::unique_ptr<DateTimeSupport> dateTimeSupport) {
    getDateTimeSupport(serviceContext) = std::move(dateTimeSupport);
}

DateTimeSupport::DateTimeSupport() {
    loadTimeZoneInfo({const_cast<timelib_tzdb*>(timelib_builtin_db()), TimeZoneDBDeleter()});
}

DateTimeSupport::DateTimeSupport(
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase) {
    loadTimeZoneInfo(std::move(timeZoneDatabase));
}

void DateTimeSupport::TimeZoneDBDeleter::operator()(timelib_tzdb* timeZoneDatabase) {
    if (timeZoneDatabase != timelib_builtin_db()) {
        timelib_zoneinfo_dtor(timeZoneDatabase);
    }
}

DateTimeSupport::~DateTimeSupport() {
    for (auto&& entry : _timeZones) {
        timelib_tzinfo_dtor(entry.second);
    }
}

void DateTimeSupport::loadTimeZoneInfo(
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase) {
    invariant(timeZoneDatabase);
    _timeZoneDatabase = std::move(timeZoneDatabase);
    int nTimeZones;
    auto timezone_identifier_list =
        timelib_timezone_identifiers_list(_timeZoneDatabase.get(), &nTimeZones);
    for (int i = 0; i < nTimeZones; ++i) {
        auto entry = timezone_identifier_list[i];
        int errorCode = TIMELIB_ERROR_NO_ERROR;
        auto tzInfo = timelib_parse_tzfile(entry.id, _timeZoneDatabase.get(), &errorCode);
        if (!tzInfo) {
            invariant(errorCode != TIMELIB_ERROR_NO_ERROR);
            fassertFailedWithStatusNoTrace(
                40474,
                {ErrorCodes::FailedToParse,
                 str::stream() << "failed to parse time zone file for time zone identifier \""
                               << entry.id
                               << "\": "
                               << timelib_get_error_message(errorCode)});
        }
        invariant(errorCode == TIMELIB_ERROR_NO_ERROR);
        _timeZones[entry.id] = tzInfo;
    }
}

}  // namespace mongo
