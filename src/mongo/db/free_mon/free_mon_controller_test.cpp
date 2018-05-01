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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <future>
#include <iostream>
#include <snappy.h>

#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_storage.h"

#include "mongo/base/data_type_validated.h"
#include "mongo/base/deinitializer_context.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/object_check.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"


namespace mongo {
namespace {


class FreeMonMetricsCollectorMock : public FreeMonCollectorInterface {
public:
    ~FreeMonMetricsCollectorMock() {
        // ASSERT_TRUE(_state == State::kStarted);
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) final {
        _state = State::kStarted;

        builder.append("mock", "some data");

        {
            stdx::lock_guard<stdx::mutex> lck(_mutex);

            ++_counter;

            if (_counter == _wait) {
                _condvar.notify_all();
            }
        }
    }

    std::string name() const final {
        return "mock";
    }

    void setSignalOnCount(int c) {
        _wait = c;
    }

    std::uint32_t count() {
        stdx::lock_guard<stdx::mutex> lck(_mutex);
        return _counter;
    }

    void wait() {
        stdx::unique_lock<stdx::mutex> lck(_mutex);
        while (_counter < _wait) {
            _condvar.wait(lck);
        }
    }

private:
    /**
    * Private enum to ensure caller uses class correctly.
    */
    enum class State {
        kNotStarted,
        kStarted,
    };

    // state
    State _state{State::kNotStarted};

    std::uint32_t _counter{0};

    stdx::mutex _mutex;
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

std::vector<BSONObj> decompressMetrics(ConstDataRange cdr) {
    std::string outBuffer;
    snappy::Uncompress(cdr.data(), cdr.length(), &outBuffer);

    std::vector<BSONObj> metrics;
    ConstDataRangeCursor cdrc(outBuffer.data(), outBuffer.data() + outBuffer.size());
    while (!cdrc.empty()) {
        auto swDoc = cdrc.readAndAdvance<Validated<BSONObj>>();
        ASSERT_OK(swDoc.getStatus());
        metrics.emplace_back(swDoc.getValue().val.getOwned());
    }

    return metrics;
}

/**
 * Countdown latch that propagates a message.
 */
template <typename T>
class CountdownLatchResult {
public:
    CountdownLatchResult(uint32_t count) : _count(count) {}

    /**
     * Set the count of events to wait for.
     */
    void reset(uint32_t count) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        ASSERT_EQ(_count, 0UL);
        ASSERT_GT(count, 0UL);

        _count = count;
        _payload = T();
    }

    /**
     * Set the payload and signal waiter.
     */
    void set(T payload) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (_count > 0) {
            --_count;
            _payload = std::move(payload);
            _condvar.notify_one();
        }
    }

    /**
     * Waits for duration until N events have occured.
     *
     * Returns boost::none on timeout.
     */
    boost::optional<T> wait_for(Milliseconds duration) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);

        if (!_condvar.wait_for(
                lock, duration.toSystemDuration(), [this]() { return _count == 0; })) {
            return {};
        }

        return _payload;
    }

private:
    // Condition variable to signal consumer
    stdx::condition_variable _condvar;

    // Lock for condition variable and to protect state
    stdx::mutex _mutex;

    // Count to wait fore
    uint32_t _count;

    // Provided payload
    T _payload;
};

class FreeMonNetworkInterfaceMock : public FreeMonNetworkInterface {
public:
    struct Options {
        // If sync = true, then execute the callback immediately and the subsequent future chain
        // This allows us to ensure the follow up functions to a network request are executed
        // before anything else is processed by FreeMonProcessor
        bool doSync{false};

        // Faults to inject for registration
        bool failRegisterHttp{false};
        bool invalidRegister{false};
        bool haltRegister{false};

        // Faults to inject for metrics
        bool haltMetrics{false};
        bool fail2MetricsUploads{false};
        bool permanentlyDeleteAfter3{false};
    };

    explicit FreeMonNetworkInterfaceMock(executor::ThreadPoolTaskExecutor* threadPool,
                                         Options options)
        : _threadPool(threadPool), _options(options), _countdownMetrics(0) {}
    ~FreeMonNetworkInterfaceMock() final = default;

    Future<FreeMonRegistrationResponse> sendRegistrationAsync(
        const FreeMonRegistrationRequest& req) final {
        log() << "Sending Registration ...";

        _registers.addAndFetch(1);

        Promise<FreeMonRegistrationResponse> promise;
        auto future = promise.getFuture();
        if (_options.doSync) {
            promise.setFrom(doRegister(req));
        } else {
            auto shared_promise = promise.share();

            auto swSchedule = _threadPool->scheduleWork([shared_promise, req, this](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {

                auto swResp = doRegister(req);
                if (!swResp.isOK()) {
                    shared_promise.setError(swResp.getStatus());
                } else {
                    shared_promise.emplaceValue(swResp.getValue());
                }

            });

            ASSERT_OK(swSchedule.getStatus());
        }

        return future;
    }

    StatusWith<FreeMonRegistrationResponse> doRegister(const FreeMonRegistrationRequest& req) {

        if (_options.failRegisterHttp) {
            return Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Mock failure");
        }

        auto resp = FreeMonRegistrationResponse();
        resp.setVersion(1);

        if (_options.invalidRegister) {
            resp.setVersion(42);
        }

        resp.setId("regId123");

        if (_options.haltRegister) {
            resp.setHaltMetricsUploading(true);
        }

        resp.setReportingInterval(1);

        return resp;
    }


    Future<FreeMonMetricsResponse> sendMetricsAsync(const FreeMonMetricsRequest& req) final {
        log() << "Sending Metrics ...";

        _metrics.addAndFetch(1);

        Promise<FreeMonMetricsResponse> promise;
        auto future = promise.getFuture();
        if (_options.doSync) {
            promise.setFrom(doMetrics(req));
        } else {
            auto shared_promise = promise.share();

            auto swSchedule = _threadPool->scheduleWork([shared_promise, req, this](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {

                auto swResp = doMetrics(req);
                if (!swResp.isOK()) {
                    shared_promise.setError(swResp.getStatus());
                } else {
                    shared_promise.emplaceValue(swResp.getValue());
                }

            });

            ASSERT_OK(swSchedule.getStatus());
        }

        return future;
    }

    StatusWith<FreeMonMetricsResponse> doMetrics(const FreeMonMetricsRequest& req) {
        auto cdr = req.getMetrics();

        {
            stdx::lock_guard<stdx::mutex> lock(_metricsLock);
            auto metrics = decompressMetrics(cdr);
            _lastMetrics = metrics;
            _countdownMetrics.set(metrics);
        }

        if (_options.fail2MetricsUploads && _metrics.loadRelaxed() < 3) {
            return Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Mock failure");
        }

        auto resp = FreeMonMetricsResponse();
        resp.setVersion(1);
        resp.setReportingInterval(1);

        resp.setId("metricsId456"_sd);

        if (_options.haltMetrics) {
            resp.setHaltMetricsUploading(true);
        }

        if (_options.permanentlyDeleteAfter3 && _metrics.loadRelaxed() > 3) {
            resp.setPermanentlyDelete(true);
        }

        return resp;
    }

    int32_t getRegistersCalls() const {
        return _registers.load();
    }

    int32_t getMetricsCalls() const {
        return _metrics.load();
    }

    boost::optional<std::vector<BSONObj>> waitMetricsCalls(uint32_t count, Milliseconds wait) {
        _countdownMetrics.reset(count);
        return _countdownMetrics.wait_for(wait);
    }

    std::vector<BSONObj> getLastMetrics() {
        stdx::lock_guard<stdx::mutex> lock(_metricsLock);
        return _lastMetrics;
    }


private:
    AtomicInt32 _registers;
    AtomicInt32 _metrics;

    executor::ThreadPoolTaskExecutor* _threadPool;

    stdx::mutex _metricsLock;
    std::vector<BSONObj> _lastMetrics;

    Options _options;

    CountdownLatchResult<std::vector<BSONObj>> _countdownMetrics;
};

class FreeMonControllerTest : public ServiceContextMongoDTest {

private:
    void setUp() final;
    void tearDown() final;

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    executor::NetworkInterfaceMock* _mockNetwork{nullptr};

    std::unique_ptr<executor::ThreadPoolTaskExecutor> _mockThreadPool;
};

void FreeMonControllerTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    // Set up a NetworkInterfaceMock. Note, unlike NetworkInterfaceASIO, which has its own pool of
    // threads, tasks in the NetworkInterfaceMock must be carried out synchronously by the (single)
    // thread the unit test is running on.
    auto netForFixedTaskExecutor = std::make_unique<executor::NetworkInterfaceMock>();
    _mockNetwork = netForFixedTaskExecutor.get();

    // Set up a ThreadPoolTaskExecutor. Note, for local tasks this TaskExecutor uses a
    // ThreadPoolMock, and for remote tasks it uses the NetworkInterfaceMock created above. However,
    // note that the ThreadPoolMock uses the NetworkInterfaceMock's threads to run tasks, which is
    // again just the (single) thread the unit test is running on. Therefore, all tasks, local and
    // remote, must be carried out synchronously by the test thread.
    _mockThreadPool = makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));

    _mockThreadPool->startup();

    _opCtx = cc().makeOperationContext();

    //_storage = stdx::make_unique<repl::StorageInterfaceImpl>();
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());


    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));


    // Create collection with one document.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();

    auto statusCC = repl::StorageInterface::get(service)->createCollection(
        _opCtx.get(), NamespaceString("admin", "system.version"), collectionOptions);
    ASSERT_OK(statusCC);
}

void FreeMonControllerTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

repl::ReplicationCoordinatorMock* FreeMonControllerTest::_getReplCoord() const {
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT(replCoord) << "No ReplicationCoordinator installed";
    auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock) << "Unexpected type for installed ReplicationCoordinator";
    return replCoordMock;
}

#define ASSERT_RANGE(target, lower, upper)    \
    {                                         \
        auto __x = counter.getNextDuration(); \
        ASSERT_GTE(__x, target + lower);      \
        ASSERT_LTE(__x, target + upper);      \
    }

// Positive: Ensure deadlines sort properly
TEST(FreeMonRetryTest, TestRegistration) {
    PseudoRandom random(0);
    RegistrationRetryCounter counter(random);
    counter.reset();

    ASSERT_EQ(counter.getNextDuration(), Seconds(1));
    ASSERT_EQ(counter.getNextDuration(), Seconds(1));

    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 1; i <= 10; ++i) {
            ASSERT_TRUE(counter.incrementError());

            int64_t base = pow(2, i);
            ASSERT_RANGE(Seconds(base), Seconds(2), Seconds(10));
        }

        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(1024), Seconds(60), Seconds(120));
        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(1024), Seconds(60), Seconds(120));

        counter.reset();
    }

    // Validate max timeout
    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 1; i <= 163; ++i) {
            ASSERT_TRUE(counter.incrementError());
        }
        ASSERT_FALSE(counter.incrementError());

        counter.reset();
    }
}

// Positive: Ensure deadlines sort properly
TEST(FreeMonRetryTest, TestMetrics) {
    PseudoRandom random(0);
    MetricsRetryCounter counter(random);
    counter.reset();

    ASSERT_EQ(counter.getNextDuration(), Seconds(1));
    ASSERT_EQ(counter.getNextDuration(), Seconds(1));

    int32_t minTime = 1;
    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 0; i <= 6; ++i) {
            ASSERT_TRUE(counter.incrementError());

            int64_t base = pow(2, i);
            ASSERT_RANGE(Seconds(base), Seconds(minTime / 2), Seconds(minTime));
        }

        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(64), Seconds(minTime / 2), Seconds(minTime));
        ASSERT_TRUE(counter.incrementError());
        ASSERT_RANGE(Seconds(64), Seconds(minTime / 2), Seconds(minTime));

        counter.reset();
    }

    // Validate max timeout
    for (int j = 0; j < 3; j++) {
        // Fail requests
        for (int i = 1; i < 9456; ++i) {
            ASSERT_TRUE(counter.incrementError());
        }
        ASSERT_FALSE(counter.incrementError());

        counter.reset();
    }
}

// Positive: Ensure the response is validated correctly
TEST(FreeMonProcessorTest, TestRegistrationResponseValidation) {
    ASSERT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: bad protocol version
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 42LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: halt uploading
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << true << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large registartation id
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id" << std::string(5000, 'a')
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large URL
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << std::string(5000, 'b')
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large message
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << std::string(5000, 'c')
                       << "reportingInterval"
                       << 1LL))));

    // Negative: too small a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 0LL))));

    // Negative: too large a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateRegistrationResponse(FreeMonRegistrationResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << (60LL * 60 * 24 + 1LL)))));
}


// Positive: Ensure the response is validated correctly
TEST(FreeMonProcessorTest, TestMetricsResponseValidation) {
    ASSERT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),

        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: bad protocol version
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 42LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: halt uploading
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << true << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large registartation id
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << std::string(5000, 'a')
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 1LL))));

    // Negative: large URL
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(
        FreeMonMetricsResponse::parse(IDLParserErrorContext("foo"),
                                      BSON("version" << 1LL << "haltMetricsUploading" << false

                                                     << "permanentlyDelete"
                                                     << false
                                                     << "id"
                                                     << "mock123"
                                                     << "informationalURL"
                                                     << std::string(5000, 'b')
                                                     << "message"
                                                     << "msg456"
                                                     << "reportingInterval"
                                                     << 1LL))));

    // Negative: large message
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << std::string(5000, 'c')
                       << "reportingInterval"
                       << 1LL))));

    // Negative: too small a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << 0LL))));

    // Negative: too large a reporting interval
    ASSERT_NOT_OK(FreeMonProcessor::validateMetricsResponse(FreeMonMetricsResponse::parse(
        IDLParserErrorContext("foo"),
        BSON("version" << 1LL << "haltMetricsUploading" << false << "permanentlyDelete" << false
                       << "id"
                       << "mock123"
                       << "informationalURL"
                       << "http://www.example.com/123"
                       << "message"
                       << "msg456"
                       << "reportingInterval"
                       << (60LL * 60 * 24 + 1LL)))));
}

/**
 * Fluent class that encapsulates how many turns of a crank is needed to do a particular operation.
 *
 * All commands take 1 turn except registerCommand and metricsSend since these have a HTTP send an
 * HTTP receive.
 */
class Turner {
public:
    Turner() = default;

    Turner& registerServer() {
        return inc(1, 1);
    }

    Turner& registerCommand(size_t count = 1) {
        return inc(2, count);
    }

    Turner& unRegisterCommand() {
        return inc(1, 1);
    }

    Turner& collect(size_t count = 1) {
        return inc(1, count);
    }

    Turner& metricsSend(size_t count = 1) {
        return inc(2, count);
    }

    operator size_t() {
        return _count;
    }

private:
    Turner& inc(size_t perOperatioCost, size_t numberOfOperations) {
        _count += (perOperatioCost * numberOfOperations);
        return *this;
    }

private:
    size_t _count;
};

/**
 * Utility class to manage controller setup and lifecycle for testing.
 */
struct ControllerHolder {
    ControllerHolder(executor::ThreadPoolTaskExecutor* pool,
                     FreeMonNetworkInterfaceMock::Options opts,
                     bool useCrankForTest = true) {
        auto registerCollectorUnique = stdx::make_unique<FreeMonMetricsCollectorMock>();
        auto metricsCollectorUnique = stdx::make_unique<FreeMonMetricsCollectorMock>();

        // If we want to manually turn the crank the queue, we must process the messages
        // synchronously
        if (useCrankForTest) {
            opts.doSync = true;
        }

        ASSERT_EQ(opts.doSync, useCrankForTest);

        auto networkUnique =
            std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkInterfaceMock(pool, opts));
        network = static_cast<FreeMonNetworkInterfaceMock*>(networkUnique.get());
        controller = std::make_unique<FreeMonController>(std::move(networkUnique), useCrankForTest);

        registerCollector = registerCollectorUnique.get();
        metricsCollector = metricsCollectorUnique.get();

        controller->addRegistrationCollector(std::move(registerCollectorUnique));
        controller->addMetricsCollector(std::move(metricsCollectorUnique));
    }

    ~ControllerHolder() {
        controller->stop();
    }

    void start(RegistrationType registrationType) {
        std::vector<std::string> tags;
        controller->start(registrationType, tags);
    }


    FreeMonController* operator->() {
        return controller.get();
    }

    FreeMonMetricsCollectorMock* registerCollector;
    FreeMonMetricsCollectorMock* metricsCollector;
    FreeMonNetworkInterfaceMock* network;

    std::unique_ptr<FreeMonController> controller;
};


// Positive: Test Register works
TEST_F(FreeMonControllerTest, TestRegister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().registerCommand());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 0UL);
}

// Negatve: Test Register times out if network stack drops messages
TEST_F(FreeMonControllerTest, TestRegisterTimeout) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.failRegisterHttp = true;

    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));
    controller->turnCrankForTest(Turner().registerCommand(2));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);
    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);
}

// Negatve: Test Register times out if the registration is wrong
TEST_F(FreeMonControllerTest, TestRegisterFail) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.invalidRegister = true;
    ControllerHolder controller(_mockThreadPool.get(), opts, false);

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_NOT_OK(controller->registerServerCommand(duration_cast<Milliseconds>(Seconds(15))));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);
    ASSERT_EQ(controller.network->getRegistersCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
}

// Positive: Ensure registration halts
TEST_F(FreeMonControllerTest, TestRegisterHalts) {

    FreeMonNetworkInterfaceMock::Options opts;
    opts.haltRegister = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));
    controller->turnCrankForTest(Turner().registerCommand());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);
    ASSERT_EQ(controller.network->getRegistersCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
}

// Positive: Test Metrics works on server register
TEST_F(FreeMonControllerTest, TestMetrics) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}


// Positive: Test Metrics is collected but no registration happens on empty storage
TEST_F(FreeMonControllerTest, TestMetricsWithEmptyStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_GTE(controller.network->getRegistersCalls(), 0);
    ASSERT_GTE(controller.network->getMetricsCalls(), 0);

    ASSERT_EQ(controller.registerCollector->count(), 0UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}

FreeMonStorageState initStorage(StorageStateEnum e) {
    FreeMonStorageState storage;
    storage.setVersion(1UL);

    storage.setRegistrationId("Foo");
    storage.setState(e);
    storage.setInformationalURL("http://www.example.com");
    storage.setMessage("Hello World");
    storage.setUserReminder("");
    return storage;
}

// Positive: Test Metrics is collected and implicit registration happens when storage is initialized
TEST_F(FreeMonControllerTest, TestMetricsWithEnabledStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}

// Positive: Test Metrics is collected but no registration happens on disabled storage
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorage) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_GTE(controller.network->getRegistersCalls(), 0);
    ASSERT_GTE(controller.network->getMetricsCalls(), 0);

    ASSERT_EQ(controller.registerCollector->count(), 0UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}


// Positive: Test Metrics is collected but no registration happens on disabled storage until user
// registers
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorageThenRegister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().registerCommand().collect(2).metricsSend());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL + 2UL);
}

// Positive: Test Metrics is collected but no registration happens, then register, then Unregister,
// and finally register again
TEST_F(FreeMonControllerTest, TestMetricsWithDisabledStorageThenRegisterAndReregister) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::disabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().collect(4));

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::enabled);

    ASSERT_OK(controller->unregisterServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().unRegisterCommand().collect(3));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::disabled);

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().registerCommand().collect(2).metricsSend());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get())->getState() == StorageStateEnum::enabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 2UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL + 3UL + 2UL);
}

// Positive: Test DeRegister cancels a register that is in the middle of retrying
TEST_F(FreeMonControllerTest, TestMetricsUnregisterCancelsRegister) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.failRegisterHttp = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::DoNotRegister);

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));
    controller->turnCrankForTest(Turner().registerCommand(2));

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::pending);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);

    ASSERT_OK(controller->unregisterServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().unRegisterCommand());

    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 2);
    ASSERT_GTE(controller.registerCollector->count(), 2UL);
}

// Positive: Test Metrics halts
TEST_F(FreeMonControllerTest, TestMetricsHalt) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.haltMetrics = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(4).metricsSend());

    ASSERT_TRUE(!FreeMonStorage::read(_opCtx.get()).get().getRegistrationId().empty());
    ASSERT_TRUE(FreeMonStorage::read(_opCtx.get()).get().getState() == StorageStateEnum::disabled);

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 4UL);
}


// Positive: Test Metrics permanently deletes if requested
TEST_F(FreeMonControllerTest, TestMetricsPermanentlyDelete) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.permanentlyDeleteAfter3 = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(
        Turner().registerServer().registerCommand().collect(5).metricsSend(4));

    ASSERT_FALSE(FreeMonStorage::read(_opCtx.get()).is_initialized());

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 3);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 3UL);
}

// Positive: ensure registration id rotates
TEST_F(FreeMonControllerTest, TestRegistrationIdRotatesAfterRegistration) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    FreeMonStorage::replace(_opCtx.get(), initStorage(StorageStateEnum::enabled));

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);
    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(2));

    // Ensure registration rotated the id
    ASSERT_EQ(FreeMonStorage::read(_opCtx.get())->getRegistrationId(), "regId123");

    controller->turnCrankForTest(Turner().metricsSend().collect());

    // Ensure metrics rotated the id
    ASSERT_EQ(FreeMonStorage::read(_opCtx.get())->getRegistrationId(), "metricsId456");

    ASSERT_GTE(controller.network->getRegistersCalls(), 1);
    ASSERT_GTE(controller.network->getMetricsCalls(), 1);

    ASSERT_EQ(controller.registerCollector->count(), 1UL);
    ASSERT_GTE(controller.metricsCollector->count(), 1UL);
}

// Positive: ensure pre-registration metrics batching occurs
// Positive: ensure we only get two metrics each time
TEST_F(FreeMonControllerTest, TestPreRegistrationMetricBatching) {
    ControllerHolder controller(_mockThreadPool.get(), FreeMonNetworkInterfaceMock::Options());

    controller.start(RegistrationType::RegisterAfterOnTransitionToPrimary);

    controller->turnCrankForTest(Turner().registerServer().collect(3));

    ASSERT_OK(controller->registerServerCommand(Milliseconds::min()));

    controller->turnCrankForTest(Turner().registerCommand().collect(1));

    controller->turnCrankForTest(Turner().metricsSend().collect(1));

    // Ensure we sent all the metrics batched before registration
    ASSERT_EQ(controller.network->getLastMetrics().size(), 4UL);

    controller->turnCrankForTest(Turner().metricsSend().collect(1));

    // Ensure we only send 2 metrics in the normal happy case
    ASSERT_EQ(controller.network->getLastMetrics().size(), 2UL);
}

// Negative: Test metrics buffers on failure, and retries
TEST_F(FreeMonControllerTest, TestMetricBatchingOnError) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.fail2MetricsUploads = true;
    ControllerHolder controller(_mockThreadPool.get(), opts);

    controller.start(RegistrationType::RegisterOnStart);

    controller->turnCrankForTest(Turner().registerServer().registerCommand().collect(2));

    controller->turnCrankForTest(Turner().metricsSend().collect());

    // Ensure we sent all the metrics batched before registration
    ASSERT_EQ(controller.network->getLastMetrics().size(), 2UL);

    controller->turnCrankForTest(Turner().metricsSend().collect());

    // Ensure we resent all the failed metrics
    ASSERT_EQ(controller.network->getLastMetrics().size(), 3UL);
}

// Negative: Test metrics buffers on failure, and retries and ensure 2 metrics occurs after a blip
// of an error
// Note: this test operates in real-time because it needs to test multiple retries matched with
// metrics collection.
TEST_F(FreeMonControllerTest, TestMetricBatchingOnErrorRealtime) {
    FreeMonNetworkInterfaceMock::Options opts;
    opts.fail2MetricsUploads = true;
    ControllerHolder controller(_mockThreadPool.get(), opts, false);

    controller.start(RegistrationType::RegisterOnStart);

    // Ensure the first upload sends 2 samples
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().size(), 2UL);

    // Ensure the second upload sends 3 samples because first failed
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().size(), 3UL);

    // Ensure the third upload sends 5 samples because second failed
    // Since the second retry is 2s, we collected 2 samples
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_GTE(controller.network->getLastMetrics().size(), 4UL);

    // Ensure the fourth upload sends 2 samples
    ASSERT_TRUE(controller.network->waitMetricsCalls(1, Seconds(5)).is_initialized());
    ASSERT_EQ(controller.network->getLastMetrics().size(), 2UL);
}

// TODO: Positive: ensure optional fields are rotated

// TODO: Positive: Test Metrics works on command register on primary
// TODO: Positive: Test Metrics works on startup register on secondary
// TODO: Positive: Test Metrics works on secondary after opObserver register
// TODO: Positive: Test Metrics works on secondary after opObserver de-register

}  // namespace
}  // namespace mongo
