// tokuft_engine_options.cpp

/*======
This file is part of Percona Server for MongoDB.

Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    Percona Server for MongoDB is free software: you can redistribute
    it and/or modify it under the terms of the GNU Affero General
    Public License, version 3, as published by the Free Software
    Foundation.

    Percona Server for MongoDB is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public
    License along with Percona Server for MongoDB.  If not, see
    <http://www.gnu.org/licenses/>.
======= */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/db/storage/tokuft/tokuft_engine_options.h"

namespace mongo {

    TokuFTEngineOptions::TokuFTEngineOptions()
        : cacheSize(0),
          checkpointPeriod(60),
          cleanerIterations(5),
          cleanerPeriod(2),
          directio(false),
          fsRedzone(5),
          journalCommitInterval(100),
          lockTimeout(100),
          locktreeMaxMemory(0),  // let this be the ft default, computed from cacheSize
          directoryForIndexes(false),
          compressBuffersBeforeEviction(false),
          numCachetableBucketMutexes(1<<20)
    {}

    Status TokuFTEngineOptions::add(moe::OptionSection* options) {
        moe::OptionSection tokuftOptions("TokuFT engine options");

        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.cacheSize",
                "tokuftEngineCacheSize", moe::UnsignedLongLong, "TokuFT engine cache size (bytes)");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.cleanerIterations",
                "tokuftEngineCleanerIterations", moe::Int, "TokuFT engine cleaner iterations");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.cleanerPeriod",
                "tokuftEngineCleanerPeriod", moe::Int, "TokuFT engine cleaner period (s)");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.directio",
                "tokuftEngineDirectio", moe::Bool, "TokuFT engine use Direct I/O");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.fsRedzone",
                "tokuftEngineFsRedzone", moe::Int, "TokuFT engine filesystem redzone");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.journalCommitInterval",
                "tokuftEngineJournalCommitInterval", moe::Int, "TokuFT engine journal commit interval (ms)");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.lockTimeout",
                "tokuftEngineLockTimeout", moe::Int, "TokuFT engine lock wait timeout (ms)");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.locktreeMaxMemory",
                "tokuftEngineLocktreeMaxMemory", moe::UnsignedLongLong, "TokuFT locktree size (bytes)");
        // TODO: MSE-39
        //tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.directoryForIndexes",
        //        "tokuftEngineDirectoryForIndexes", moe::Bool, "TokuFT use a separate directory for indexes");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.compressBuffersBeforeEviction",
                "tokuftEngineCompressBuffersBeforeEviction", moe::Bool, "TokuFT engine compress buffers before eviction");
        tokuftOptions.addOptionChaining("storage.tokuft.engineOptions.numCachetableBucketMutexes",
                "tokuftEngineNumCachetableBucketMutexes", moe::Int, "TokuFT engine num cachetable bucket mutexes");

        return options->addSection(tokuftOptions);
    }

    bool TokuFTEngineOptions::handlePreValidation(const moe::Environment& params) {
        return true;
    }

    Status TokuFTEngineOptions::store(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
        if (params.count("storage.tokuft.engineOptions.cacheSize")) {
            cacheSize = params["storage.tokuft.engineOptions.cacheSize"].as<unsigned long long>();
            if (cacheSize < (1ULL<<30)) {
                warning() << "TokuFT: cacheSize is under 1GB, this is not recommended for production." << std::endl;
            }
        }
        if (params.count("storage.syncPeriodSecs")) {
            checkpointPeriod = static_cast<int>(params["storage.syncPeriodSecs"].as<double>());
            if (checkpointPeriod <= 0) {
                StringBuilder sb;
                sb << "storage.syncPeriodSecs must be > 0, but attempted to set to: "
                   << checkpointPeriod;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.cleanerIterations")) {
            cleanerIterations = params["storage.tokuft.engineOptions.cleanerIterations"].as<int>();
            if (cleanerIterations < 0) {
                StringBuilder sb;
                sb << "storage.tokuft.engineOptions.cleanerIterations must be >= 0, but attempted to set to: "
                   << cleanerIterations;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.cleanerPeriod")) {
            cleanerPeriod = params["storage.tokuft.engineOptions.cleanerPeriod"].as<int>();
            if (cleanerPeriod < 0) {
                StringBuilder sb;
                sb << "storage.tokuft.engineOptions.cleanerPeriod must be >= 0, but attempted to set to: "
                   << cleanerPeriod;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.directio")) {
            directio = params["storage.tokuft.engineOptions.directio"].as<bool>();
        }
        if (params.count("storage.tokuft.engineOptions.fsRedzone")) {
            fsRedzone = params["storage.tokuft.engineOptions.fsRedzone"].as<int>();
            if (fsRedzone < 0 || fsRedzone > 100) {
                StringBuilder sb;
                sb << "storage.tokuft.engineOptions.fsRedzone must be between 0 and 100, but attempted to set to: "
                   << fsRedzone;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.journalCommitInterval")) {
            journalCommitInterval = params["storage.tokuft.engineOptions.journalCommitInterval"].as<int>();
            if (journalCommitInterval < 1 || journalCommitInterval > 300) {
                StringBuilder sb;
                sb << "storage.tokuft.engineOptions.journalCommitInterval must be between 1 and 300, but attempted to set to: "
                   << journalCommitInterval;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.lockTimeout")) {
            lockTimeout = params["storage.tokuft.engineOptions.lockTimeout"].as<int>();
            if (lockTimeout < 0 || lockTimeout > 60000) {
                StringBuilder sb;
                sb << "storage.tokuft.engineOptions.lockTimeout must be between 0 and 60000, but attempted to set to: "
                   << lockTimeout;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("storage.tokuft.engineOptions.locktreeMaxMemory")) {
            locktreeMaxMemory = params["storage.tokuft.engineOptions.locktreeMaxMemory"].as<unsigned long long>();
            if (lockTimeout < (100LL<<20)) {
                warning() << "TokuFT: locktreeMaxMemory is under 100MB, this is not recommended for production." << std::endl;
            }
        }
        // TODO: MSE-39
        //if (params.count("storage.tokuft.engineOptions.directoryForIndexes")) {
        //    directoryForIndexes = params["storage.tokuft.engineOptions.directoryForIndexes"].as<bool>();
        //}
        if (params.count("storage.tokuft.engineOptions.compressBuffersBeforeEviction")) {
            compressBuffersBeforeEviction = params["storage.tokuft.engineOptions.compressBuffersBeforeEviction"].as<bool>();
        }
        if (params.count("storage.tokuft.engineOptions.numCachetableBucketMutexes")) {
            numCachetableBucketMutexes = params["storage.tokuft.engineOptions.numCachetableBucketMutexes"].as<int>();
        }

        return Status::OK();
    }
}
