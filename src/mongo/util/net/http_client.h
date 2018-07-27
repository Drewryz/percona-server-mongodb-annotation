/**
 * Copyright (C) 2018 MongoDB Inc.
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

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

constexpr uint64_t kConnectionTimeoutSeconds = 60L;
constexpr uint64_t kTotalRequestTimeoutSeconds = 120L;

/**
 * Interface used to upload and receive binary payloads to HTTP servers.
 */
class HttpClient {
public:
    virtual ~HttpClient() = default;

    /**
     * Configure all future requests on this client to allow insecure http:// urls.
     * By default, only https:// is allowed.
     */
    virtual void allowInsecureHTTP(bool allow) = 0;

    /**
     * Assign a set of headers for this request.
     */
    virtual void setHeaders(const std::vector<std::string>& headers) = 0;

    /**
     * Perform a POST request to specified URL.
     */
    virtual DataBuilder post(StringData url, ConstDataRange data) const = 0;

    /**
     * Futurized helper for HttpClient::post().
     */
    Future<DataBuilder> postAsync(executor::ThreadPoolTaskExecutor* executor,
                                  StringData url,
                                  std::shared_ptr<std::vector<std::uint8_t>> data) const;

    /**
     * Perform a GET request from the specified URL.
     */
    virtual DataBuilder get(StringData url) const = 0;

    /**
     * Futurized helpr for HttpClient::get().
     */
    Future<DataBuilder> getAsync(executor::ThreadPoolTaskExecutor* executor, StringData url) const;

    /**
     * Factory method provided by client implementation.
     */
    static std::unique_ptr<HttpClient> create();
};

}  // namespace mongo
