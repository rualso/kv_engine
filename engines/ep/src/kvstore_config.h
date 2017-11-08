/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include "config.h"

#include "configuration.h"
#include "logger.h"

#include <string>

class Logger;

class KVStoreConfig {
public:
    /**
     * This constructor intialises the object from a central
     * ep-engine Configuration instance.
     */
    KVStoreConfig(Configuration& config, uint16_t shardId);

    /**
     * This constructor sets the mandatory config options
     *
     * Optional config options are set using a separate method
     */
    KVStoreConfig(uint16_t _maxVBuckets,
                  uint16_t _maxShards,
                  const std::string& _dbname,
                  const std::string& _backend,
                  uint16_t _shardId,
                  bool persistDocNamespace,
                  const std::string& rocksDBOptions_ = "",
                  const std::string& rocksDBCFOptions_ = "",
                  const std::string& rocksDbBBTOptions_ = "");

    uint16_t getMaxVBuckets() const {
        return maxVBuckets;
    }

    uint16_t getMaxShards() const {
        return maxShards;
    }

    std::string getDBName() const {
        return dbname;
    }

    const std::string& getBackend() const {
        return backend;
    }

    uint16_t getShardId() const {
        return shardId;
    }

    Logger& getLogger() {
        return *logger;
    }

    /**
     * Indicates whether or not underlying file operations will be
     * buffered by the storage engine used.
     *
     * Only recognised by CouchKVStore
     */
    bool getBuffered() const {
        return buffered;
    }

    /**
     * Used to override the default logger object
     */
    KVStoreConfig& setLogger(Logger& _logger);

    /**
     * Used to override the default buffering behaviour.
     *
     * Only recognised by CouchKVStore
     */
    KVStoreConfig& setBuffered(bool _buffered);

    bool shouldPersistDocNamespace() const {
        return persistDocNamespace;
    }

    void setPersistDocNamespace(bool value) {
        persistDocNamespace = value;
    }

    uint64_t getPeriodicSyncBytes() const {
        return periodicSyncBytes;
    }

    void setPeriodicSyncBytes(uint64_t bytes) {
        periodicSyncBytes = bytes;
    }

    // Following specific to RocksDB.
    // TODO: Move into a RocksDBKVStoreConfig subclass.

    /*
     * Return the RocksDB Database level options.
     */
    const std::string& getRocksDBOptions() {
        return rocksDBOptions;
    }

    /*
     * Return the RocksDB Column Family level options.
     */
    const std::string& getRocksDBCFOptions() {
        return rocksDBCFOptions;
    }

    /*
     * Return the RocksDB Block Based Table options.
     */
    const std::string& getRocksDbBBTOptions() {
        return rocksDbBBTOptions;
    }

    /// Return the RocksDB low priority background thread count.
    size_t getRocksDbLowPriBackgroundThreads() const {
        return rocksDbLowPriBackgroundThreads;
    }

    /// Return the RocksDB high priority background thread count.
    size_t getRocksDbHighPriBackgroundThreads() const {
        return rocksDbHighPriBackgroundThreads;
    }

    /*
     * Return the RocksDB Statistics 'stats_level'.
     */
    const std::string& getRocksdbStatsLevel() {
        return rocksdbStatsLevel;
    }

    /*
     * Return the RocksDB Block Cache size.
     */
    size_t getRocksdbBlockCacheSize() {
        return rocksdbBlockCacheSize;
    }

    // Return the RocksDB memory budget for Level-style compaction
    // optimization for the 'default' column family
    size_t getRocksdbDefaultCfMemBudget() {
        return rocksdbDefaultCfMemBudget;
    }

    // Return the RocksDB memory budget for Level-style compaction
    // optimization for the 'seqno' column family
    size_t getRocksdbSeqnoCfMemBudget() {
        return rocksdbSeqnoCfMemBudget;
    }

private:
    class ConfigChangeListener;

    uint16_t maxVBuckets;
    uint16_t maxShards;
    std::string dbname;
    std::string backend;
    uint16_t shardId;
    Logger* logger;
    bool buffered;
    bool persistDocNamespace;

    /**
     * If non-zero, tell storage layer to issue a sync() operation after every
     * N bytes written.
     */
    uint64_t periodicSyncBytes;

    // RocksDB Database level options. Semicolon-separated `<option>=<value>`
    // pairs.
    std::string rocksDBOptions;
    // RocksDB Column Family level options. Semicolon-separated
    // `<option>=<value>` pairs.
    std::string rocksDBCFOptions;
    // RocksDB Block Based Table options. Semicolon-separated
    // `<option>=<value>` pairs.
    std::string rocksDbBBTOptions;

    /// RocksDB low priority background thread count.
    size_t rocksDbLowPriBackgroundThreads = 0;

    /// RocksDB high priority background thread count.
    size_t rocksDbHighPriBackgroundThreads = 0;

    // RocksDB Statistics 'stats_level'. Possible values:
    // {'', 'kAll', 'kExceptTimeForMutex', 'kExceptDetailedTimers'}
    std::string rocksdbStatsLevel;

    // RocksDB Block Cache size
    size_t rocksdbBlockCacheSize = 0;

    // RocksDB memtable memory budget for Level-style compaction optimization
    // for the 'default' column family
    size_t rocksdbDefaultCfMemBudget = 0;

    // RocksDB memtable memory budget for Level-style compaction optimization
    // for the 'seqno' column family
    size_t rocksdbSeqnoCfMemBudget = 0;
};
