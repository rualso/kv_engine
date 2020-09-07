/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "config.h"

#ifdef _MSC_VER
#define PATH_MAX MAX_PATH
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <phosphor/phosphor.h>
#include <platform/cb_malloc.h>
#include <platform/checked_snprintf.h>
#include <string>
#include <utility>
#include <vector>
#include <cJSON.h>
#include <platform/dirutils.h>

#include "common.h"
#include "couch-kvstore/couch-kvstore.h"
#include "ep_types.h"
#include "kvstore_config.h"
#include "statwriter.h"
#include "vbucket.h"
#include "vbucket_bgfetch_item.h"

#include <JSON_checker.h>
#include <kvstore.h>
#include <platform/compress.h>

extern "C" {
    static int recordDbDumpC(Db *db, DocInfo *docinfo, void *ctx)
    {
        return CouchKVStore::recordDbDump(db, docinfo, ctx);
    }
}

extern "C" {
    static int getMultiCbC(Db *db, DocInfo *docinfo, void *ctx)
    {
        return CouchKVStore::getMultiCb(db, docinfo, ctx);
    }
}

struct kvstats_ctx {
    kvstats_ctx(bool persistDocNamespace)
        : persistDocNamespace(persistDocNamespace) {
    }
    /// A map of key to bool. If true, the key exists in the VB datafile
    std::unordered_map<StoredDocKey, bool> keyStats;
    /// Collections: When enabled this means persisted keys have namespaces
    bool persistDocNamespace;
};

static std::string getStrError(Db *db) {
    const size_t max_msg_len = 256;
    char msg[max_msg_len];
    couchstore_last_os_error(db, msg, max_msg_len);
    std::string errorStr(msg);
    return errorStr;
}

/**
 * Determine the datatype for a blob. It is _highly_ unlikely that
 * this method is being called, as it would have to be for an item
 * which is read off the disk _before_ we started to write the
 * datatype to disk (we did that in a 3.x server).
 *
 * @param doc The document to check
 * @return JSON or RAW bytes
 */
static protocol_binary_datatype_t determine_datatype(sized_buf doc) {
    if (checkUTF8JSON(reinterpret_cast<uint8_t*>(doc.buf), doc.size)) {
        return PROTOCOL_BINARY_DATATYPE_JSON;
    } else {
        return PROTOCOL_BINARY_RAW_BYTES;
    }
}

static bool endWithCompact(const std::string &filename) {
    size_t pos = filename.find(".compact");
    if (pos == std::string::npos ||
                        (filename.size() - sizeof(".compact")) != pos) {
        return false;
    }
    return true;
}

static void discoverDbFiles(const std::string &dir,
                            std::vector<std::string> &v) {
    auto files = cb::io::findFilesContaining(dir, ".couch");
    std::vector<std::string>::iterator ii;
    for (ii = files.begin(); ii != files.end(); ++ii) {
        if (!endWithCompact(*ii)) {
            v.push_back(*ii);
        }
    }
}

static int getMutationStatus(couchstore_error_t errCode) {
    switch (errCode) {
    case COUCHSTORE_SUCCESS:
        return MUTATION_SUCCESS;
    case COUCHSTORE_ERROR_NO_HEADER:
    case COUCHSTORE_ERROR_NO_SUCH_FILE:
    case COUCHSTORE_ERROR_DOC_NOT_FOUND:
        // this return causes ep engine to drop the failed flush
        // of an item since it does not know about the itme any longer
        return DOC_NOT_FOUND;
    default:
        // this return causes ep engine to keep requeuing the failed
        // flush of an item
        return MUTATION_FAILED;
    }
}

static bool allDigit(std::string &input) {
    size_t numchar = input.length();
    for(size_t i = 0; i < numchar; ++i) {
        if (!isdigit(input[i])) {
            return false;
        }
    }
    return true;
}

static std::string couchkvstore_strerrno(Db *db, couchstore_error_t err) {
    return (err == COUCHSTORE_ERROR_OPEN_FILE ||
            err == COUCHSTORE_ERROR_READ ||
            err == COUCHSTORE_ERROR_WRITE ||
            err == COUCHSTORE_ERROR_FILE_CLOSE) ? getStrError(db) : "none";
}

static DocKey makeDocKey(const sized_buf buf, bool restoreNamespace) {
    if (restoreNamespace) {
        return DocKey(reinterpret_cast<const uint8_t*>(&buf.buf[1]),
                      buf.size - 1,
                      DocNamespace(buf.buf[0]));
    } else {
        return DocKey(reinterpret_cast<const uint8_t*>(buf.buf),
                      buf.size,
                      DocNamespace::DefaultCollection);
    }
}

struct GetMultiCbCtx {
    GetMultiCbCtx(CouchKVStore &c, uint16_t v, vb_bgfetch_queue_t &f) :
        cks(c), vbId(v), fetches(f) {}

    CouchKVStore &cks;
    uint16_t vbId;
    vb_bgfetch_queue_t &fetches;
};

struct StatResponseCtx {
public:
    StatResponseCtx(std::map<std::pair<uint16_t, uint16_t>, vbucket_state> &sm,
                    uint16_t vb) : statMap(sm), vbId(vb) {
        /* EMPTY */
    }

    std::map<std::pair<uint16_t, uint16_t>, vbucket_state> &statMap;
    uint16_t vbId;
};

struct AllKeysCtx {
    AllKeysCtx(std::shared_ptr<Callback<const DocKey&>> callback, uint32_t cnt)
        : cb(callback), count(cnt) { }

    std::shared_ptr<Callback<const DocKey&>> cb;
    uint32_t count;
};

couchstore_content_meta_flags CouchRequest::getContentMeta(const Item& it) {
    couchstore_content_meta_flags rval;

    if (mcbp::datatype::is_json(it.getDataType())) {
        rval = COUCH_DOC_IS_JSON;
    } else {
        rval = COUCH_DOC_NON_JSON_MODE;
    }

    if (it.getNBytes() > 0 && !mcbp::datatype::is_snappy(it.getDataType())) {
        //Compress only if a value exists and is not already compressed
        rval |= COUCH_DOC_IS_COMPRESSED;
    }

    return rval;
}

CouchRequest::CouchRequest(const Item& it,
                           uint64_t rev,
                           MutationRequestCallback& cb,
                           bool del,
                           bool persistDocNamespace)
    : IORequest(it.getVBucketId(), cb, del, it.getKey()),
      value(it.getValue()),
      fileRevNum(rev) {
    // Collections: TODO: Temporary switch to ensure upgrades don't break.
    if (persistDocNamespace) {
        dbDoc.id = {const_cast<char*>(reinterpret_cast<const char*>(
                            key.getDocNameSpacedData())),
                    it.getKey().getDocNameSpacedSize()};
    } else {
        dbDoc.id = {const_cast<char*>(key.c_str()), it.getKey().size()};
    }

    if (it.getNBytes()) {
        dbDoc.data.buf = const_cast<char *>(value->getData());
        dbDoc.data.size = it.getNBytes();
    } else {
        dbDoc.data.buf = NULL;
        dbDoc.data.size = 0;
    }
    meta.setCas(it.getCas());
    meta.setFlags(it.getFlags());
    meta.setExptime(it.getExptime());
    meta.setDataType(it.getDataType());

    dbDocInfo.db_seq = it.getBySeqno();

    // Now allocate space to hold the meta and get it ready for storage
    dbDocInfo.rev_meta.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    dbDocInfo.rev_meta.buf = meta.prepareAndGetForPersistence();

    dbDocInfo.rev_seq = it.getRevSeqno();
    dbDocInfo.size = dbDoc.data.size;

    if (del) {
        dbDocInfo.deleted =  1;
    } else {
        dbDocInfo.deleted = 0;
    }
    dbDocInfo.id = dbDoc.id;
    dbDocInfo.content_meta = getContentMeta(it);
}

CouchKVStore::CouchKVStore(KVStoreConfig& config)
    : CouchKVStore(config, *couchstore_get_default_file_ops()) {
}

CouchKVStore::CouchKVStore(KVStoreConfig& config,
                           FileOpsInterface& ops,
                           bool readOnly,
                           std::shared_ptr<RevisionMap> dbFileRevMap)
    : KVStore(config, readOnly),
      dbname(config.getDBName()),
      dbFileRevMap(dbFileRevMap),
      intransaction(false),
      scanCounter(0),
      logger(config.getLogger()),
      base_ops(ops) {
    createDataDir(dbname);
    statCollectingFileOps = getCouchstoreStatsOps(st.fsStats, base_ops);
    statCollectingFileOpsCompaction = getCouchstoreStatsOps(
        st.fsStatsCompaction, base_ops);

    // init db file map with default revision number, 1
    numDbFiles = configuration.getMaxVBuckets();

    // pre-allocate lookup maps (vectors) given we have a relatively
    // small, fixed number of vBuckets.
    cachedDocCount.assign(numDbFiles, Couchbase::RelaxedAtomic<size_t>(0));
    cachedDeleteCount.assign(numDbFiles, Couchbase::RelaxedAtomic<size_t>(-1));
    cachedFileSize.assign(numDbFiles, Couchbase::RelaxedAtomic<uint64_t>(0));
    cachedSpaceUsed.assign(numDbFiles, Couchbase::RelaxedAtomic<uint64_t>(0));
    cachedVBStates.resize(numDbFiles);

    initialize();
}

CouchKVStore::CouchKVStore(KVStoreConfig& config, FileOpsInterface& ops)
    : CouchKVStore(config,
                   ops,
                   false /*readonly*/,
                   std::make_shared<RevisionMap>(config.getMaxVBuckets())) {
}

/**
 * Make a read-only CouchKVStore from this object
 */
std::unique_ptr<CouchKVStore> CouchKVStore::makeReadOnlyStore() {
    // Not using make_unique due to the private constructor we're calling
    return std::unique_ptr<CouchKVStore>(
            new CouchKVStore(configuration, dbFileRevMap));
}

CouchKVStore::CouchKVStore(KVStoreConfig& config,
                           std::shared_ptr<RevisionMap> dbFileRevMap)
    : CouchKVStore(config,
                   *couchstore_get_default_file_ops(),
                   true /*readonly*/,
                   dbFileRevMap) {
}

void CouchKVStore::initialize() {
    std::vector<uint16_t> vbids;
    std::vector<std::string> files;
    discoverDbFiles(dbname, files);
    populateFileNameMap(files, &vbids);

    couchstore_error_t errorCode;

    std::vector<uint16_t>::iterator itr = vbids.begin();
    for (; itr != vbids.end(); ++itr) {
        uint16_t id = *itr;
        DbHolder db(*this);
        errorCode = openDB(id, db, COUCHSTORE_OPEN_FLAG_RDONLY);
        if (errorCode == COUCHSTORE_SUCCESS) {
            readVBState(db, id);
            /* update stat */
            ++st.numLoadedVb;
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::initialize: openDB"
                       " error:%s, name:%s/%" PRIu16 ".couch.%" PRIu64,
                       couchstore_strerror(errorCode),
                       dbname.c_str(),
                       id,
                       db.getFileRev());
            cachedVBStates[id] = NULL;
        }

        if (!isReadOnly()) {
            removeCompactFile(dbname, id);
        }
    }
}

CouchKVStore::~CouchKVStore() {
    close();
}

void CouchKVStore::reset(uint16_t vbucketId) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::reset: Not valid on a read-only "
                        "object.");
    }

    vbucket_state* state = getVBucketState(vbucketId);
    if (state) {
        state->reset();

        cachedDocCount[vbucketId] = 0;
        cachedDeleteCount[vbucketId] = 0;
        cachedFileSize[vbucketId] = 0;
        cachedSpaceUsed[vbucketId] = 0;

        // Unlink the current revision and then increment it to ensure any
        // pending delete doesn't delete us. Note that the expectation is that
        // some higher level per VB lock is required to prevent data-races here.
        // KVBucket::vb_mutexes is used in this case.
        unlinkCouchFile(vbucketId, (*dbFileRevMap)[vbucketId]);
        incrementRevision(vbucketId);

        setVBucketState(
                vbucketId, *state, VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT);
    } else {
        throw std::invalid_argument("CouchKVStore::reset: No entry in cached "
                        "states for vbucket " + std::to_string(vbucketId));
    }
}

void CouchKVStore::set(const Item& itm,
                       Callback<TransactionContext, mutation_result>& cb) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::set: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("CouchKVStore::set: intransaction must be "
                        "true to perform a set operation.");
    }

    bool deleteItem = false;
    MutationRequestCallback requestcb;
    uint64_t fileRev = (*dbFileRevMap)[itm.getVBucketId()];

    // each req will be de-allocated after commit
    requestcb.setCb = &cb;
    CouchRequest* req =
            new CouchRequest(itm,
                             fileRev,
                             requestcb,
                             deleteItem,
                             configuration.shouldPersistDocNamespace());
    pendingReqsQ.push_back(req);
}

GetValue CouchKVStore::get(const StoredDocKey& key,
                           uint16_t vb,
                           bool fetchDelete) {
    DbHolder db(*this);
    couchstore_error_t errCode = openDB(vb, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        ++st.numGetFailure;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::get: openDB error:%s, vb:%" PRIu16,
                   couchstore_strerror(errCode), vb);
        return GetValue(nullptr, couchErr2EngineErr(errCode));
    }

    GetValue gv = getWithHeader(db, key, vb, GetMetaOnly::No, fetchDelete);
    return gv;
}

GetValue CouchKVStore::getWithHeader(void* dbHandle,
                                     const StoredDocKey& key,
                                     uint16_t vb,
                                     GetMetaOnly getMetaOnly,
                                     bool fetchDelete) {
    Db *db = (Db *)dbHandle;
    auto start = ProcessClock::now();
    DocInfo *docInfo = NULL;
    sized_buf id;
    GetValue rv;

    if (configuration.shouldPersistDocNamespace()) {
        id = {const_cast<char*>(reinterpret_cast<const char*>(
                      key.getDocNameSpacedData())),
              key.getDocNameSpacedSize()};

    } else {
        id = {const_cast<char*>(reinterpret_cast<const char*>(key.data())),
              key.size()};
    }

    couchstore_error_t errCode = couchstore_docinfo_by_id(db, (uint8_t *)id.buf,
                                                          id.size, &docInfo);
    if (errCode != COUCHSTORE_SUCCESS) {
        if (getMetaOnly == GetMetaOnly::No) {
            // log error only if this is non-xdcr case
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getWithHeader: couchstore_docinfo_by_id "
                       "error:%s [%s], vb:%" PRIu16,
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db, errCode).c_str(), vb);
        }
    } else {
        if (docInfo == nullptr) {
            throw std::logic_error("CouchKVStore::getWithHeader: "
                    "couchstore_docinfo_by_id returned success but docInfo "
                    "is NULL");
        }
        errCode = fetchDoc(db, docInfo, rv, vb, getMetaOnly);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getWithHeader: fetchDoc error:%s [%s],"
                       " vb:%" PRIu16 ", deleted:%s",
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db, errCode).c_str(), vb,
                       docInfo->deleted ? "yes" : "no");
        }

        // record stats
        st.readTimeHisto.add(
                std::chrono::duration_cast<std::chrono::microseconds>(
                        ProcessClock::now() - start));
        if (errCode == COUCHSTORE_SUCCESS) {
            st.readSizeHisto.add(key.size() + rv.item->getNBytes());
        }
    }

    if(errCode != COUCHSTORE_SUCCESS) {
        ++st.numGetFailure;
    }

    couchstore_free_docinfo(docInfo);
    rv.setStatus(couchErr2EngineErr(errCode));
    return rv;
}

void CouchKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t &itms) {
    if (itms.empty()) {
        return;
    }
    int numItems = itms.size();

    DbHolder db(*this);
    couchstore_error_t errCode = openDB(vb, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getMulti: openDB error:%s, "
                   "vb:%" PRIu16 ", numDocs:%d",
                   couchstore_strerror(errCode), vb, numItems);
        st.numGetFailure += numItems;
        for (auto& item : itms) {
            item.second.value.setStatus(ENGINE_NOT_MY_VBUCKET);
        }
        return;
    }

    size_t idx = 0;
    std::vector<sized_buf> ids(itms.size());
    for (auto& item : itms) {
        if (configuration.shouldPersistDocNamespace()) {
            ids[idx] = {const_cast<char*>(reinterpret_cast<const char*>(
                                item.first.getDocNameSpacedData())),
                        item.first.getDocNameSpacedSize()};
        } else {
            ids[idx] = {const_cast<char*>(reinterpret_cast<const char*>(
                                item.first.data())),
                        item.first.size()};
        }

        ++idx;
    }

    GetMultiCbCtx ctx(*this, vb, itms);

    errCode = couchstore_docinfos_by_id(
            db, ids.data(), itms.size(), 0, getMultiCbC, &ctx);
    if (errCode != COUCHSTORE_SUCCESS) {
        st.numGetFailure += numItems;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getMulti: "
                   "couchstore_docinfos_by_id error %s [%s], vb:%" PRIu16,
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(db, errCode).c_str(),
                   vb);
        for (auto& item : itms) {
            item.second.value.setStatus(couchErr2EngineErr(errCode));
        }
    }

    // If available, record how many reads() we did for this getMulti;
    // and the average reads per document.
    auto* stats = couchstore_get_db_filestats(db);
    if (stats != nullptr) {
        const auto readCount = stats->getReadCount();
        st.getMultiFsReadCount += readCount;
        st.getMultiFsReadHisto.add(readCount);
        st.getMultiFsReadPerDocHisto.add(readCount / itms.size());
    }
}

void CouchKVStore::del(const Item& itm, Callback<TransactionContext, int>& cb) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::del: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("CouchKVStore::del: intransaction must be "
                        "true to perform a delete operation.");
    }

    uint64_t fileRev = (*dbFileRevMap)[itm.getVBucketId()];
    MutationRequestCallback requestcb;
    requestcb.delCb = &cb;
    CouchRequest* req =
            new CouchRequest(itm,
                             fileRev,
                             requestcb,
                             true,
                             configuration.shouldPersistDocNamespace());
    pendingReqsQ.push_back(req);
}

void CouchKVStore::delVBucket(uint16_t vbucket, uint64_t fileRev) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::delVBucket: Not valid on a "
                        "read-only object.");
    }

    unlinkCouchFile(vbucket, fileRev);
}

std::vector<vbucket_state *> CouchKVStore::listPersistedVbuckets() {
    std::vector<vbucket_state*> result;
    for (const auto& vb : cachedVBStates) {
        result.emplace_back(vb.get());
    }
    return result;
}

void CouchKVStore::getPersistedStats(std::map<std::string,
                                     std::string> &stats) {
    std::vector<char> buffer;
    std::string fname = dbname + "/stats.json";
    if (access(fname.c_str(), R_OK) == -1) {
        return ;
    }

    std::ifstream session_stats;
    session_stats.exceptions (session_stats.failbit | session_stats.badbit);
    try {
        session_stats.open(fname.c_str(), std::ios::binary);
        session_stats.seekg(0, std::ios::end);
        int flen = session_stats.tellg();
        if (flen < 0) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::getPersistedStats:"
                       " Error in session stats ifstream!!!");
            session_stats.close();
            return;
        }
        session_stats.seekg(0, std::ios::beg);
        buffer.resize(flen + 1);
        session_stats.read(buffer.data(), flen);
        session_stats.close();
        buffer[flen] = '\0';

        cJSON *json_obj = cJSON_Parse(buffer.data());
        if (!json_obj) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::getPersistedStats:"
                       " Failed to parse the session stats json doc!!!");
            return;
        }

        int json_arr_size = cJSON_GetArraySize(json_obj);
        for (int i = 0; i < json_arr_size; ++i) {
            cJSON *obj = cJSON_GetArrayItem(json_obj, i);
            if (obj) {
                stats[obj->string] = obj->valuestring ? obj->valuestring : "";
            }
        }
        cJSON_Delete(json_obj);

    } catch (const std::ifstream::failure &e) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getPersistedStats: Failed to load the engine "
                   "session stats due to IO exception \"%s\"", e.what());
    } catch (...) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getPersistedStats: Failed to load the engine "
                   "session stats due to IO exception");
    }
}

static std::string getDBFileName(const std::string &dbname,
                                 uint16_t vbid,
                                 uint64_t rev) {
    return dbname + "/" + std::to_string(vbid) + ".couch." +
           std::to_string(rev);
}

static int edit_docinfo_hook(DocInfo **info, const sized_buf *item) {
    // Examine the metadata of the doc
    auto documentMetaData = MetaDataFactory::createMetaData((*info)->rev_meta);
    // Allocate latest metadata
    std::unique_ptr<MetaData> metadata;
    if (documentMetaData->getVersionInitialisedFrom() == MetaData::Version::V0) {
        // Metadata doesn't have flex_meta_code/datatype. Provision space for
        // these paramenters.

        // If the document is compressed we need to inflate it to
        // determine if it is json or not.
        cb::compression::Buffer inflated;
        cb::const_char_buffer data {item->buf, item->size};
        if (((*info)->content_meta | COUCH_DOC_IS_COMPRESSED) ==
                (*info)->content_meta) {
            if (!cb::compression::inflate(cb::compression::Algorithm::Snappy,
                                          data, inflated)) {
                throw std::runtime_error(
                    "edit_docinfo_hook: failed to inflate document with seqno: " +
                    std::to_string((*info)->db_seq) + " revno: " +
                    std::to_string((*info)->rev_seq));
            }
            data = inflated;
        }

        protocol_binary_datatype_t datatype = PROTOCOL_BINARY_RAW_BYTES;
        if (checkUTF8JSON(reinterpret_cast<const uint8_t*>(data.data()),
                          data.size())) {
            datatype = PROTOCOL_BINARY_DATATYPE_JSON;
        }

        // Now create a blank latest metadata.
        metadata = MetaDataFactory::createMetaData();
        // Copy the metadata this will pull across available V0 fields.
        *metadata = *documentMetaData;

        // Setup flex code and datatype
        metadata->setFlexCode();
        metadata->setDataType(datatype);
    } else {
        // The metadata in the document is V1 and needs no changes.
        return 0;
    }

    // the docInfo pointer includes the DocInfo and the data it points to.
    // this must be a pointer which cb_free() can deallocate
    char* buffer = static_cast<char*>(cb_calloc(1, sizeof(DocInfo) +
                             (*info)->id.size +
                             MetaData::getMetaDataSize(MetaData::Version::V1)));


    DocInfo* docInfo = reinterpret_cast<DocInfo*>(buffer);

    // Deep-copy the incoming DocInfo, then we'll fix the pointers/buffer data
    *docInfo = **info;

    // Correct the id buffer
    docInfo->id.buf = buffer + sizeof(DocInfo);
    std::memcpy(docInfo->id.buf, (*info)->id.buf, docInfo->id.size);

    // Correct the rev_meta pointer and fill it in.
    docInfo->rev_meta.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    docInfo->rev_meta.buf = buffer + sizeof(DocInfo) + docInfo->id.size;
    metadata->copyToBuf(docInfo->rev_meta);

    // Free the orginal
    couchstore_free_docinfo(*info);

    // Return the newly allocated docinfo with corrected metadata
    *info = docInfo;

    return 1;
}

/**
 * Notify the expiry callback that a document has expired
 *
 * @param info     document information for the expired item
 * @param metadata metadata of the document
 * @param item     buffer containing data and size
 * @param ctx      context for compaction
 * @param currtime current time
 */
static int notify_expired_item(DocInfo& info,
                               MetaData& metadata,
                               sized_buf item,
                               compaction_ctx& ctx,
                               time_t currtime) {
    cb::char_buffer data;
    cb::compression::Buffer inflated;

    if (mcbp::datatype::is_xattr(metadata.getDataType())) {
        if (item.buf == nullptr) {
            // We need to pass on the entire document to the callback
            return COUCHSTORE_COMPACT_NEED_BODY;
        }

        // A document on disk is marked snappy in two ways.
        // 1) info.content_meta if the document was compressed by couchstore
        // 2) datatype snappy if the document was already compressed when stored
        if ((info.content_meta & COUCH_DOC_IS_COMPRESSED) ||
            mcbp::datatype::is_snappy(metadata.getDataType())) {
            using namespace cb::compression;

            if (!inflate(Algorithm::Snappy, {item.buf, item.size}, inflated)) {
                LOG(EXTENSION_LOG_WARNING,
                    "time_purge_hook: failed to inflate document with seqno %" PRIu64 ""
                    "revno: %" PRIu64, info.db_seq, info.rev_seq);
                return COUCHSTORE_ERROR_CORRUPT;
            }
            // Now remove snappy bit
            metadata.setDataType(metadata.getDataType() &
                                 ~PROTOCOL_BINARY_DATATYPE_SNAPPY);
            data = inflated;
        }
    }

    // Collections: TODO: Restore to stored namespace
    Item it(makeDocKey(info.id, ctx.config->shouldPersistDocNamespace()),
            metadata.getFlags(),
            metadata.getExptime(),
            data.buf,
            data.len,
            metadata.getDataType(),
            metadata.getCas(),
            info.db_seq,
            ctx.db_file_id,
            info.rev_seq);

    it.setRevSeqno(info.rev_seq);
    ctx.expiryCallback->callback(it, currtime);

    return COUCHSTORE_SUCCESS;
}

static int time_purge_hook(Db* d, DocInfo* info, sized_buf item, void* ctx_p) {
    compaction_ctx* ctx = static_cast<compaction_ctx*>(ctx_p);
    const uint16_t vbid = ctx->db_file_id;

    if (info == nullptr) {
        // Compaction finished
        return couchstore_set_purge_seq(d, ctx->max_purged_seq[vbid]);
    }

    DbInfo infoDb;
    auto err = couchstore_db_info(d, &infoDb);
    if (err != COUCHSTORE_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "time_purge_hook: couchstore_db_info() failed: %s",
            couchstore_strerror(err));
        return err;
    }

    uint64_t max_purge_seq = 0;
    auto it = ctx->max_purged_seq.find(vbid);

    if (it == ctx->max_purged_seq.end()) {
        ctx->max_purged_seq[vbid] = 0;
    } else {
        max_purge_seq = it->second;
    }

    if (info->rev_meta.size >= MetaData::getMetaDataSize(MetaData::Version::V0)) {
        auto metadata = MetaDataFactory::createMetaData(info->rev_meta);
        uint32_t exptime = metadata->getExptime();

        // Is the collections eraser installed?
        if (ctx->collectionsEraser &&
            ctx->collectionsEraser(
                    makeDocKey(info->id,
                               ctx->config->shouldPersistDocNamespace()),
                    int64_t(info->db_seq),
                    info->deleted,
                    ctx->eraserContext)) {
            ctx->stats.collectionsItemsPurged++;
            return COUCHSTORE_COMPACT_DROP_ITEM;
        }

        if (info->deleted) {
            if (info->db_seq != infoDb.last_sequence) {
                if (ctx->drop_deletes) { // all deleted items must be dropped ...
                    if (max_purge_seq < info->db_seq) {
                        ctx->max_purged_seq[vbid] = info->db_seq; // track max_purged_seq
                    }
                    ctx->stats.tombstonesPurged++;
                    return COUCHSTORE_COMPACT_DROP_ITEM;      // ...unconditionally
                }
                if (exptime < ctx->purge_before_ts &&
                        (!ctx->purge_before_seq ||
                         info->db_seq <= ctx->purge_before_seq)) {
                    if (max_purge_seq < info->db_seq) {
                        ctx->max_purged_seq[vbid] = info->db_seq;
                    }
                    ctx->stats.tombstonesPurged++;
                    return COUCHSTORE_COMPACT_DROP_ITEM;
                }
            }
        } else {
            time_t currtime = ep_real_time();
            if (exptime && exptime < currtime) {
                int ret;
                try {
                    ret = notify_expired_item(*info, *metadata, item,
                                             *ctx, currtime);
                } catch (const std::bad_alloc&) {
                    LOG(EXTENSION_LOG_WARNING,
                        "time_purge_hook: memory allocation failed");
                    return COUCHSTORE_ERROR_ALLOC_FAIL;
                }

                if (ret != COUCHSTORE_SUCCESS) {
                    return ret;
                }
            }
        }
    }

    if (ctx->bloomFilterCallback) {
        bool deleted = info->deleted;
        // Collections: TODO: Permanently restore to stored namespace
        DocKey key = makeDocKey(
                info->id, ctx->config->shouldPersistDocNamespace());

        try {
            ctx->bloomFilterCallback->callback(
                    ctx->db_file_id, key, deleted);
        } catch (std::runtime_error& re) {
            LOG(EXTENSION_LOG_WARNING,
                "time_purge_hook: exception occurred when invoking the "
                "bloomfilter callback on vbucket:%" PRIu16
                " - Details: %s", vbid, re.what());
        }
    }

    return COUCHSTORE_COMPACT_KEEP_ITEM;
}

bool CouchKVStore::compactDB(compaction_ctx *hook_ctx) {
    bool result = false;

    try {
        result = compactDBInternal(hook_ctx, edit_docinfo_hook);
    } catch(std::logic_error& le) {
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::compactDB: exception while performing "
            "compaction for vbucket:%" PRIu16
            " - Details: %s", hook_ctx->db_file_id, le.what());
    }
    if (!result) {
        ++st.numCompactionFailure;
    }
    return result;
}

FileInfo CouchKVStore::toFileInfo(const DbInfo& info) {
    return FileInfo{
            info.doc_count, info.deleted_count, info.file_size, info.purge_seq};
}

bool CouchKVStore::compactDBInternal(compaction_ctx* hook_ctx,
                                     couchstore_docinfo_hook docinfo_hook) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::compactDB: Cannot perform "
                        "on a read-only instance.");
    }
    couchstore_compact_hook       hook = time_purge_hook;
    couchstore_docinfo_hook dhook = docinfo_hook;
    FileOpsInterface         *def_iops = statCollectingFileOpsCompaction.get();
    DbHolder compactdb(*this);
    DbHolder targetDb(*this);
    couchstore_error_t         errCode = COUCHSTORE_SUCCESS;
    ProcessClock::time_point     start = ProcessClock::now();
    std::string                 dbfile;
    std::string           compact_file;
    std::string               new_file;
    DbInfo                        info;
    uint16_t                      vbid = hook_ctx->db_file_id;
    hook_ctx->config = &configuration;

    TRACE_EVENT1("CouchKVStore", "compactDB", "vbid", vbid);

    // Open the source VBucket database file ...
    errCode = openDB(
            vbid, compactdb, (uint64_t)COUCHSTORE_OPEN_FLAG_RDONLY, def_iops);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB openDB error:%s, vb:%" PRIu16
                   ", fileRev:%" PRIu64,
                   couchstore_strerror(errCode),
                   vbid,
                   compactdb.getFileRev());
        return false;
    }

    uint64_t new_rev = compactdb.getFileRev() + 1;

    // Build the temporary vbucket.compact file name
    dbfile = getDBFileName(dbname, vbid, compactdb.getFileRev());
    compact_file = dbfile + ".compact";

    couchstore_open_flags flags(COUCHSTORE_COMPACT_FLAG_UPGRADE_DB);

    couchstore_db_info(compactdb, &info);
    hook_ctx->stats.pre = toFileInfo(info);

    /**
     * This flag disables IO buffering in couchstore which means
     * file operations will trigger syscalls immediately. This has
     * a detrimental impact on performance and is only intended
     * for testing.
     */
    if(!configuration.getBuffered()) {
        flags |= COUCHSTORE_OPEN_FLAG_UNBUFFERED;
    }

    // Should automatic fsync() be configured for compaction?
    const auto periodicSyncBytes = configuration.getPeriodicSyncBytes();
    if (periodicSyncBytes != 0) {
        flags |= couchstore_encode_periodic_sync_flags(periodicSyncBytes);
    }

    // Perform COMPACTION of vbucket.couch.rev into vbucket.couch.rev.compact
    errCode = couchstore_compact_db_ex(compactdb,
                                       compact_file.c_str(),
                                       flags,
                                       hook,
                                       dhook,
                                       hook_ctx,
                                       def_iops);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB:couchstore_compact_db_ex "
                   "error:%s [%s], name:%s",
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(compactdb, errCode).c_str(),
                   dbfile.c_str());
        return false;
    }

    // Close the source Database File once compaction is done
    compactdb.close();

    // Rename the .compact file to one with the next revision number
    new_file = getDBFileName(dbname, vbid, new_rev);
    if (rename(compact_file.c_str(), new_file.c_str()) != 0) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB: rename error:%s, old:%s, new:%s",
                   cb_strerror().c_str(), compact_file.c_str(), new_file.c_str());

        removeCompactFile(compact_file);
        return false;
    }

    // Open the newly compacted VBucket database file ...
    errCode = openSpecificDB(
            vbid, new_rev, targetDb, (uint64_t)COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB: openDB#2 error:%s, file:%s, "
                   "fileRev:%" PRIu64,
                   couchstore_strerror(errCode),
                   new_file.c_str(),
                   targetDb.getFileRev());
        if (remove(new_file.c_str()) != 0) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::compactDB: remove error:%s, path:%s",
                       cb_strerror().c_str(), new_file.c_str());
        }
        return false;
    }

    // Update the global VBucket file map so all operations use the new file
    updateDbFileMap(vbid, new_rev);

    logger.log(EXTENSION_LOG_INFO,
               "INFO: created new couch db file, name:%s rev:%" PRIu64,
               new_file.c_str(), new_rev);

    couchstore_db_info(targetDb.getDb(), &info);
    hook_ctx->stats.post = toFileInfo(info);

    cachedFileSize[vbid] = info.file_size;
    cachedSpaceUsed[vbid] = info.space_used;

    // also update cached state with dbinfo
    vbucket_state* state = getVBucketState(vbid);
    if (state) {
        state->highSeqno = info.last_sequence;
        state->purgeSeqno = info.purge_seq;
        cachedDeleteCount[vbid] = info.deleted_count;
        cachedDocCount[vbid] = info.doc_count;
    }

    // Removing the stale couch file
    unlinkCouchFile(vbid, compactdb.getFileRev());

    st.compactHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - start));

    return true;
}

vbucket_state * CouchKVStore::getVBucketState(uint16_t vbucketId) {
    return cachedVBStates[vbucketId].get();
}

bool CouchKVStore::setVBucketState(uint16_t vbucketId,
                                   const vbucket_state& vbstate,
                                   VBStatePersist options) {
    std::map<uint16_t, uint64_t>::iterator mapItr;
    couchstore_error_t errorCode;

    if (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
            options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
        DbHolder db(*this);
        errorCode =
                openDB(vbucketId, db, (uint64_t)COUCHSTORE_OPEN_FLAG_CREATE);
        if (errorCode != COUCHSTORE_SUCCESS) {
            ++st.numVbSetFailure;
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::setVBucketState: openDB error:%s, "
                       "vb:%" PRIu16 ", fileRev:%" PRIu64,
                       couchstore_strerror(errorCode),
                       vbucketId,
                       db.getFileRev());
            return false;
        }

        errorCode = saveVBState(db, vbstate);
        if (errorCode != COUCHSTORE_SUCCESS) {
            ++st.numVbSetFailure;
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore:setVBucketState: saveVBState error:%s, "
                       "vb:%" PRIu16 ", fileRev:%" PRIu64,
                       couchstore_strerror(errorCode),
                       vbucketId,
                       db.getFileRev());
            return false;
        }

        if (options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
            errorCode = couchstore_commit(db);
            if (errorCode != COUCHSTORE_SUCCESS) {
                ++st.numVbSetFailure;
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore:setVBucketState: couchstore_commit "
                           "error:%s [%s], vb:%" PRIu16 ", rev:%" PRIu64,
                           couchstore_strerror(errorCode),
                           couchkvstore_strerrno(db, errorCode).c_str(),
                           vbucketId,
                           db.getFileRev());
                return false;
            }
        }

        DbInfo info;
        errorCode = couchstore_db_info(db, &info);
        if (errorCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::setVBucketState: couchstore_db_info "
                       "error:%s, vb:%" PRIu16, couchstore_strerror(errorCode),
                       vbucketId);
        } else {
            cachedSpaceUsed[vbucketId] = info.space_used;
            cachedFileSize[vbucketId] = info.file_size;
        }
    } else {
        throw std::invalid_argument("CouchKVStore::setVBucketState: invalid vb state "
                        "persist option specified for vbucket id:" +
                        std::to_string(vbucketId));
    }

    return true;
}

bool CouchKVStore::snapshotVBucket(uint16_t vbucketId,
                                   const vbucket_state &vbstate,
                                   VBStatePersist options) {
    if (isReadOnly()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::snapshotVBucket: cannot be performed on a "
                   "read-only KVStore instance");
        return false;
    }

    auto start = ProcessClock::now();

    if (updateCachedVBState(vbucketId, vbstate) &&
         (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
          options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        vbucket_state* vbs = getVBucketState(vbucketId);
        if (!setVBucketState(vbucketId, *vbs, options)) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::snapshotVBucket: setVBucketState failed "
                       "state:%s, vb:%" PRIu16,
                       VBucket::toString(vbstate.state), vbucketId);
            return false;
        }
    }

    LOG(EXTENSION_LOG_DEBUG,
        "CouchKVStore::snapshotVBucket: Snapshotted vbucket:%" PRIu16 " state:%s",
        vbucketId,
        vbstate.toJSON().c_str());

    st.snapshotHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - start));

    return true;
}

StorageProperties CouchKVStore::getStorageProperties() {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::Yes,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::No);
    return rv;
}

bool CouchKVStore::commit(const Item* collectionsManifest) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::commit: Not valid on a read-only "
                        "object.");
    }

    if (intransaction) {
        if (commit2couchstore(collectionsManifest)) {
            intransaction = false;
            transactionCtx.reset();
        }
    }

    return !intransaction;
}

bool CouchKVStore::getStat(const char* name, size_t& value)  {
    if (strcmp("failure_compaction", name) == 0) {
        value = st.numCompactionFailure.load();
        return true;
    } else if (strcmp("failure_get", name) == 0) {
        value = st.numGetFailure.load();
        return true;
    } else if (strcmp("io_total_read_bytes", name) == 0) {
        value = st.fsStats.totalBytesRead.load() +
                st.fsStatsCompaction.totalBytesRead.load();
        return true;
    } else if (strcmp("io_total_write_bytes", name) == 0) {
        value = st.fsStats.totalBytesWritten.load() +
                st.fsStatsCompaction.totalBytesWritten.load();
        return true;
    } else if (strcmp("io_compaction_read_bytes", name) == 0) {
        value = st.fsStatsCompaction.totalBytesRead;
        return true;
    } else if (strcmp("io_compaction_write_bytes", name) == 0) {
        value = st.fsStatsCompaction.totalBytesWritten;
        return true;
    } else if (strcmp("io_bg_fetch_read_count", name) == 0) {
        value = st.getMultiFsReadCount;
        return true;
    }

    return false;
}

void CouchKVStore::pendingTasks() {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::pendingTasks: Not valid on a "
                        "read-only object.");
    }

    if (!pendingFileDeletions.empty()) {
        std::queue<std::string> queue;
        pendingFileDeletions.getAll(queue);

        while (!queue.empty()) {
            std::string filename_str = queue.front();
            if (remove(filename_str.c_str()) == -1) {
                logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::pendingTasks: "
                           "remove error:%d, file%s", errno,
                           filename_str.c_str());
                if (errno != ENOENT) {
                    pendingFileDeletions.push(filename_str);
                }
            }
            queue.pop();
        }
    }
}

ScanContext* CouchKVStore::initScanContext(
        std::shared_ptr<StatusCallback<GetValue>> cb,
        std::shared_ptr<StatusCallback<CacheLookup>> cl,
        uint16_t vbid,
        uint64_t startSeqno,
        DocumentFilter options,
        ValueFilter valOptions) {
    DbHolder db(*this);
    couchstore_error_t errorCode =
            openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errorCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::initScanContext: openDB error:%s, "
                   "name:%s/%" PRIu16 ".couch.%" PRIu64,
                   couchstore_strerror(errorCode),
                   dbname.c_str(),
                   vbid,
                   db.getFileRev());
        return NULL;
    }

    DbInfo info;
    errorCode = couchstore_db_info(db, &info);
    if (errorCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::initScanContext: couchstore_db_info error:%s",
                   couchstore_strerror(errorCode));
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::initScanContext: Failed to read DB info for "
            "backfill. vb:%" PRIu16 " rev:%" PRIu64 " error: %s",
            vbid,
            db.getFileRev(),
            couchstore_strerror(errorCode));
        return NULL;
    }

    uint64_t count = 0;
    errorCode = couchstore_changes_count(
            db, startSeqno, std::numeric_limits<uint64_t>::max(), &count);
    if (errorCode != COUCHSTORE_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::initScanContext:Failed to obtain changes "
            "count for vb:%" PRIu16 " rev:%" PRIu64 " start_seqno:%" PRIu64
            " error: %s",
            vbid,
            db.getFileRev(),
            startSeqno,
            couchstore_strerror(errorCode));
        return NULL;
    }

    size_t scanId = scanCounter++;

    {
        LockHolder lh(scanLock);
        scans[scanId] = db.releaseDb();
    }

    ScanContext* sctx = new ScanContext(cb,
                                        cl,
                                        vbid,
                                        scanId,
                                        startSeqno,
                                        info.last_sequence,
                                        info.purge_seq,
                                        options,
                                        valOptions,
                                        count,
                                        configuration);
    sctx->logger = &logger;
    return sctx;
}

static couchstore_docinfos_options getDocFilter(const DocumentFilter& filter) {
    switch (filter) {
    case DocumentFilter::ALL_ITEMS:
        return COUCHSTORE_NO_OPTIONS;
    case DocumentFilter::NO_DELETES:
        return COUCHSTORE_NO_DELETES;
    }

    std::string err("getDocFilter: Illegal document filter!" +
                    std::to_string(static_cast<int>(filter)));
    throw std::runtime_error(err);
}

scan_error_t CouchKVStore::scan(ScanContext* ctx) {
    if (!ctx) {
        return scan_failed;
    }

    if (ctx->lastReadSeqno == ctx->maxSeqno) {
        return scan_success;
    }

    TRACE_EVENT_START2("CouchKVStore",
                       "scan",
                       "vbid",
                       ctx->vbid,
                       "startSeqno",
                       ctx->startSeqno);

    Db* db;
    {
        LockHolder lh(scanLock);
        auto itr = scans.find(ctx->scanId);
        if (itr == scans.end()) {
            return scan_failed;
        }

        db = itr->second;
    }

    uint64_t start = ctx->startSeqno;
    if (ctx->lastReadSeqno != 0) {
        start = ctx->lastReadSeqno + 1;
    }

    couchstore_error_t errorCode;
    errorCode = couchstore_changes_since(db,
                                         start,
                                         getDocFilter(ctx->docFilter),
                                         recordDbDumpC,
                                         static_cast<void*>(ctx));

    TRACE_EVENT_END1(
            "CouchKVStore", "scan", "lastReadSeqno", ctx->lastReadSeqno);

    if (errorCode != COUCHSTORE_SUCCESS) {
        if (errorCode == COUCHSTORE_ERROR_CANCEL) {
            return scan_again;
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::scan couchstore_changes_since "
                       "error:%s [%s]", couchstore_strerror(errorCode),
                       couchkvstore_strerrno(db, errorCode).c_str());
            return scan_failed;
        }
    }
    return scan_success;
}

void CouchKVStore::destroyScanContext(ScanContext* ctx) {
    if (!ctx) {
        return;
    }

    LockHolder lh(scanLock);
    auto itr = scans.find(ctx->scanId);
    if (itr != scans.end()) {
        closeDatabaseHandle(itr->second);
        scans.erase(itr);
    }
    delete ctx;
}

DbInfo CouchKVStore::getDbInfo(uint16_t vbid) {
    DbHolder db(*this);
    couchstore_error_t errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        DbInfo info;
        errCode = couchstore_db_info(db, &info);
        if (errCode == COUCHSTORE_SUCCESS) {
            return info;
        } else {
            throw std::runtime_error(
                    "CouchKVStore::getDbInfo: failed "
                    "to read database info for vBucket " +
                    std::to_string(vbid) + " revision " +
                    std::to_string(db.getFileRev()) +
                    " - couchstore returned error: " +
                    couchstore_strerror(errCode));
        }
    } else {
        // open failed - map couchstore error code to exception.
        std::errc ec;
        switch (errCode) {
            case COUCHSTORE_ERROR_OPEN_FILE:
                ec = std::errc::no_such_file_or_directory; break;
            default:
                ec = std::errc::io_error; break;
        }
        throw std::system_error(
                std::make_error_code(ec),
                "CouchKVStore::getDbInfo: failed to open database file for "
                "vBucket = " +
                        std::to_string(vbid) + " rev = " +
                        std::to_string(db.getFileRev()) + " with error:" +
                        couchstore_strerror(errCode));
    }
}

void CouchKVStore::close() {
    intransaction = false;
}

uint64_t CouchKVStore::checkNewRevNum(std::string &dbFileName, bool newFile) {
    uint64_t newrev = 0;
    std::string nameKey;

    if (!newFile) {
        // extract out the file revision number first
        size_t secondDot = dbFileName.rfind(".");
        nameKey = dbFileName.substr(0, secondDot);
    } else {
        nameKey = dbFileName;
    }
    nameKey.append(".");
    const auto files = cb::io::findFilesWithPrefix(nameKey);
    std::vector<std::string>::const_iterator itor;
    // found file(s) whoes name has the same key name pair with different
    // revision number
    for (itor = files.begin(); itor != files.end(); ++itor) {
        const std::string &filename = *itor;
        if (endWithCompact(filename)) {
            continue;
        }

        size_t secondDot = filename.rfind(".");
        char *ptr = NULL;
        uint64_t revnum = strtoull(filename.substr(secondDot + 1).c_str(), &ptr, 10);
        if (newrev < revnum) {
            newrev = revnum;
            dbFileName = filename;
        }
    }
    return newrev;
}

void CouchKVStore::updateDbFileMap(uint16_t vbucketId, uint64_t newFileRev) {
    if (vbucketId >= numDbFiles) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::updateDbFileMap: Cannot update db file map "
                   "for an invalid vbucket, vb:%" PRIu16", rev:%" PRIu64,
                   vbucketId, newFileRev);
        return;
    }
    // MB-27963: obtain write access whilst we update the file map openDB also
    // obtains this mutex to ensure the fileRev it obtains doesn't become stale
    // by the time it hits sys_open.
    std::lock_guard<cb::WriterLock> lg(openDbMutex);

    (*dbFileRevMap)[vbucketId] = newFileRev;
}

couchstore_error_t CouchKVStore::openDB(uint16_t vbucketId,
                                        DbHolder& db,
                                        couchstore_open_flags options,
                                        FileOpsInterface* ops) {
    // MB-27963: obtain read access whilst we open the file, updateDbFileMap
    // serialises on this mutex so we can be sure the fileRev we read should
    // still be a valid file once we hit sys_open
    std::lock_guard<cb::ReaderLock> lg(openDbMutex);
    uint64_t fileRev = (*dbFileRevMap)[vbucketId];
    return openSpecificDB(vbucketId, fileRev, db, options, ops);
}

couchstore_error_t CouchKVStore::openSpecificDB(uint16_t vbucketId,
                                                uint64_t fileRev,
                                                DbHolder& db,
                                                couchstore_open_flags options,
                                                FileOpsInterface* ops) {
    std::string dbFileName = getDBFileName(dbname, vbucketId, fileRev);
    db.setFileRev(fileRev); // save the rev so the caller can log it

    if(ops == nullptr) {
        ops = statCollectingFileOps.get();
    }

    couchstore_error_t errorCode = COUCHSTORE_SUCCESS;

    /**
     * This flag disables IO buffering in couchstore which means
     * file operations will trigger syscalls immediately. This has
     * a detrimental impact on performance and is only intended
     * for testing.
     */
    if(!configuration.getBuffered()) {
        options |= COUCHSTORE_OPEN_FLAG_UNBUFFERED;
    }

    errorCode = couchstore_open_db_ex(
            dbFileName.c_str(), options, ops, db.getDbAddress());

    /* update command statistics */
    st.numOpen++;
    if (errorCode) {
        st.numOpenFailure++;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::openDB: error:%s [%s],"
                   " name:%s, option:%" PRIX64 ", fileRev:%" PRIu64,
                   couchstore_strerror(errorCode),
                   cb_strerror().c_str(),
                   dbFileName.c_str(),
                   options,
                   fileRev);

        if (errorCode == COUCHSTORE_ERROR_NO_SUCH_FILE) {
            auto dotPos = dbFileName.find_last_of(".");
            if (dotPos != std::string::npos) {
                dbFileName = dbFileName.substr(0, dotPos);
            }
            auto files = cb::io::findFilesWithPrefix(dbFileName);
            logger.log(
                    EXTENSION_LOG_WARNING,
                    "CouchKVStore::openDB: No such file, found:%zd alternative "
                    "files for %s",
                    files.size(),
                    dbFileName.c_str());
            for (const auto& f : files) {
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore::openDB: Found %s",
                           f.c_str());
            }
        }
    }

    return errorCode;
}

void CouchKVStore::populateFileNameMap(std::vector<std::string> &filenames,
                                       std::vector<uint16_t> *vbids) {
    std::vector<std::string>::iterator fileItr;

    for (fileItr = filenames.begin(); fileItr != filenames.end(); ++fileItr) {
        const std::string &filename = *fileItr;
        size_t secondDot = filename.rfind(".");
        std::string nameKey = filename.substr(0, secondDot);
        size_t firstDot = nameKey.rfind(".");
#ifdef _MSC_VER
        size_t firstSlash = nameKey.rfind("\\");
#else
        size_t firstSlash = nameKey.rfind("/");
#endif

        std::string revNumStr = filename.substr(secondDot + 1);
        char *ptr = NULL;
        uint64_t revNum = strtoull(revNumStr.c_str(), &ptr, 10);

        std::string vbIdStr = nameKey.substr(firstSlash + 1,
                                            (firstDot - firstSlash) - 1);
        if (allDigit(vbIdStr)) {
            int vbId = atoi(vbIdStr.c_str());
            if (vbids) {
                vbids->push_back(static_cast<uint16_t>(vbId));
            }
            uint64_t old_rev_num = (*dbFileRevMap)[vbId];
            if (old_rev_num == revNum) {
                continue;
            } else if (old_rev_num < revNum) { // stale revision found
                (*dbFileRevMap)[vbId] = revNum;
            } else { // stale file found (revision id has rolled over)
                old_rev_num = revNum;
            }
            std::stringstream old_file;
            old_file << dbname << "/" << vbId << ".couch." << old_rev_num;
            if (access(old_file.str().c_str(), F_OK) == 0) {
                if (!isReadOnly()) {
                    if (remove(old_file.str().c_str()) == 0) {
                        logger.log(EXTENSION_LOG_INFO,
                                  "CouchKVStore::populateFileNameMap: Removed "
                                  "stale file:%s", old_file.str().c_str());
                    } else {
                        logger.log(EXTENSION_LOG_WARNING,
                                   "CouchKVStore::populateFileNameMap: remove "
                                   "error:%s, file:%s", cb_strerror().c_str(),
                                   old_file.str().c_str());
                    }
                } else {
                    logger.log(EXTENSION_LOG_WARNING,
                               "CouchKVStore::populateFileNameMap: A read-only "
                               "instance of the underlying store "
                               "was not allowed to delete a stale file:%s",
                               old_file.str().c_str());
                }
            }
        } else {
            // skip non-vbucket database file, master.couch etc
            logger.log(EXTENSION_LOG_DEBUG,
                       "CouchKVStore::populateFileNameMap: Non-vbucket database file, %s, skip adding "
                       "to CouchKVStore dbFileMap", filename.c_str());
        }
    }
}

couchstore_error_t CouchKVStore::fetchDoc(Db* db,
                                          DocInfo* docinfo,
                                          GetValue& docValue,
                                          uint16_t vbId,
                                          GetMetaOnly metaOnly) {
    couchstore_error_t errCode = COUCHSTORE_SUCCESS;
    std::unique_ptr<MetaData> metadata;
    try {
        metadata = MetaDataFactory::createMetaData(docinfo->rev_meta);
    } catch (std::logic_error&) {
        return COUCHSTORE_ERROR_DB_NO_LONGER_VALID;
    }

    if (metaOnly == GetMetaOnly::Yes) {
        // Collections: TODO: Permanently restore to stored namespace
        auto it = std::make_unique<Item>(
                makeDocKey(docinfo->id,
                           configuration.shouldPersistDocNamespace()),
                metadata->getFlags(),
                metadata->getExptime(),
                nullptr,
                docinfo->size,
                metadata->getDataType(),
                metadata->getCas(),
                docinfo->db_seq,
                vbId);

        it->setRevSeqno(docinfo->rev_seq);

        if (docinfo->deleted) {
            it->setDeleted();
        }
        docValue = GetValue(std::move(it));
        // update ep-engine IO stats
        ++st.io_bg_fetch_docs_read;
        st.io_bgfetch_doc_bytes += (docinfo->id.size + docinfo->rev_meta.size);
    } else {
        Doc *doc = nullptr;
        size_t valuelen = 0;
        void* valuePtr = nullptr;
        protocol_binary_datatype_t datatype = PROTOCOL_BINARY_RAW_BYTES;
        errCode = couchstore_open_doc_with_docinfo(db, docinfo, &doc,
                                                   DECOMPRESS_DOC_BODIES);
        if (errCode == COUCHSTORE_SUCCESS) {
            if (doc == nullptr) {
                throw std::logic_error("CouchKVStore::fetchDoc: doc is NULL");
            }

            if (doc->id.size > UINT16_MAX) {
                throw std::logic_error("CouchKVStore::fetchDoc: "
                            "doc->id.size (which is" +
                            std::to_string(doc->id.size) + ") is greater than "
                            + std::to_string(UINT16_MAX));
            }

            valuelen = doc->data.size;
            valuePtr = doc->data.buf;

            if (metadata->getVersionInitialisedFrom() == MetaData::Version::V0) {
                // This is a super old version of a couchstore file.
                // Try to determine if the document is JSON or raw bytes
                datatype = determine_datatype(doc->data);
            } else {
                datatype = metadata->getDataType();
            }
        } else if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND && docinfo->deleted) {
            datatype = metadata->getDataType();
        } else {
            return errCode;
        }

        try {
            // Collections: TODO: Restore to stored namespace
            auto it = std::make_unique<Item>(
                    makeDocKey(docinfo->id,
                               configuration.shouldPersistDocNamespace()),
                    metadata->getFlags(),
                    metadata->getExptime(),
                    valuePtr,
                    valuelen,
                    datatype,
                    metadata->getCas(),
                    docinfo->db_seq,
                    vbId,
                    docinfo->rev_seq);

             if (docinfo->deleted) {
                 it->setDeleted();
             }
             docValue = GetValue(std::move(it));
        } catch (std::bad_alloc&) {
            couchstore_free_document(doc);
            return COUCHSTORE_ERROR_ALLOC_FAIL;
        }

        // update ep-engine IO stats
        ++st.io_bg_fetch_docs_read;
        st.io_bgfetch_doc_bytes +=
                (docinfo->id.size + docinfo->rev_meta.size + valuelen);

        couchstore_free_document(doc);
    }
    return COUCHSTORE_SUCCESS;
}

int CouchKVStore::recordDbDump(Db *db, DocInfo *docinfo, void *ctx) {

    ScanContext* sctx = static_cast<ScanContext*>(ctx);
    auto* cb = sctx->callback.get();
    auto* cl = sctx->lookup.get();

    Doc *doc = nullptr;
    sized_buf value{nullptr, 0};
    uint64_t byseqno = docinfo->db_seq;
    uint16_t vbucketId = sctx->vbid;

    sized_buf key = docinfo->id;
    if (key.size > UINT16_MAX) {
        throw std::invalid_argument("CouchKVStore::recordDbDump: "
                        "docinfo->id.size (which is " + std::to_string(key.size) +
                        ") is greater than " + std::to_string(UINT16_MAX));
    }

    // Collections: TODO: Permanently restore to stored namespace
    DocKey docKey = makeDocKey(
            docinfo->id, sctx->config.shouldPersistDocNamespace());

    if (sctx->collectionsContext.manageSeparator(docKey)) {
        sctx->lastReadSeqno = byseqno;
        return COUCHSTORE_SUCCESS;
    }

    CacheLookup lookup(docKey,
                       byseqno,
                       vbucketId,
                       sctx->collectionsContext.getSeparator());
    cl->callback(lookup);
    if (cl->getStatus() == ENGINE_KEY_EEXISTS) {
        sctx->lastReadSeqno = byseqno;
        return COUCHSTORE_SUCCESS;
    } else if (cl->getStatus() == ENGINE_ENOMEM) {
        return COUCHSTORE_ERROR_CANCEL;
    }

    auto metadata = MetaDataFactory::createMetaData(docinfo->rev_meta);

    if (sctx->valFilter != ValueFilter::KEYS_ONLY) {
        couchstore_open_options openOptions = 0;

        /**
         * If the stored document has V0 metdata (no datatype)
         * or no special request is made to retrieve compressed documents
         * as is, then DECOMPRESS the document and update datatype
         */
        if (docinfo->rev_meta.size == metadata->getMetaDataSize(MetaData::Version::V0) ||
            sctx->valFilter == ValueFilter::VALUES_DECOMPRESSED) {
            openOptions = DECOMPRESS_DOC_BODIES;
        }

        auto errCode = couchstore_open_doc_with_docinfo(db, docinfo, &doc,
                                                        openOptions);

        if (errCode == COUCHSTORE_SUCCESS) {
            value = doc->data;
            if (doc->data.size) {
                if ((openOptions & DECOMPRESS_DOC_BODIES) == 0) {
                    // We always store the document bodies compressed on disk,
                    // but now the client _wanted_ to fetch the document
                    // in a compressed mode.
                    // We've never stored the "compressed" flag on disk
                    // (as we don't keep items compressed in memory).
                    // Update the datatype flag for this item to
                    // reflect that it is compressed so that the
                    // receiver of the object may notice (Note:
                    // this is currently _ONLY_ happening via DCP
                     auto datatype = metadata->getDataType();
                     metadata->setDataType(datatype | PROTOCOL_BINARY_DATATYPE_SNAPPY);
                } else if (metadata->getVersionInitialisedFrom() == MetaData::Version::V0) {
                    // This is a super old version of a couchstore file.
                    // Try to determine if the document is JSON or raw bytes
                    metadata->setDataType(determine_datatype(doc->data));
                }
            } else {
                // No data, it cannot have a datatype!
                metadata->setDataType(PROTOCOL_BINARY_RAW_BYTES);
            }
        } else if (errCode != COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            sctx->logger->log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::recordDbDump: "
                              "couchstore_open_doc_with_docinfo error:%s [%s], "
                              "vb:%" PRIu16 ", seqno:%" PRIu64,
                              couchstore_strerror(errCode),
                              couchkvstore_strerrno(db, errCode).c_str(),
                              vbucketId, docinfo->rev_seq);
            return COUCHSTORE_SUCCESS;
        }
    }

    // Collections: TODO: Permanently restore to stored namespace
    auto it = std::make_unique<Item>(
            DocKey(makeDocKey(key, sctx->config.shouldPersistDocNamespace())),
            metadata->getFlags(),
            metadata->getExptime(),
            value.buf,
            value.size,
            metadata->getDataType(),
            metadata->getCas(),
            docinfo->db_seq, // return seq number being persisted on disk
            vbucketId,
            docinfo->rev_seq);

    if (docinfo->deleted) {
        it->setDeleted();
    }

    bool onlyKeys = (sctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;
    GetValue rv(std::move(it), ENGINE_SUCCESS, -1, onlyKeys);
    cb->callback(rv);

    couchstore_free_document(doc);

    if (cb->getStatus() == ENGINE_ENOMEM) {
        return COUCHSTORE_ERROR_CANCEL;
    }

    sctx->lastReadSeqno = byseqno;
    return COUCHSTORE_SUCCESS;
}

bool CouchKVStore::commit2couchstore(const Item* collectionsManifest) {
    bool success = true;

    size_t pendingCommitCnt = pendingReqsQ.size();
    if (pendingCommitCnt == 0 && !collectionsManifest) {
        return success;
    }

    // Use the vbucket of the first item or the manifest item
    uint16_t vbucket2flush = pendingCommitCnt
                                     ? pendingReqsQ[0]->getVBucketId()
                                     : collectionsManifest->getVBucketId();

    TRACE_EVENT2("CouchKVStore",
                 "commit2couchstore",
                 "vbid",
                 vbucket2flush,
                 "pendingCommitCnt",
                 pendingCommitCnt);

    // When an item and a manifest are present, vbucket2flush is read from the
    // item. Check it matches the manifest
    if (pendingCommitCnt && collectionsManifest &&
        vbucket2flush != collectionsManifest->getVBucketId()) {
        throw std::logic_error(
                "CouchKVStore::commit2couchstore: manifest/item vbucket "
                "mismatch vbucket2flush:" +
                std::to_string(vbucket2flush) + " manifest vb:" +
                std::to_string(collectionsManifest->getVBucketId()));
    }

    std::vector<Doc*> docs(pendingCommitCnt);
    std::vector<DocInfo*> docinfos(pendingCommitCnt);

    for (size_t i = 0; i < pendingCommitCnt; ++i) {
        CouchRequest *req = pendingReqsQ[i];
        docs[i] = (Doc *)req->getDbDoc();
        docinfos[i] = req->getDbDocInfo();
        if (vbucket2flush != req->getVBucketId()) {
            throw std::logic_error(
                    "CouchKVStore::commit2couchstore: "
                    "mismatch between vbucket2flush (which is "
                    + std::to_string(vbucket2flush) + ") and pendingReqsQ["
                    + std::to_string(i) + "] (which is "
                    + std::to_string(req->getVBucketId()) + ")");
        }
    }

    // The docinfo callback needs to know if the DocNamespace feature is on
    kvstats_ctx kvctx(configuration.shouldPersistDocNamespace());
    // flush all
    couchstore_error_t errCode =
            saveDocs(vbucket2flush, docs, docinfos, kvctx, collectionsManifest);

    if (errCode) {
        success = false;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::commit2couchstore: saveDocs error:%s, "
                   "vb:%" PRIu16,
                   couchstore_strerror(errCode),
                   vbucket2flush);
    }

    commitCallback(pendingReqsQ, kvctx, errCode);

    // clean up
    for (size_t i = 0; i < pendingCommitCnt; ++i) {
        delete pendingReqsQ[i];
    }
    pendingReqsQ.clear();
    return success;
}

static int readDocInfos(Db *db, DocInfo *docinfo, void *ctx) {
    if (ctx == nullptr) {
        throw std::invalid_argument("readDocInfos: ctx must be non-NULL");
    }
    if (!docinfo) {
        throw std::invalid_argument("readDocInfos: docInfo must be non-NULL");
    }
    kvstats_ctx* cbCtx = static_cast<kvstats_ctx*>(ctx);
    if(docinfo) {
        // An item exists in the VB DB file.
        if (!docinfo->deleted) {
            // Collections: TODO: Permanently restore to stored namespace
            auto itr = cbCtx->keyStats.find(
                    makeDocKey(docinfo->id, cbCtx->persistDocNamespace));
            if (itr != cbCtx->keyStats.end()) {
                itr->second = true;
            }
        }
    }
    return 0;
}

couchstore_error_t CouchKVStore::saveDocs(uint16_t vbid,
                                          const std::vector<Doc*>& docs,
                                          std::vector<DocInfo*>& docinfos,
                                          kvstats_ctx& kvctx,
                                          const Item* collectionsManifest) {
    couchstore_error_t errCode;
    DbInfo info;
    DbHolder db(*this);
    errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_CREATE);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveDocs: openDB error:%s, vb:%" PRIu16
                   ", rev:%" PRIu64 ", numdocs:%" PRIu64,
                   couchstore_strerror(errCode),
                   vbid,
                   db.getFileRev(),
                   uint64_t(docs.size()));
        return errCode;
    } else {
        vbucket_state* state = getVBucketState(vbid);
        if (state == nullptr) {
            throw std::logic_error(
                    "CouchKVStore::saveDocs: cachedVBStates[" +
                    std::to_string(vbid) + "] is NULL");
        }

        uint64_t maxDBSeqno = 0;

        // Only do a couchstore_save_documents if there are docs
        if (docs.size() > 0) {
            std::vector<sized_buf> ids(docs.size());
            for (size_t idx = 0; idx < docs.size(); idx++) {
                ids[idx] = docinfos[idx]->id;
                maxDBSeqno = std::max(maxDBSeqno, docinfos[idx]->db_seq);
                DocKey key = makeDocKey(
                        ids[idx], configuration.shouldPersistDocNamespace());
                kvctx.keyStats[key] = false;
            }
            errCode = couchstore_docinfos_by_id(db,
                                                ids.data(),
                                                (unsigned)ids.size(),
                                                0,
                                                readDocInfos,
                                                &kvctx);
            if (errCode != COUCHSTORE_SUCCESS) {
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore::saveDocs: couchstore_docinfos_by_id "
                           "error:%s [%s], vb:%" PRIu16 ", numdocs:%" PRIu64,
                           couchstore_strerror(errCode),
                           couchkvstore_strerrno(db, errCode).c_str(),
                           vbid,
                           uint64_t(docs.size()));
                return errCode;
            }

            auto cs_begin = ProcessClock::now();
            uint64_t flags = COMPRESS_DOC_BODIES | COUCHSTORE_SEQUENCE_AS_IS;
            errCode = couchstore_save_documents(db,
                                                docs.data(),
                                                docinfos.data(),
                                                (unsigned)docs.size(),
                                                flags);
            st.saveDocsHisto.add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                            ProcessClock::now() - cs_begin));
            if (errCode != COUCHSTORE_SUCCESS) {
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore::saveDocs: couchstore_save_documents "
                           "error:%s [%s], vb:%" PRIu16 ", numdocs:%" PRIu64,
                           couchstore_strerror(errCode),
                           couchkvstore_strerrno(db, errCode).c_str(),
                           vbid,
                           uint64_t(docs.size()));
                return errCode;
            }
        }

        errCode = saveVBState(db, *state);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::saveDocs: saveVBState error:%s [%s]",
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db, errCode).c_str());
            return errCode;
        }

        if (collectionsManifest) {
            saveCollectionsManifest(*db, *collectionsManifest);
        }

        auto cs_begin = ProcessClock::now();
        errCode = couchstore_commit_nosync(db);
        st.commitHisto.add(
                std::chrono::duration_cast<std::chrono::microseconds>(
                        ProcessClock::now() - cs_begin));
        if (errCode) {
            logger.log(
                    EXTENSION_LOG_WARNING,
                    "CouchKVStore::saveDocs: couchstore_commit_nosync error:%s [%s]",
                    couchstore_strerror(errCode),
                    couchkvstore_strerrno(db, errCode).c_str());
            return errCode;
        }

        st.batchSize.add(docs.size());

        // retrieve storage system stats for file fragmentation computation
        errCode = couchstore_db_info(db, &info);
        if (errCode) {
            logger.log(
                    EXTENSION_LOG_WARNING,
                    "CouchKVStore::saveDocs: couchstore_db_info error:%s [%s]",
                    couchstore_strerror(errCode),
                    couchkvstore_strerrno(db, errCode).c_str());
            return errCode;
        }
        cachedSpaceUsed[vbid] = info.space_used;
        cachedFileSize[vbid] = info.file_size;
        cachedDeleteCount[vbid] = info.deleted_count;
        cachedDocCount[vbid] = info.doc_count;

        // Check seqno if we wrote documents
        if (docs.size() > 0 && maxDBSeqno != info.last_sequence) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::saveDocs: Seqno in db header (%" PRIu64 ")"
                       " is not matched with what was persisted (%" PRIu64 ")"
                       " for vb:%" PRIu16,
                       info.last_sequence, maxDBSeqno, vbid);
        }
        state->highSeqno = info.last_sequence;
    }

    /* update stat */
    if(errCode == COUCHSTORE_SUCCESS) {
        st.docsCommitted = docs.size();
    }

    return errCode;
}

void CouchKVStore::commitCallback(std::vector<CouchRequest *> &committedReqs,
                                  kvstats_ctx &kvctx,
                                  couchstore_error_t errCode) {
    size_t commitSize = committedReqs.size();

    for (size_t index = 0; index < commitSize; index++) {
        size_t dataSize = committedReqs[index]->getNBytes();
        size_t keySize = committedReqs[index]->getKey().size();
        /* update ep stats */
        ++st.io_num_write;
        st.io_write_bytes += (keySize + dataSize);

        if (committedReqs[index]->isDelete()) {
            int rv = getMutationStatus(errCode);
            if (rv != -1) {
                const auto& key = committedReqs[index]->getKey();
                if (kvctx.keyStats[key]) {
                    rv = 1; // Deletion is for an existing item on DB file.
                } else {
                    rv = 0; // Deletion is for a non-existing item on DB file.
                }
            }
            if (errCode) {
                ++st.numDelFailure;
            } else {
                st.delTimeHisto.add(committedReqs[index]->getDelta());
            }
            committedReqs[index]->getDelCallback()->callback(*transactionCtx,
                                                             rv);
        } else {
            int rv = getMutationStatus(errCode);
            const auto& key = committedReqs[index]->getKey();
            bool insertion = !kvctx.keyStats[key];
            if (errCode) {
                ++st.numSetFailure;
            } else {
                st.writeTimeHisto.add(committedReqs[index]->getDelta());
                st.writeSizeHisto.add(dataSize + keySize);
            }
            mutation_result p(rv, insertion);
            committedReqs[index]->getSetCallback()->callback(*transactionCtx,
                                                             p);
        }
    }
}

ENGINE_ERROR_CODE CouchKVStore::readVBState(Db *db, uint16_t vbId) {
    sized_buf id;
    LocalDoc *ldoc = NULL;
    couchstore_error_t errCode = COUCHSTORE_SUCCESS;
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    int64_t highSeqno = 0;
    std::string failovers;
    uint64_t purgeSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t hlcCasEpochSeqno = HlcCasSeqnoUninitialised;
    bool mightContainXattrs = false;

    DbInfo info;
    errCode = couchstore_db_info(db, &info);
    if (errCode == COUCHSTORE_SUCCESS) {
        highSeqno = info.last_sequence;
        purgeSeqno = info.purge_seq;
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::readVBState: couchstore_db_info error:%s"
                   ", vb:%" PRIu16, couchstore_strerror(errCode), vbId);
        return couchErr2EngineErr(errCode);
    }

    id.buf = (char *)"_local/vbstate";
    id.size = sizeof("_local/vbstate") - 1;
    errCode = couchstore_open_local_document(db, (void *)id.buf,
                                             id.size, &ldoc);
    if (errCode != COUCHSTORE_SUCCESS) {
        if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            logger.log(EXTENSION_LOG_NOTICE,
                       "CouchKVStore::readVBState: '_local/vbstate' not found "
                       "for vb:%d", vbId);
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::readVBState: couchstore_open_local_document"
                       " error:%s, vb:%" PRIu16, couchstore_strerror(errCode),
                       vbId);
        }
    } else {
        const std::string statjson(ldoc->json.buf, ldoc->json.size);
        cJSON *jsonObj = cJSON_Parse(statjson.c_str());
        if (!jsonObj) {
            couchstore_free_local_document(ldoc);
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::readVBState: Failed to "
                       "parse the vbstat json doc for vb:%" PRIu16 ", json:%s",
                       vbId , statjson.c_str());
            return couchErr2EngineErr(errCode);
        }

        const std::string vb_state = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "state"));
        const std::string checkpoint_id = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj,"checkpoint_id"));
        const std::string max_deleted_seqno = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));
        const std::string snapStart = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "snap_start"));
        const std::string snapEnd = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "snap_end"));
        const std::string maxCasValue = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "max_cas"));
        const std::string hlcCasEpoch =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "hlc_epoch"));
        mightContainXattrs = getJSONObjBool(
                cJSON_GetObjectItem(jsonObj, "might_contain_xattrs"));

        cJSON *failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0
                || max_deleted_seqno.compare("") == 0) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::readVBState: State"
                       " JSON doc for vb:%" PRIu16 " is in the wrong format:%s, "
                       "vb state:%s, checkpoint id:%s and max deleted seqno:%s",
                       vbId, statjson.c_str(), vb_state.c_str(),
                       checkpoint_id.c_str(), max_deleted_seqno.c_str());
        } else {
            state = VBucket::fromString(vb_state.c_str());
            parseUint64(max_deleted_seqno.c_str(), &maxDeletedSeqno);
            parseUint64(checkpoint_id.c_str(), &checkpointId);

            if (snapStart.compare("") == 0) {
                lastSnapStart = highSeqno;
            } else {
                parseUint64(snapStart.c_str(), &lastSnapStart);
            }

            if (snapEnd.compare("") == 0) {
                lastSnapEnd = highSeqno;
            } else {
                parseUint64(snapEnd.c_str(), &lastSnapEnd);
            }

            if (maxCasValue.compare("") != 0) {
                parseUint64(maxCasValue.c_str(), &maxCas);

                // MB-17517: If the maxCas on disk was invalid then don't use it -
                // instead rebuild from the items we load from disk (i.e. as per
                // an upgrade from an earlier version).
                if (maxCas == static_cast<uint64_t>(-1)) {
                    logger.log(EXTENSION_LOG_WARNING,
                               "CouchKVStore::readVBState: Invalid max_cas "
                               "(0x%" PRIx64 ") read from '%s' for vb:%" PRIu16
                               ". Resetting max_cas to zero.",
                               maxCas, id.buf, vbId);
                    maxCas = 0;
                }
            }

            if (!hlcCasEpoch.empty()) {
                parseInt64(hlcCasEpoch.c_str(), &hlcCasEpochSeqno);
            }

            if (failover_json) {
                failovers = to_string(failover_json, false);
            }
        }
        cJSON_Delete(jsonObj);
        couchstore_free_local_document(ldoc);
    }

    cachedVBStates[vbId] = std::make_unique<vbucket_state>(state,
                                                           checkpointId,
                                                           maxDeletedSeqno,
                                                           highSeqno,
                                                           purgeSeqno,
                                                           lastSnapStart,
                                                           lastSnapEnd,
                                                           maxCas,
                                                           hlcCasEpochSeqno,
                                                           mightContainXattrs,
                                                           failovers);

    return couchErr2EngineErr(errCode);
}

couchstore_error_t CouchKVStore::saveVBState(Db *db,
                                             const vbucket_state &vbState) {
    std::stringstream jsonState;

    jsonState << "{\"state\": \"" << VBucket::toString(vbState.state) << "\""
              << ",\"checkpoint_id\": \"" << vbState.checkpointId << "\""
              << ",\"max_deleted_seqno\": \"" << vbState.maxDeletedSeqno << "\"";
    if (!vbState.failovers.empty()) {
        jsonState << ",\"failover_table\": " << vbState.failovers;
    }
    jsonState << ",\"snap_start\": \"" << vbState.lastSnapStart << "\""
              << ",\"snap_end\": \"" << vbState.lastSnapEnd << "\""
              << ",\"max_cas\": \"" << vbState.maxCas << "\""
              << ",\"hlc_epoch\": \"" << vbState.hlcCasEpochSeqno << "\"";

    if (vbState.mightContainXattrs) {
        jsonState << ",\"might_contain_xattrs\": true";
    } else {
        jsonState << ",\"might_contain_xattrs\": false";
    }

    jsonState << "}";

    LocalDoc lDoc;
    lDoc.id.buf = (char *)"_local/vbstate";
    lDoc.id.size = sizeof("_local/vbstate") - 1;
    std::string state = jsonState.str();
    lDoc.json.buf = (char *)state.c_str();
    lDoc.json.size = state.size();
    lDoc.deleted = 0;

    couchstore_error_t errCode = couchstore_save_local_document(db, &lDoc);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveVBState couchstore_save_local_document "
                   "error:%s [%s]", couchstore_strerror(errCode),
                   couchkvstore_strerrno(db, errCode).c_str());
    }

    return errCode;
}

couchstore_error_t CouchKVStore::saveCollectionsManifest(
        Db& db, const Item& collectionsManifest) {
    LocalDoc lDoc;
    lDoc.id.buf = const_cast<char*>(Collections::CouchstoreManifest);
    lDoc.id.size = Collections::CouchstoreManifestLen;

    // Convert the Item into JSON
    std::string state =
            Collections::VB::Manifest::serialToJson(collectionsManifest);

    lDoc.json.buf = const_cast<char*>(state.c_str());
    lDoc.json.size = state.size();
    lDoc.deleted = 0;

    couchstore_error_t errCode = couchstore_save_local_document(&db, &lDoc);

    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveCollectionsManifest "
                   "couchstore_save_local_document "
                   "error:%s [%s]",
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(&db, errCode).c_str());
    }

    return errCode;
}

std::string CouchKVStore::readCollectionsManifest(Db& db) {
    sized_buf id;
    id.buf = const_cast<char*>(Collections::CouchstoreManifest);
    id.size = sizeof(Collections::CouchstoreManifest) - 1;

    LocalDocHolder lDoc;
    auto errCode = couchstore_open_local_document(
            &db, (void*)id.buf, id.size, lDoc.getLocalDocAddress());
    if (errCode != COUCHSTORE_SUCCESS) {
        if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            logger.log(EXTENSION_LOG_NOTICE,
                   "CouchKVStore::readCollectionsManifest: doc not found");
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::readCollectionsManifest: "
                   "couchstore_open_local_document error:%s",
                   couchstore_strerror(errCode));
        }

        return {};
    }

    return {lDoc.getLocalDoc()->json.buf, lDoc.getLocalDoc()->json.size};
}

int CouchKVStore::getMultiCb(Db *db, DocInfo *docinfo, void *ctx) {
    if (docinfo == nullptr) {
        throw std::invalid_argument("CouchKVStore::getMultiCb: docinfo "
                "must be non-NULL");
    }
    if (ctx == nullptr) {
        throw std::invalid_argument("CouchKVStore::getMultiCb: ctx must "
                "be non-NULL");
    }

    GetMultiCbCtx *cbCtx = static_cast<GetMultiCbCtx *>(ctx);
    // Collections: TODO: Permanently restore to stored namespace
    DocKey key = makeDocKey(docinfo->id,
                            cbCtx->cks.getConfig().shouldPersistDocNamespace());
    KVStoreStats& st = cbCtx->cks.getKVStoreStat();

    vb_bgfetch_queue_t::iterator qitr = cbCtx->fetches.find(key);
    if (qitr == cbCtx->fetches.end()) {
        // this could be a serious race condition in couchstore,
        // log a warning message and continue
        cbCtx->cks.logger.log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::getMultiCb: Couchstore returned "
                              "invalid docinfo, no pending bgfetch has been "
                              "issued for a key in vb:%" PRIu16 ", "
                              "seqno:%" PRIu64, cbCtx->vbId, docinfo->rev_seq);
        return 0;
    }

    vb_bgfetch_item_ctx_t& bg_itm_ctx = (*qitr).second;
    GetMetaOnly meta_only = bg_itm_ctx.isMetaOnly;

    couchstore_error_t errCode = cbCtx->cks.fetchDoc(
            db, docinfo, bg_itm_ctx.value, cbCtx->vbId, meta_only);
    if (errCode != COUCHSTORE_SUCCESS && (meta_only == GetMetaOnly::No)) {
        st.numGetFailure++;
    }

    bg_itm_ctx.value.setStatus(cbCtx->cks.couchErr2EngineErr(errCode));

    bool return_val_ownership_transferred = false;
    for (auto& fetch : bg_itm_ctx.bgfetched_list) {
        return_val_ownership_transferred = true;
        // populate return value for remaining fetch items with the
        // same seqid
        fetch->value = &bg_itm_ctx.value;
        st.readTimeHisto.add(
                std::chrono::duration_cast<std::chrono::microseconds>(
                        ProcessClock::now() - fetch->initTime));
        if (errCode == COUCHSTORE_SUCCESS) {
            st.readSizeHisto.add(bg_itm_ctx.value.item->getKey().size() +
                                 bg_itm_ctx.value.item->getNBytes());
        }
    }
    if (!return_val_ownership_transferred) {
        cbCtx->cks.logger.log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::getMultiCb called with zero"
                              "items in bgfetched_list, vb:%" PRIu16
                              ", seqno:%" PRIu64,
                              cbCtx->vbId, docinfo->rev_seq);
    }

    return 0;
}


void CouchKVStore::closeDatabaseHandle(Db *db) {
    couchstore_error_t ret = couchstore_close_file(db);
    if (ret != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::closeDatabaseHandle: couchstore_close_file "
                   "error:%s [%s]", couchstore_strerror(ret),
                   couchkvstore_strerrno(db, ret).c_str());
    }
    ret = couchstore_free_db(db);
    if (ret != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::closeDatabaseHandle: couchstore_free_db "
                   "error:%s [%s]", couchstore_strerror(ret),
                   couchkvstore_strerrno(nullptr, ret).c_str());
    }
    st.numClose++;
}

ENGINE_ERROR_CODE CouchKVStore::couchErr2EngineErr(couchstore_error_t errCode) {
    switch (errCode) {
    case COUCHSTORE_SUCCESS:
        return ENGINE_SUCCESS;
    case COUCHSTORE_ERROR_ALLOC_FAIL:
        return ENGINE_ENOMEM;
    case COUCHSTORE_ERROR_DOC_NOT_FOUND:
        return ENGINE_KEY_ENOENT;
    case COUCHSTORE_ERROR_NO_SUCH_FILE:
    case COUCHSTORE_ERROR_NO_HEADER:
    default:
        // same as the general error return code of
        // EPBucket::getInternal
        return ENGINE_TMPFAIL;
    }
}

size_t CouchKVStore::getNumPersistedDeletes(uint16_t vbid) {
    size_t delCount = cachedDeleteCount[vbid];
    if (delCount != (size_t) -1) {
        return delCount;
    }

    DbHolder db(*this);
    couchstore_error_t errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        DbInfo info;
        errCode = couchstore_db_info(db, &info);
        if (errCode == COUCHSTORE_SUCCESS) {
            cachedDeleteCount[vbid] = info.deleted_count;
            return info.deleted_count;
        } else {
            throw std::runtime_error(
                    "CouchKVStore::getNumPersistedDeletes:"
                    "Failed to read database info for vBucket = " +
                    std::to_string(vbid) + " rev = " +
                    std::to_string(db.getFileRev()) + " with error:" +
                    couchstore_strerror(errCode));
        }
    } else {
        // open failed - map couchstore error code to exception.
        std::errc ec;
        switch (errCode) {
            case COUCHSTORE_ERROR_OPEN_FILE:
                ec = std::errc::no_such_file_or_directory; break;
            default:
                ec = std::errc::io_error; break;
        }
        throw std::system_error(std::make_error_code(ec),
                                "CouchKVStore::getNumPersistedDeletes:"
                                "Failed to open database file for vBucket = " +
                                        std::to_string(vbid) + " rev = " +
                                        std::to_string(db.getFileRev()) +
                                        " with error:" +
                                        couchstore_strerror(errCode));
    }
    return 0;
}

DBFileInfo CouchKVStore::getDbFileInfo(uint16_t vbid) {

    DbInfo info = getDbInfo(vbid);
    return DBFileInfo{info.file_size, info.space_used};
}

DBFileInfo CouchKVStore::getAggrDbFileInfo() {
    DBFileInfo kvsFileInfo;
    /**
     * Iterate over all the vbuckets to get the total.
     * If the vbucket is dead, then its value would
     * be zero.
     */
    for (uint16_t vbid = 0; vbid < numDbFiles; vbid++) {
        kvsFileInfo.fileSize += cachedFileSize[vbid].load();
        kvsFileInfo.spaceUsed += cachedSpaceUsed[vbid].load();
    }
    return kvsFileInfo;
}

size_t CouchKVStore::getNumItems(uint16_t vbid, uint64_t min_seq,
                                 uint64_t max_seq) {
    DbHolder db(*this);
    uint64_t count = 0;
    couchstore_error_t errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        errCode = couchstore_changes_count(db, min_seq, max_seq, &count);
        if (errCode != COUCHSTORE_SUCCESS) {
            throw std::runtime_error(
                    "CouchKVStore::getNumItems: Failed to "
                    "get changes count for vBucket = " +
                    std::to_string(vbid) + " rev = " +
                    std::to_string(db.getFileRev()) + " with error:" +
                    couchstore_strerror(errCode));
        }
    } else {
        throw std::invalid_argument(
                "CouchKVStore::getNumItems: Failed to "
                "open database file for vBucket = " +
                std::to_string(vbid) + " rev = " +
                std::to_string(db.getFileRev()) + " with error:" +
                couchstore_strerror(errCode));
    }
    return count;
}

size_t CouchKVStore::getItemCount(uint16_t vbid) {
    if (!isReadOnly()) {
        return cachedDocCount.at(vbid);
    }
    return getDbInfo(vbid).doc_count;
}

RollbackResult CouchKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                      std::shared_ptr<RollbackCB> cb) {
    DbHolder db(*this);
    DbInfo info;
    couchstore_error_t errCode;

    // Open the vbucket's file and determine the latestSeqno persisted.
    errCode = openDB(vbid, db, (uint64_t)COUCHSTORE_OPEN_FLAG_RDONLY);
    std::stringstream dbFileName;
    dbFileName << dbname << "/" << vbid << ".couch." << db.getFileRev();

    if (errCode == COUCHSTORE_SUCCESS) {
        errCode = couchstore_db_info(db, &info);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_db_info error:%s, "
                       "name:%s", couchstore_strerror(errCode),
                       dbFileName.str().c_str());
            return RollbackResult(false, 0, 0, 0);
        }
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: openDB error:%s, name:%s",
                   couchstore_strerror(errCode), dbFileName.str().c_str());
        return RollbackResult(false, 0, 0, 0);
    }
    uint64_t latestSeqno = info.last_sequence;

    // Count how many updates are in the vbucket's file. We'll later compare
    // this with how many items must be discarded and hence decide if it is
    // better to discard everything and start from an empty vBucket.
    uint64_t totSeqCount = 0;
    errCode = couchstore_changes_count(db, 0, latestSeqno, &totSeqCount);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: "
                   "couchstore_changes_count(0, %" PRIu64
                   ") error:%s [%s], "
                   "vb:%" PRIu16 ", rev:%" PRIu64,
                   latestSeqno,
                   couchstore_strerror(errCode),
                   cb_strerror().c_str(),
                   vbid,
                   db.getFileRev());
        return RollbackResult(false, 0, 0, 0);
    }

    // Open the vBucket file again; and search for a header which is
    // before the requested rollback point - the Rollback Header.
    DbHolder newdb(*this);
    errCode = openDB(vbid, newdb, 0);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: openDB#2 error:%s, name:%s",
                   couchstore_strerror(errCode), dbFileName.str().c_str());
        return RollbackResult(false, 0, 0, 0);
    }

    while (info.last_sequence > rollbackSeqno) {
        errCode = couchstore_rewind_db_header(newdb);
        if (errCode != COUCHSTORE_SUCCESS) {
            // rewind_db_header cleans up (frees DB) on error; so
            // release db in DbHolder to prevent a double-free.
            newdb.releaseDb();
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_rewind_db_header "
                       "error:%s [%s], vb:%" PRIu16 ", latestSeqno:%" PRIu64
                       ", rollbackSeqno:%" PRIu64,
                       couchstore_strerror(errCode), cb_strerror().c_str(),
                       vbid, latestSeqno, rollbackSeqno);
            //Reset the vbucket and send the entire snapshot,
            //as a previous header wasn't found.
            return RollbackResult(false, 0, 0, 0);
        }
        errCode = couchstore_db_info(newdb, &info);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_db_info error:%s, "
                       "name:%s", couchstore_strerror(errCode),
                       dbFileName.str().c_str());
            return RollbackResult(false, 0, 0, 0);
        }
    }

    // Count how many updates we need to discard to rollback to the Rollback
    // Header. If this is too many; then prefer to discard everything (than
    // have to patch up a large amount of in-memory data).
    uint64_t rollbackSeqCount = 0;
    errCode = couchstore_changes_count(
            db, info.last_sequence, latestSeqno, &rollbackSeqCount);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: "
                   "couchstore_changes_count#2(%" PRIu64 ", %" PRIu64
                   ") "
                   "error:%s [%s], vb:%" PRIu16 ", rev:%" PRIu64,
                   info.last_sequence,
                   latestSeqno,
                   couchstore_strerror(errCode),
                   cb_strerror().c_str(),
                   vbid,
                   db.getFileRev());
        return RollbackResult(false, 0, 0, 0);
    }
    if ((totSeqCount / 2) <= rollbackSeqCount) {
        //doresetVbucket flag set or rollback is greater than 50%,
        //reset the vbucket and send the entire snapshot
        return RollbackResult(false, 0, 0, 0);
    }

    // We have decided to perform a rollback to the Rollback Header.
    // Iterate across the series of keys which have been updated /since/ the
    // Rollback Header; invoking a callback on each. This allows the caller to
    // then inspect the state of the given key in the Rollback Header, and
    // correct the in-memory view:
    // * If the key is not present in the Rollback header then delete it from
    //   the HashTable (if either didn't exist yet, or had previously been
    //   deleted in the Rollback header).
    // * If the key is present in the Rollback header then replace the in-memory
    // value with the value from the Rollback header.
    cb->setDbHeader(newdb);
    auto cl = std::make_shared<NoLookupCallback>();
    ScanContext* ctx = initScanContext(cb, cl, vbid, info.last_sequence+1,
                                       DocumentFilter::ALL_ITEMS,
                                       ValueFilter::KEYS_ONLY);
    scan_error_t error = scan(ctx);
    destroyScanContext(ctx);

    if (error != scan_success) {
        return RollbackResult(false, 0, 0, 0);
    }

    if (readVBState(newdb, vbid) != ENGINE_SUCCESS) {
        return RollbackResult(false, 0, 0, 0);
    }
    cachedDeleteCount[vbid] = info.deleted_count;
    cachedDocCount[vbid] = info.doc_count;

    //Append the rewinded header to the database file
    errCode = couchstore_commit(newdb);

    if (errCode != COUCHSTORE_SUCCESS) {
        return RollbackResult(false, 0, 0, 0);
    }

    vbucket_state* vb_state = getVBucketState(vbid);
    return RollbackResult(true, vb_state->highSeqno,
                          vb_state->lastSnapStart, vb_state->lastSnapEnd);
}

int populateAllKeys(Db *db, DocInfo *docinfo, void *ctx) {
    AllKeysCtx *allKeysCtx = (AllKeysCtx *)ctx;
    // Collections: TODO: Restore to stored namespace
    DocKey key = makeDocKey(docinfo->id, false /*restore namespace*/);
    (allKeysCtx->cb)->callback(key);
    if (--(allKeysCtx->count) <= 0) {
        //Only when count met is less than the actual number of entries
        return COUCHSTORE_ERROR_CANCEL;
    }
    return COUCHSTORE_SUCCESS;
}

ENGINE_ERROR_CODE
CouchKVStore::getAllKeys(uint16_t vbid,
                         const DocKey start_key,
                         uint32_t count,
                         std::shared_ptr<Callback<const DocKey&>> cb) {
    DbHolder db(*this);
    couchstore_error_t errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if(errCode == COUCHSTORE_SUCCESS) {
        sized_buf ref = {NULL, 0};
        ref.buf = (char*) start_key.data();
        ref.size = start_key.size();
        AllKeysCtx ctx(cb, count);
        errCode = couchstore_all_docs(db,
                                      &ref,
                                      COUCHSTORE_NO_DELETES,
                                      populateAllKeys,
                                      static_cast<void*>(&ctx));
        if (errCode == COUCHSTORE_SUCCESS ||
                errCode == COUCHSTORE_ERROR_CANCEL)  {
            return ENGINE_SUCCESS;
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getAllKeys: couchstore_all_docs "
                       "error:%s [%s] vb:%" PRIu16 ", rev:%" PRIu64,
                       couchstore_strerror(errCode),
                       cb_strerror().c_str(),
                       vbid,
                       db.getFileRev());
        }
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getAllKeys: openDB error:%s, vb:%" PRIu16
                   ", rev:%" PRIu64,
                   couchstore_strerror(errCode),
                   vbid,
                   db.getFileRev());
    }
    return ENGINE_FAILED;
}

void CouchKVStore::unlinkCouchFile(uint16_t vbucket,
                                   uint64_t fRev) {

    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::unlinkCouchFile: Not valid on a "
                "read-only object.");
    }
    char fname[PATH_MAX];
    try {
        checked_snprintf(fname, sizeof(fname), "%s/%d.couch.%" PRIu64,
                         dbname.c_str(), vbucket, fRev);
    } catch (std::exception&) {
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::unlinkCouchFile: Failed to build filename:%s",
            fname);
        return;
    }

    if (remove(fname) == -1) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::unlinkCouchFile: remove error:%u, "
                   "vb:%" PRIu16 ", rev:%" PRIu64 ", fname:%s", errno, vbucket,
                   fRev, fname);

        if (errno != ENOENT) {
            std::string file_str = fname;
            pendingFileDeletions.push(file_str);
        }
    }
}

void CouchKVStore::removeCompactFile(const std::string& dbname, uint16_t vbid) {
    std::string dbfile = getDBFileName(dbname, vbid, (*dbFileRevMap)[vbid]);
    std::string compact_file = dbfile + ".compact";

    if (!isReadOnly()) {
        removeCompactFile(compact_file);
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::removeCompactFile: A read-only instance of "
                   "the underlying store was not allowed to delete a temporary"
                   "file: %s",
                   compact_file.c_str());
    }
}

void CouchKVStore::removeCompactFile(const std::string &filename) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::removeCompactFile: Not valid on "
                "a read-only object.");
    }

    if (access(filename.c_str(), F_OK) == 0) {
        if (remove(filename.c_str()) == 0) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::removeCompactFile: Removed compact "
                       "filename:%s", filename.c_str());
        }
        else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::removeCompactFile: remove error:%s, "
                       "filename:%s", cb_strerror().c_str(), filename.c_str());

            if (errno != ENOENT) {
                pendingFileDeletions.push(const_cast<std::string &>(filename));
            }
        }
    }
}

std::string CouchKVStore::getCollectionsManifest(uint16_t vbid) {
    DbHolder db(*this);

    // openDB logs error details
    couchstore_error_t errCode = openDB(vbid, db, COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        return {};
    }

    return readCollectionsManifest(*db);
}

void CouchKVStore::incrementRevision(uint16_t vbid) {
    (*dbFileRevMap)[vbid]++;
}

uint64_t CouchKVStore::prepareToDelete(uint16_t vbid) {
    // Clear the stats so it looks empty (real deletion of the disk data occurs
    // later)
    cachedDocCount[vbid] = 0;
    cachedDeleteCount[vbid] = 0;
    cachedFileSize[vbid] = 0;
    cachedSpaceUsed[vbid] = 0;
    return (*dbFileRevMap)[vbid];
}

/* end of couch-kvstore.cc */
