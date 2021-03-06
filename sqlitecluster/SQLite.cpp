#include <libstuff/libstuff.h>
#include "SQLite.h"

#define DBINFO(_MSG_) SINFO("{" << _filename << "} " << _MSG_)

atomic<int64_t> SQLite::_transactionAttemptCount(0);
mutex SQLite::_pageLogMutex;

atomic<int> SQLite::passiveCheckpointPageMin(2500); // Approx 10mb
atomic<int> SQLite::fullCheckpointPageMin(25000); // Approx 100mb (pages are assumed to be 4kb)

// Tracing can only be enabled or disabled globally, not per object.
atomic<bool> SQLite::enableTrace(false);

string SQLite::initializeFilename(const string& filename) {
    // Canonicalize our filename and save that version.
    if (filename == ":memory:") {
        // This path is special, it exists in memory. This doesn't actually work correctly with journaling and such, as
        // we'll act as if they're all referencing the same file when we're not. This should therefore only be used
        // with a single SQLite object.
        return filename;
    } else {
        char resolvedPath[PATH_MAX];
        char* result = realpath(filename.c_str(), resolvedPath);
        if (!result) {
            SERROR("Couldn't resolve pathname for: " << filename);
        }
        return resolvedPath;
    }
}

SQLite::SharedData& SQLite::initializeSharedData(sqlite3* db, const string& filename, const vector<string>& journalNames) {
    static map<string, SharedData*> sharedDataLookupMap;
    static mutex instantiationMutex;
    lock_guard<mutex> lock(instantiationMutex);
    auto sharedDataIterator = sharedDataLookupMap.find(filename);
    if (sharedDataIterator == sharedDataLookupMap.end()) {
        SharedData* sharedData = new SharedData();

        // Read the highest commit count from the database, and store it in commitCount.
        string query = "SELECT MAX(maxIDs) FROM (" + _getJournalQuery(journalNames, {"SELECT MAX(id) as maxIDs FROM"}, true) + ")";
        SQResult result;
        SASSERT(!SQuery(db, "getting commit count", query, result));
        uint64_t commitCount = result.empty() ? 0 : SToUInt64(result[0][0]);
        sharedData->commitCount = commitCount;

        // And then read the hash for that transaction.
        string lastCommittedHash, ignore;
        getCommit(db, journalNames, commitCount, ignore, lastCommittedHash);
        sharedData->lastCommittedHash.store(lastCommittedHash);

        // If we have a commit count, we should have a hash as well.
        if (commitCount && lastCommittedHash.empty()) {
            SERROR("Loaded commit count " << commitCount << " with empty hash.");
        }

        // Insert our SharedData object into the global map.
        sharedDataLookupMap.emplace(filename, sharedData);
        return *sharedData;
    } else {
        // Otherwise, use the existing one.
        return *(sharedDataIterator->second);
    }
}

sqlite3* SQLite::initializeDB(const string& filename, int64_t mmapSizeGB) {
    // Open the DB in read-write mode.
    SINFO((SFileExists(filename) ? "Opening" : "Creating") << " database '" << filename << "'.");
    sqlite3* db;
    SASSERT(!sqlite3_open_v2(filename.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL));

    // PRAGMA legacy_file_format=OFF sets the default for creating new databases, so it must be called before creating
    // any tables to be effective.
    SASSERT(!SQuery(db, "new file format for DESC indexes", "PRAGMA legacy_file_format = OFF"));

    return db;
}

vector<string> SQLite::initializeJournal(sqlite3* db, int minJournalTables) {
    // Make sure we don't try and create more journals than we can name.
    SASSERT(minJournalTables < 10'000);

    // First, we create all of the tables through `minJournalTables` if they don't exist.
    for (int currentJounalTable = -1; currentJounalTable <= minJournalTables; currentJounalTable++) {
        char tableName[27] = {0};
        if (currentJounalTable < 0) {
            // The `-1` entry is just plain "journal".
            snprintf(tableName, 27, "journal");
        } else {
            snprintf(tableName, 27, "journal%04i", currentJounalTable);
        }
        if (SQVerifyTable(db, tableName, "CREATE TABLE " + string(tableName) + " ( id INTEGER PRIMARY KEY, query TEXT, hash TEXT )")) {
            SHMMM("Created " << tableName << " table.");
        }
    }

    // And we'll figure out which journal tables actually exist, which may be more than we require. They must be
    // sequential.
    int currentJounalTable = -1;
    vector<string> journalNames;
    while (true) {
        char tableName[27] = {0};
        if (currentJounalTable < 0) {
            // The `-1` entry is just plain "journal".
            snprintf(tableName, 27, "journal");
        } else {
            snprintf(tableName, 27, "journal%04i", currentJounalTable);
        }

        if (SQVerifyTableExists(db, tableName)) {
            journalNames.push_back(tableName);
            currentJounalTable++;
        } else {
            break;
        }
    }
    return journalNames;
}

uint64_t SQLite::initializeJournalSize(sqlite3* db, const vector<string>& journalNames) {
    // We keep track of the number of rows in the journal, so that we can delete old entries when we're over our size
    // limit.
    // We want the min of all journal tables.
    string minQuery = _getJournalQuery(journalNames, {"SELECT MIN(id) AS id FROM"}, true);
    minQuery = "SELECT MIN(id) AS id FROM (" + minQuery + ")";

    // And the max.
    string maxQuery = _getJournalQuery(journalNames, {"SELECT MAX(id) AS id FROM"}, true);
    maxQuery = "SELECT MAX(id) AS id FROM (" + maxQuery + ")";

    // Look up the min and max values in the database.
    SQResult result;
    SASSERT(!SQuery(db, "getting commit min", minQuery, result));
    uint64_t min = SToUInt64(result[0][0]);
    SASSERT(!SQuery(db, "getting commit max", maxQuery, result));
    uint64_t max = SToUInt64(result[0][0]);

    // And save the difference as the size of the journal.
    return max - min;
}

void SQLite::commonConstructorInitialization() {
    // Perform sanity checks.
    SASSERT(!_filename.empty());
    SASSERT(_cacheSize > 0);
    SASSERT(_maxJournalSize > 0);

    // Turn on page logging if specified.
    if (_pageLoggingEnabled) {
        sqlite3_begin_concurrent_report_enable(_db, 1);
    }

    // WAL is what allows simultaneous read/writing.
    SASSERT(!SQuery(_db, "enabling write ahead logging", "PRAGMA journal_mode = WAL;"));

    if (_mmapSizeGB) {
        SASSERT(!SQuery(_db, "enabling memory-mapped I/O", "PRAGMA mmap_size=" + to_string(_mmapSizeGB * 1024 * 1024 * 1024) + ";"));
    }

    // Do our own checkpointing.
    sqlite3_wal_hook(_db, _sqliteWALCallback, this);

    // Enable tracing for performance analysis.
    sqlite3_trace_v2(_db, SQLITE_TRACE_STMT, _sqliteTraceCallback, this);

    // Update the cache. -size means KB; +size means pages
    SINFO("Setting cache_size to " << _cacheSize << "KB");
    SQuery(_db, "increasing cache size", "PRAGMA cache_size = -" + SQ(_cacheSize) + ";");

    // Register the authorizer callback which allows callers to whitelist particular data in the DB.
    sqlite3_set_authorizer(_db, _sqliteAuthorizerCallback, this);

    // I tested and found that we could set about 10,000,000 and the number of steps to run and get a callback once a
    // second. This is set to be a bit more granular than that, which is probably adequate.
    sqlite3_progress_handler(_db, 1'000'000, _progressHandlerCallback, this);

    // Check if synchronous has been set and run query to use a custom synchronous setting
    if (!_synchronous.empty()) {
        SASSERT(!SQuery(_db, "setting custom synchronous commits", "PRAGMA synchronous = " + SQ(_synchronous)  + ";"));
    } else {
        DBINFO("Using SQLite default PRAGMA synchronous");
    }
}

SQLite::SQLite(const string& filename, int cacheSize, int maxJournalSize,
               int minJournalTables, const string& synchronous, int64_t mmapSizeGB, bool pageLoggingEnabled) :
    _filename(initializeFilename(filename)),
    _maxJournalSize(maxJournalSize),
    _db(initializeDB(_filename, mmapSizeGB)),
    _journalNames(initializeJournal(_db, minJournalTables)),
    _sharedData(initializeSharedData(_db, _filename, _journalNames)),
    _journalName(_journalNames[0]),
    _journalSize(initializeJournalSize(_db, _journalNames)),
    _pageLoggingEnabled(pageLoggingEnabled),
    _cacheSize(cacheSize),
    _synchronous(synchronous),
    _mmapSizeGB(mmapSizeGB)
{
    commonConstructorInitialization();
}

SQLite::SQLite(const SQLite& from) :
    _filename(from._filename),
    _maxJournalSize(from._maxJournalSize),
    _db(initializeDB(_filename, from._mmapSizeGB)), // Create a *new* DB handle from the same filename, don't copy the existing handle.
    _journalNames(from._journalNames),
    _sharedData(from._sharedData),
    _journalName(_journalNames[(_sharedData.nextJournalCount++ % _journalNames.size() - 1) + 1]),
    _journalSize(from._journalSize),
    _pageLoggingEnabled(from._pageLoggingEnabled),
    _cacheSize(from._cacheSize),
    _synchronous(from._synchronous),
    _mmapSizeGB(from._mmapSizeGB)
{
    commonConstructorInitialization();
}

int SQLite::_progressHandlerCallback(void* arg) {
    SQLite* sqlite = static_cast<SQLite*>(arg);
    uint64_t now = STimeNow();
    if (sqlite->_timeoutLimit && now > sqlite->_timeoutLimit) {
        // Timeout! We don't throw here, we let `read` and `write` do it so we don't throw out of the middle of a
        // sqlite3 operation.
        sqlite->_timeoutError = now - sqlite->_timeoutStart;

        // Return non-zero causes sqlite to interrupt the operation.
        return 1;
    } else if (sqlite->_sharedData._checkpointThreadBusy.load()) {
        if (sqlite->_enableCheckpointInterrupt) {
            SINFO("[checkpoint] Abandoning transaction to unblock checkpoint");
            sqlite->_abandonForCheckpoint = true;
            return 2;
        } else {
            SHMMM("[checkpoint] Not unblocking transaction for checkpoint because _enableCheckpointInterrupt disabled.");
        }
    }
    return 0;
}

void SQLite::_sqliteLogCallback(void* pArg, int iErrCode, const char* zMsg) {
    SSYSLOG(LOG_INFO, "[info] " << "{SQLITE} Code: " << iErrCode << ", Message: " << zMsg);
}

int SQLite::_sqliteTraceCallback(unsigned int traceCode, void* c, void* p, void* x) {
    if (enableTrace && traceCode == SQLITE_TRACE_STMT) {
        SINFO("NORMALIZED_SQL:" << sqlite3_normalized_sql((sqlite3_stmt*)p));
    }
    return 0;
}

int SQLite::_sqliteWALCallback(void* data, sqlite3* db, const char* dbName, int pageCount) {
    SQLite* object = static_cast<SQLite*>(data);
    object->_sharedData._currentPageCount.store(pageCount);
    // Try a passive checkpoint if full checkpoints aren't enabled, *or* if the page count is less than the required
    // size for a full checkpoint.
    if (pageCount >= fullCheckpointPageMin.load()) {
        // If we get here, then full checkpoints are enabled, and we have enough pages in the WAL file to perform one.
        SINFO("[checkpoint] " << pageCount << " pages behind, beginning complete checkpoint.");

        // This thread will run independently. We capture the variables we need here and pass them by value.
        string filename = object->_filename;
        string dbNameCopy = dbName;
        int alreadyCheckpointing = object->_sharedData._checkpointThreadBusy.fetch_add(1);
        if (alreadyCheckpointing) {
            SINFO("[checkpoint] Not starting checkpoint thread. It's already running.");
            return SQLITE_OK;
        }
        SDEBUG("[checkpoint] starting thread with count: " << object->_sharedData._currentPageCount.load());

        // We pass `destructorLock` into the thread to block the SQLite object's destructor from running while
        // the checkpoint is still using that object. The lock is created in the parent thread, and then passed
        // by move to the checkpoint thread, guaranteeing that there's no race here in returning from
        // _sqliteWALCallback before we've acquired the lock. This does not protect in the opposite direction: if you
        // destroy an SQLite while `_sqliteWALCallback` is running (i.e., inside a call to `SQLite::write()`, then
        // things will still break.
        thread([object, filename, dbNameCopy, destructorLock = unique_lock<mutex>(object->_destructorMutex)]() {
            SInitialize("checkpoint");
            uint64_t start = STimeNow();

            // Lock the mutex that keeps anyone from starting a new transaction.
            unique_lock<decltype(object->_sharedData.blockNewTransactionsMutex)> transactionLock(object->_sharedData.blockNewTransactionsMutex);

            while (1) {
                // Lock first, this prevents anyone from updating the count while we're operating here.
                unique_lock<mutex> lock(object->_sharedData.notifyWaitMutex);

                // Now that we have the lock, check the count. If there are no outstanding transactions, we can
                // checkpoint immediately, and then we'll return.
                int count = object->_sharedData.currentTransactionCount.load();

                // Lets re-check if we still need a full check point, it could be that a passive check point runs
                // after we have started this loop and check points a large chunk or all of the pages we were trying
                // to check point here. That means that this thread is now blocking new transactions waiting to run a
                // full check point for no reason. We wait for the page count to be less than half of the required amount
                // to prevent bouncing off of this check every loop. If that's the case, just break out of the this loop
                // and wait for the next full check point to be required.
                int pageCount = object->_sharedData._currentPageCount.load();
                if (pageCount < (fullCheckpointPageMin.load() / 2)) {
                    SINFO("[checkpoint] Page count decreased below half the threshold, count is now " << pageCount << ", exiting full checkpoint loop.");
                    break;
                } else {
                    SINFO("[checkpoint] Waiting on " << count << " remaining transactions.");
                    object->_sharedData.checkpointRequired(*object);
                }

                if (count == 0) {

                    // Time and run the checkpoint operation.
                    uint64_t checkpointStart = STimeNow();
                    SINFO("[checkpoint] Waited " << ((checkpointStart - start) / 1000)
                          << "ms for pending transactions. Starting complete checkpoint.");
                    int walSizeFrames = 0;
                    int framesCheckpointed = 0;
                    int result = sqlite3_wal_checkpoint_v2(object->_db, dbNameCopy.c_str(), SQLITE_CHECKPOINT_RESTART, &walSizeFrames, &framesCheckpointed);
                    SINFO("[checkpoint] restart checkpoint complete. Result: " << result << ". Total frames checkpointed: "
                          << framesCheckpointed << " of " << walSizeFrames
                          << " in " << ((STimeNow() - checkpointStart) / 1000) << "ms.");

                    // We're done. Anyone can start a new transaction.
                    object->_sharedData.checkpointComplete(*object);
                    break;
                }

                // There are outstanding transactions (or we would have hit `break` above), so we'll wait until
                // someone says the count has changed, and try again.
                object->_sharedData.blockNewTransactionsCV.wait(lock);
            }

            // Allow the next checkpointer.
            object->_sharedData._checkpointThreadBusy.store(0);
        }).detach();
    }
    return SQLITE_OK;
}

string SQLite::_getJournalQuery(const list<string>& queryParts, bool append) {
    return _getJournalQuery(_journalNames, queryParts, append);
}

string SQLite::_getJournalQuery(const vector<string>& journalNames, const list<string>& queryParts, bool append) {
    list<string> queries;
    for (const string& name : journalNames) {
        queries.emplace_back(SComposeList(queryParts, " " + name + " ") + (append ? " " + name : ""));
    }
    string query = SComposeList(queries, " UNION ");
    return query;
}

SQLite::~SQLite() {
    lock_guard<mutex> lock(_destructorMutex);
    // Now we can clean up our own data.
    // First, rollback any incomplete transaction.
    if (!_uncommittedQuery.empty()) {
        SINFO("Rolling back in destructor.");
        rollback();
        SINFO("Rollback in destructor complete.");
    }

    // Finally, Close the DB.
    DBINFO("Closing database '" << _filename << ".");
    SASSERTWARN(_uncommittedQuery.empty());
    SASSERT(!sqlite3_close(_db));
    DBINFO("Database closed.");
}

void SQLite::waitForCheckpoint() {
    shared_lock<decltype(_sharedData.blockNewTransactionsMutex)> lock(_sharedData.blockNewTransactionsMutex);
}

bool SQLite::beginTransaction(TRANSACTION_TYPE type) {
    if (type == TRANSACTION_TYPE::EXCLUSIVE) {
        _sharedData.commitLock.lock();
        _sharedData._commitLockTimer.start("EXCLUSIVE");
        _mutexLocked = true;
    }
    SASSERT(!_insideTransaction);
    SASSERT(_uncommittedHash.empty());
    SASSERT(_uncommittedQuery.empty());
    {
        unique_lock<mutex> lock(_sharedData.notifyWaitMutex);
        _sharedData.currentTransactionCount++;
    }
    _sharedData.blockNewTransactionsCV.notify_one();

    // Reset before the query, as it's possible the query sets these.
    _abandonForCheckpoint = false;
    _autoRolledBack = false;

    SDEBUG("[concurrent] Beginning transaction");
    uint64_t before = STimeNow();
    _currentTransactionAttemptCount = -1;
    _insideTransaction = !SQuery(_db, "starting db transaction", "BEGIN CONCURRENT");

    // Because some other thread could commit once we've run `BEGIN CONCURRENT`, this value can be slightly behind
    // where we're actually able to start such that we know we shouldn't get a conflict if this commits successfully on
    // leader. However, this is perfectly safe, it just adds the possibility that threads on followers wait for an
    // extra transaction to complete before starting, which is an anti-optimization, but the alternative is wrapping
    // the above `BEGIN CONCURRENT` and the `getCommitCount` call in a lock, which is worse.
    _dbCountAtStart = getCommitCount();
    _queryCache.clear();
    _queryCount = 0;
    _cacheHits = 0;
    _beginElapsed = STimeNow() - before;
    _readElapsed = 0;
    _writeElapsed = 0;
    _prepareElapsed = 0;
    _commitElapsed = 0;
    _rollbackElapsed = 0;
    return _insideTransaction;
}

bool SQLite::verifyTable(const string& tableName, const string& sql, bool& created) {
    // sqlite trims semicolon, so let's not supply it else we get confused later
    SASSERT(!SEndsWith(sql, ";"));

    // First, see if it's there
    SQResult result;
    SASSERT(read("SELECT sql FROM sqlite_master WHERE type='table' AND tbl_name=" + SQ(tableName) + ";", result));
    const string& collapsedSQL = SCollapse(sql);
    if (result.empty()) {
        // Table doesn't already exist, create it
        SINFO("Creating '" << tableName << "': " << collapsedSQL);
        SASSERT(write(collapsedSQL + ";"));
        created = true;
        return true; // New table was created to spec
    } else {
        // Table exists, verify it's correct.  Now, this can be a little tricky.
        // We'll count "correct" as having all the correct columns, in the correct
        // order.  However, the whitespace can differ.
        SASSERT(!result[0].empty());
        created = false;
        const string& collapsedResult = SCollapse(result[0][0]);
        if (SStrip(collapsedResult, " ", false) == SStrip(collapsedSQL, " ", false)) {
            // Looking good
            SINFO("'" << tableName << "' already exists with correct schema.");
            return true;
        } else {
            // Not right -- need to upgrade?
            SHMMM("'" << tableName << "' has incorrect schema, need to upgrade? Is '" << collapsedResult << "' expected  '" << collapsedSQL << "'");
            return false;
        }
    }
}

bool SQLite::verifyIndex(const string& indexName, const string& tableName, const string& indexSQLDefinition, bool isUnique, bool createIfNotExists) {
    SINFO("Verifying index '" << indexName << "'. isUnique? " << to_string(isUnique));
    SQResult result;
    SASSERT(read("SELECT sql FROM sqlite_master WHERE type='index' AND tbl_name=" + SQ(tableName) + " AND name=" + SQ(indexName) + ";", result));

    string createSQL = "CREATE" + string(isUnique ? " UNIQUE " : " ") + "INDEX " + indexName + " ON " + tableName + " " + indexSQLDefinition;
    if (result.empty()) {
        if (!createIfNotExists) {
            SINFO("Index '" << indexName << "' does not exist on table '" << tableName << "'.");
            return false;
        }
        SINFO("Creating index '" << indexName << "' on table '" << tableName << "': " << indexSQLDefinition << ". Executing '" << createSQL << "'.");
        SASSERT(write(createSQL + ";"));
        return true;
    } else {
        // Index exists, verify it is correct. Ignore spaces.
        SASSERT(!result[0].empty());
        return SIEquals(SReplace(createSQL, " ", ""), SReplace(result[0][0], " ", ""));
    }
}

bool SQLite::addColumn(const string& tableName, const string& column, const string& columnType) {
    // Add a column to the table if it does not exist.  Totally freak out on error.
    const string& sql =
        SCollapse(read("SELECT sql FROM sqlite_master WHERE type='table' AND tbl_name='" + tableName + "';"));
    if (!SContains(sql, " " + column + " ")) {
        // Add column
        SINFO("Adding " << column << " " << columnType << " to " << tableName);
        SASSERT(write("ALTER TABLE " + tableName + " ADD COLUMN " + column + " " + columnType + ";"));
        return true;
    }
    SWARN("Schema upgrade failed for table " << tableName << ", unrecognized sql '" << sql << "'");
    return false;
}

string SQLite::read(const string& query) {
    // Execute the read-only query
    SQResult result;
    if (!read(query, result)) {
        return "";
    }
    if (result.empty() || result[0].empty()) {
        return "";
    }
    return result[0][0];
}

bool SQLite::read(const string& query, SQResult& result) {
    uint64_t before = STimeNow();
    _queryCount++;
    auto foundQuery = _queryCache.find(query);
    if (foundQuery != _queryCache.end()) {
        result = foundQuery->second;
        _cacheHits++;
        return true;
    }
    _isDeterministicQuery = true;
    bool queryResult = !SQuery(_db, "read only query", query, result);
    if (_isDeterministicQuery && queryResult) {
        _queryCache.emplace(make_pair(query, result));
    }
    _checkInterruptErrors("SQLite::read"s);
    _readElapsed += STimeNow() - before;
    return queryResult;
}

void SQLite::_checkInterruptErrors(const string& error) {

    // Local error code.
    int errorCode = 0;
    uint64_t time = 0;

    // First check timeout. we want this to override the others, so we can't get stuck in an endless loop where we do
    // something like throw `checkpoint_required_error` forever and never notice that the command has timed out.
    if (_timeoutLimit) {
        uint64_t now = STimeNow();
        if (now > _timeoutLimit) {
            _timeoutError = now - _timeoutStart;
        }
        if (_timeoutError) {
            time = _timeoutError;
            resetTiming();
            errorCode = 1;
        }
    }

    if (_abandonForCheckpoint) {
        errorCode = 2;
    }

    // If we had an interrupt error, and were inside a transaction, and autocommit is now on, we have been auto-rolled
    // back, we won't need to actually do a rollback for this transaction.
    if (errorCode && _insideTransaction && sqlite3_get_autocommit(_db)) {
        SHMMM("Transaction automatically rolled back. Setting _autoRolledBack = true");
        _autoRolledBack = true;
    }

    // Reset this regardless of which error (or both) occurred. If we handled a timeout, this is still done, we don't
    // need to abandon this later.
    _abandonForCheckpoint = false;

    if (errorCode == 1) {
        throw timeout_error("timeout in "s + error, time);
    } else if (errorCode == 2) {
        throw checkpoint_required_error();
    }

    // Otherwise, no error.
}

bool SQLite::write(const string& query) {
    if (_noopUpdateMode) {
        SALERT("Non-idempotent write in _noopUpdateMode. Query: " << query);
        return true;
    }

    // This is literally identical to the idempotent version except for the check for _noopUpdateMode.
    return _writeIdempotent(query);
}

bool SQLite::writeIdempotent(const string& query) {
    return _writeIdempotent(query);
}

bool SQLite::writeUnmodified(const string& query) {
    return _writeIdempotent(query, true);
}

bool SQLite::_writeIdempotent(const string& query, bool alwaysKeepQueries) {
    SASSERT(_insideTransaction);
    _queryCache.clear();
    _queryCount++;
    SASSERT(query.empty() || SEndsWith(query, ";"));                        // Must finish everything with semicolon
    SASSERTWARN(SToUpper(query).find("CURRENT_TIMESTAMP") == string::npos); // Else will be replayed wrong

    // First, check our current state
    SQResult results;
    SASSERT(!SQuery(_db, "looking up schema version", "PRAGMA schema_version;", results));
    SASSERT(!results.empty() && !results[0].empty());
    uint64_t schemaBefore = SToUInt64(results[0][0]);
    uint64_t changesBefore = sqlite3_total_changes(_db);

    // Try to execute the query
    uint64_t before = STimeNow();
    bool result = false;
    bool usedRewrittenQuery = false;
    if (_enableRewrite) {
        int resultCode = SQuery(_db, "read/write transaction", query, 2000 * STIME_US_PER_MS, true);
        if (resultCode == SQLITE_AUTH) {
            // Run re-written query.
            _currentlyRunningRewritten = true;
            SASSERT(SEndsWith(_rewrittenQuery, ";"));
            result = !SQuery(_db, "read/write transaction", _rewrittenQuery);
            usedRewrittenQuery = true;
            _currentlyRunningRewritten = false;
        } else {
            result = !resultCode;
        }
    } else {
        result = !SQuery(_db, "read/write transaction", query);
    }
    _checkInterruptErrors("SQLite::write"s);
    _writeElapsed += STimeNow() - before;
    if (!result) {
        return false;
    }

    // See if the query changed anything
    SASSERT(!SQuery(_db, "looking up schema version", "PRAGMA schema_version;", results));
    SASSERT(!results.empty() && !results[0].empty());
    uint64_t schemaAfter = SToUInt64(results[0][0]);
    uint64_t changesAfter = sqlite3_total_changes(_db);

    // If something changed, or we're always keeping queries, then save this.
    if (alwaysKeepQueries || (schemaAfter > schemaBefore) || (changesAfter > changesBefore)) {
        _uncommittedQuery += usedRewrittenQuery ? _rewrittenQuery : query;
    }
    return true;
}

bool SQLite::prepare() {
    SASSERT(_insideTransaction);

    // We lock this here, so that we can guarantee the order in which commits show up in the database.
    if (!_mutexLocked) {
        _sharedData.commitLock.lock();
        _sharedData._commitLockTimer.start("SHARED");
        _mutexLocked = true;
    }

    // Now that we've locked anybody else from committing, look up the state of the database. We don't need to lock the
    // SharedData object to get these values as we know it can't currently change.
    string committedQuery, committedHash;
    uint64_t commitCount = _sharedData.commitCount;

    // Queue up the journal entry
    string lastCommittedHash = getCommittedHash(); // This is why we need the lock.
    _uncommittedHash = SToHex(SHashSHA1(lastCommittedHash + _uncommittedQuery));
    uint64_t before = STimeNow();

    // Crete our query.
    string query = "INSERT INTO " + _journalName + " VALUES (" + SQ(commitCount + 1) + ", " + SQ(_uncommittedQuery) + ", " + SQ(_uncommittedHash) + " )";

    // These are the values we're currently operating on, until we either commit or rollback.
    _sharedData.prepareTransactionInfo(commitCount + 1, _uncommittedQuery, _uncommittedHash, _dbCountAtStart);

    int result = SQuery(_db, "updating journal", query);
    _prepareElapsed += STimeNow() - before;
    if (result) {
        // Couldn't insert into the journal; roll back the original commit
        SWARN("Unable to prepare transaction, got result: " << result << ". Rolling back: " << _uncommittedQuery);
        rollback();
        return false;
    }

    // Ready to commit
    SDEBUG("Prepared transaction");

    // We're still holding commitLock now, and will until the commit is complete.
    return true;
}

int SQLite::commit() {
    SASSERT(_insideTransaction);
    SASSERT(!_uncommittedHash.empty()); // Must prepare first
    int result = 0;

    // Do we need to truncate as we go?
    uint64_t newJournalSize = _journalSize + 1;
    if (newJournalSize > _maxJournalSize) {
        // Delete the oldest entry
        uint64_t before = STimeNow();
        string query = "DELETE FROM " + _journalName + " "
                       "WHERE id < (SELECT MAX(id) FROM " + _journalName + ") - " + SQ(_maxJournalSize) + " "
                       "LIMIT 10";
        SASSERT(!SQuery(_db, "Deleting oldest journal rows", query));

        // Figure out the new journal size.
        SQResult result;
        SASSERT(!SQuery(_db, "getting commit min", "SELECT MIN(id) AS id FROM " + _journalName, result));
        uint64_t min = SToUInt64(result[0][0]);
        SASSERT(!SQuery(_db, "getting commit max", "SELECT MAX(id) AS id FROM " + _journalName, result));
        uint64_t max = SToUInt64(result[0][0]);
        newJournalSize = max - min;

        // Log timing info.
        _writeElapsed += STimeNow() - before;
    }

    // Make sure one is ready to commit
    SDEBUG("Committing transaction");

    // Record DB pages before commit to see how many the commit touches.
    int startPages, dummy;
    sqlite3_db_status(_db, SQLITE_DBSTATUS_CACHE_WRITE, &startPages, &dummy, 0);

    uint64_t before = STimeNow();
    uint64_t beforeCommit = STimeNow();
    if (_pageLoggingEnabled) {
        {
            lock_guard<mutex> lock(_pageLogMutex);
            _currentTransactionAttemptCount = _transactionAttemptCount.fetch_add(1);
            result = SQuery(_db, "committing db transaction", "COMMIT");
        }
    } else {
        result = SQuery(_db, "committing db transaction", "COMMIT");
    }

    // If there were conflicting commits, will return SQLITE_BUSY_SNAPSHOT
    SASSERT(result == SQLITE_OK || result == SQLITE_BUSY_SNAPSHOT);
    if (result == SQLITE_OK) {
        char time[16];
        snprintf(time, 16, "%.2fms", (double)(STimeNow() - beforeCommit) / 1000.0);
        SINFO("SQuery 'COMMIT' took " << time << ".");

        // And record pages after the commit.
        int endPages;
        sqlite3_db_status(_db, SQLITE_DBSTATUS_CACHE_WRITE, &endPages, &dummy, 0);

        // Similarly, record WAL file size.
        sqlite3_file *pWal = 0;
        sqlite3_int64 sz;
        sqlite3_file_control(_db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &pWal);
        pWal->pMethods->xFileSize(pWal, &sz);

        // And log both these statistics.
        SINFO("COMMIT operation wrote " << (endPages - startPages) << " pages. WAL file size is " << sz << " bytes.");

        if (_currentTransactionAttemptCount != -1) {
            const char* report = sqlite3_begin_concurrent_report(_db);
            string logLine = SWHEREAMI + "[row-level-locking] transaction attempt:" +
                             to_string(_currentTransactionAttemptCount) + " committed. report: " +
                             (report ? string(report) : "null"s);
            syslog(LOG_DEBUG, "%s", logLine.c_str());
        }
        _commitElapsed += STimeNow() - before;
        _journalSize = newJournalSize;
        _sharedData.incrementCommit(_uncommittedHash);
        SDEBUG("Commit successful (" << _sharedData.commitCount << "), releasing commitLock.");
        _insideTransaction = false;
        _uncommittedHash.clear();
        _uncommittedQuery.clear();
        _sharedData._commitLockTimer.stop();
        _sharedData.commitLock.unlock();
        _mutexLocked = false;
        _queryCache.clear();

        // Notify the checkpoint thread (if there is one) that it might be able to run now.
        {
            unique_lock<mutex> lock(_sharedData.notifyWaitMutex);
            _sharedData.currentTransactionCount--;
        }
        _sharedData.blockNewTransactionsCV.notify_one();

        // See if we can checkpoint without holding the commit lock.
        if (!_sharedData._checkpointThreadBusy) {
            int walSizeFrames = 0;
            int framesCheckpointed = 0;
            uint64_t start = STimeNow();
            int result = sqlite3_wal_checkpoint_v2(_db, 0, SQLITE_CHECKPOINT_PASSIVE, &walSizeFrames, &framesCheckpointed);
            SINFO("[checkpoint] passive checkpoint complete with " << _sharedData._currentPageCount
                  << " pages in WAL file. Result: " << result << ". Total frames checkpointed: "
                  << framesCheckpointed << " of " << walSizeFrames << " in " << ((STimeNow() - start) / 1000) << "ms.");
        }
        SINFO("Transaction commit with " << _queryCount << " queries attempted, " << _cacheHits << " served from cache.");
        _queryCount = 0;
        _cacheHits = 0;
        _dbCountAtStart = 0;
    } else {
        if (_currentTransactionAttemptCount != -1) {
            string logLine = SWHEREAMI  + "[row-level-locking] transaction attempt:" +
                             to_string(_currentTransactionAttemptCount) + " conflict, will roll back.";
            syslog(LOG_DEBUG, "%s", logLine.c_str());
            SINFO("Commit failed, waiting for rollback.");
        }
    }

    // Reset this to the default on any completion of the transaction, successful or not.
    _enableCheckpointInterrupt = true;

    // if we got SQLITE_BUSY_SNAPSHOT, then we're *still* holding commitLock, and it will need to be unlocked by
    // calling rollback().
    return result;
}

map<uint64_t, tuple<string, string, uint64_t>> SQLite::popCommittedTransactions() {
    return _sharedData.popCommittedTransactions();
}

void SQLite::rollback() {
    // Make sure we're actually inside a transaction
    if (_insideTransaction) {
        // Cancel this transaction
        if (_autoRolledBack) {
            SINFO("Transaction was automatically rolled back, not sending 'ROLLBACK'.");
            _autoRolledBack = false;
        } else {
            if (_uncommittedQuery.size()) {
                SINFO("Rolling back transaction: " << _uncommittedQuery.substr(0, 100));
            }
            uint64_t before = STimeNow();
            SASSERT(!SQuery(_db, "rolling back db transaction", "ROLLBACK"));
            _rollbackElapsed += STimeNow() - before;
        }

        if (_currentTransactionAttemptCount != -1) {
            const char* report = sqlite3_begin_concurrent_report(_db);
            string logLine = SWHEREAMI + "[row-level-locking] transaction attempt:" +
                             to_string(_currentTransactionAttemptCount) + " rolled back. report: " +
                             (report ? string(report) : "null"s);
            syslog(LOG_DEBUG, "%s", logLine.c_str());
        }

        // Finally done with this.
        _insideTransaction = false;
        _uncommittedHash.clear();
        if (_uncommittedQuery.size()) {
            SINFO("Rollback successful.");
        }
        _uncommittedQuery.clear();

        // Only unlock the mutex if we've previously locked it. We can call `rollback` to cancel a transaction without
        // ever having called `prepare`, which would have locked our mutex.
        if (_mutexLocked) {
            _mutexLocked = false;
            _sharedData._commitLockTimer.stop();
            _sharedData.commitLock.unlock();
        }
        {
            unique_lock<mutex> lock(_sharedData.notifyWaitMutex);
            _sharedData.currentTransactionCount--;
        }
        _sharedData.blockNewTransactionsCV.notify_one();
    } else {
        SINFO("Rolling back but not inside transaction, ignoring.");
    }
    _queryCache.clear();
    SINFO("Transaction rollback with " << _queryCount << " queries attempted, " << _cacheHits << " served from cache.");
    _queryCount = 0;
    _cacheHits = 0;
    _dbCountAtStart = 0;

    // Reset this to the default on any completion of the transaction, successful or not.
    _enableCheckpointInterrupt = true;
}

uint64_t SQLite::getLastTransactionTiming(uint64_t& begin, uint64_t& read, uint64_t& write, uint64_t& prepare,
                                          uint64_t& commit, uint64_t& rollback) {
    // Just populate and return
    begin = _beginElapsed;
    read = _readElapsed;
    write = _writeElapsed;
    prepare = _prepareElapsed;
    commit = _commitElapsed;
    rollback = _rollbackElapsed;
    return begin + read + write + prepare + commit + rollback;
}

bool SQLite::getCommit(uint64_t id, string& query, string& hash) {
    return getCommit(_db, _journalNames, id, query, hash);
}

bool SQLite::getCommit(sqlite3* db, const vector<string> journalNames, uint64_t id, string& query, string& hash) {
    // TODO: This can fail if called after `BEGIN TRANSACTION`, if the id we want to look up was committed by another
    // thread. We may or may never need to handle this case.
    // Look up the query and hash for the given commit
    string internalQuery = _getJournalQuery(journalNames, {"SELECT query, hash FROM", "WHERE id = " + SQ(id)});
    SQResult result;
    SASSERT(!SQuery(db, "getting commit", internalQuery, result));
    if (!result.empty()) {
        query = result[0][0];
        hash = result[0][1];
    } else {
        query = "";
        hash = "";
    }
    if (id) {
        SASSERTWARN(!query.empty());
        SASSERTWARN(!hash.empty());
    }

    // If we found a hash, we assume this was a good commit, as we'll allow an empty commit.
    return (!hash.empty());
}

string SQLite::getCommittedHash() {
    return _sharedData.lastCommittedHash.load();
}

bool SQLite::getCommits(uint64_t fromIndex, uint64_t toIndex, SQResult& result) {
    // Look up all the queries within that range
    SASSERTWARN(SWITHIN(1, fromIndex, toIndex));
    string query = _getJournalQuery({"SELECT id, hash, query FROM", "WHERE id >= " + SQ(fromIndex) +
                                    (toIndex ? " AND id <= " + SQ(toIndex) : "")});
    SDEBUG("Getting commits #" << fromIndex << "-" << toIndex);
    query = "SELECT hash, query FROM (" + query  + ") ORDER BY id";
    return !SQuery(_db, "getting commits", query, result);
}

int64_t SQLite::getLastInsertRowID() {
    // Make sure it *does* happen after an INSERT, but not with a IGNORE
    SASSERTWARN(SContains(_uncommittedQuery, "INSERT") || SContains(_uncommittedQuery, "REPLACE"));
    SASSERTWARN(!SContains(_uncommittedQuery, "IGNORE"));
    int64_t sqliteRowID = (int64_t)sqlite3_last_insert_rowid(_db);
    return sqliteRowID;
}

uint64_t SQLite::getCommitCount() {
    return _sharedData.commitCount;
}

size_t SQLite::getLastWriteChangeCount() {
    int count = sqlite3_changes(_db);
    return count > 0 ? (size_t)count : 0;
}

void SQLite::enableRewrite(bool enable) {
    _enableRewrite = enable;
}

void SQLite::setRewriteHandler(bool (*handler)(int, const char*, string&)) {
    _rewriteHandler = handler;
}

int SQLite::_sqliteAuthorizerCallback(void* pUserData, int actionCode, const char* detail1, const char* detail2,
                                      const char* detail3, const char* detail4)
{
    SQLite* db = static_cast<SQLite*>(pUserData);
    return db->_authorize(actionCode, detail1, detail2, detail3, detail4);
}

int SQLite::_authorize(int actionCode, const char* detail1, const char* detail2, const char* detail3, const char* detail4) {
    // If we've enabled re-writing, see if we need to re-write this query.
    if (_enableRewrite && !_currentlyRunningRewritten && (*_rewriteHandler)(actionCode, detail1, _rewrittenQuery)) {
        // Deny the original query, we'll re-run on the re-written version.
        return SQLITE_DENY;
    }

    // Here's where we can check for non-deterministic functions for the cache.
    if (actionCode == SQLITE_FUNCTION && detail2) {
        if (!strcmp(detail2, "random") ||
            !strcmp(detail2, "date") ||
            !strcmp(detail2, "time") ||
            !strcmp(detail2, "datetime") ||
            !strcmp(detail2, "julianday") ||
            !strcmp(detail2, "strftime") ||
            !strcmp(detail2, "changes") ||
            !strcmp(detail2, "last_insert_rowid") ||
            !strcmp(detail2, "sqlite3_version")
        ) {
            _isDeterministicQuery = false;
        }
    }

    // If the whitelist isn't set, we always return OK.
    if (!whitelist) {
        return SQLITE_OK;
    }

    switch (actionCode) {
        // The following are *always* disallowed in whitelist mode.
        case SQLITE_CREATE_INDEX:
        case SQLITE_CREATE_TABLE:
        case SQLITE_CREATE_TEMP_INDEX:
        case SQLITE_CREATE_TEMP_TABLE:
        case SQLITE_CREATE_TEMP_TRIGGER:
        case SQLITE_CREATE_TEMP_VIEW:
        case SQLITE_CREATE_TRIGGER:
        case SQLITE_CREATE_VIEW:
        case SQLITE_DELETE:
        case SQLITE_DROP_INDEX:
        case SQLITE_DROP_TABLE:
        case SQLITE_DROP_TEMP_INDEX:
        case SQLITE_DROP_TEMP_TABLE:
        case SQLITE_DROP_TEMP_TRIGGER:
        case SQLITE_DROP_TEMP_VIEW:
        case SQLITE_DROP_TRIGGER:
        case SQLITE_DROP_VIEW:
        case SQLITE_INSERT:
        case SQLITE_TRANSACTION:
        case SQLITE_UPDATE:
        case SQLITE_ATTACH:
        case SQLITE_DETACH:
        case SQLITE_ALTER_TABLE:
        case SQLITE_REINDEX:
        case SQLITE_CREATE_VTABLE:
        case SQLITE_DROP_VTABLE:
        case SQLITE_SAVEPOINT:
        case SQLITE_COPY:
        case SQLITE_RECURSIVE:
            return SQLITE_DENY;
            break;

        // The following are *always* allowed in whitelist mode.
        case SQLITE_SELECT:
        case SQLITE_ANALYZE:
        case SQLITE_FUNCTION:
            return SQLITE_OK;
            break;
        case SQLITE_PRAGMA:
        {
            string normalizedTable = SToLower(detail1);
            // We allow this particularly because we call it ourselves in `write`, and so if it's not allowed, all
            // write queries will always fail. We specifically check that `column` is empty, because if it's set, that
            // means the caller has tried to specify a schema version, which we disallow, as it can cause DB
            // corruption. Note that this still allows `PRAGMA schema_version = 1;` to crash the process. This needs to
            // get caught sooner.
            if (normalizedTable == "schema_version" && detail2 == 0) {
                return SQLITE_OK;
            } else {
                return SQLITE_DENY;
            }
            break;
        }
        case SQLITE_READ:
        {
            // See if there's an entry in the whitelist for this table.
            auto tableIt = whitelist->find(detail1);
            if (tableIt != whitelist->end()) {
                // If so, see if there's an entry for this column.
                auto columnIt = tableIt->second.find(detail2);
                if (columnIt != tableIt->second.end()) {
                    // If so, then this column is whitelisted.
                    return SQLITE_OK;
                }
            }

            // If we didn't find it, not whitelisted.
            SWARN("[security] Non-whitelisted column: " << detail2 << " in table " << detail1 << ".");
            return SQLITE_IGNORE;
        }
    }
    return SQLITE_DENY;
}

void SQLite::startTiming(uint64_t timeLimitUS) {
    _timeoutStart = STimeNow();
    _timeoutLimit = _timeoutStart + timeLimitUS;
    _timeoutError = 0;
}

void SQLite::resetTiming() {
    _timeoutLimit = 0;
    _timeoutStart = 0;
    _timeoutError = 0;
}

void SQLite::setUpdateNoopMode(bool enabled) {
    if (_noopUpdateMode == enabled) {
        return;
    }

    // Enable or disable this query.
    string query = "PRAGMA noop_update="s + (enabled ? "ON" : "OFF") + ";";
    SQuery(_db, "setting noop-update mode", query);
    _noopUpdateMode = enabled;

    // If we're inside a transaction, make sure this gets saved so it can be replicated.
    // If we're not (i.e., a transaction's already been rolled back), no need, there's nothing to replicate.
    if (_insideTransaction) {
        _uncommittedQuery += query;
    }
}

bool SQLite::getUpdateNoopMode() const {
    return _noopUpdateMode;
}

uint64_t SQLite::getDBCountAtStart() const {
    return _dbCountAtStart;
}

void SQLite::addCheckpointListener(SQLite::CheckpointRequiredListener& listener) {
    _sharedData.addCheckpointListener(listener);
}

void SQLite::removeCheckpointListener(SQLite::CheckpointRequiredListener& listener) {
    _sharedData.removeCheckpointListener(listener);
}

SQLite::SharedData::SharedData() :
nextJournalCount(0),
currentTransactionCount(0),
_currentPageCount(0),
_checkpointThreadBusy(0),
_commitLockTimer("commit lock timer", {
    {"EXCLUSIVE", chrono::steady_clock::duration::zero()},
    {"SHARED", chrono::steady_clock::duration::zero()},
})
{ }

void SQLite::SharedData::addCheckpointListener(SQLite::CheckpointRequiredListener& listener) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    _checkpointListeners.insert(&listener);
}

void SQLite::SharedData::removeCheckpointListener(SQLite::CheckpointRequiredListener& listener) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    _checkpointListeners.erase(&listener);
}

void SQLite::SharedData::checkpointRequired(SQLite& db) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    for (auto listener : _checkpointListeners) {
        listener->checkpointRequired(db);
    }
}

void SQLite::SharedData::checkpointComplete(SQLite& db) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    for (auto listener : _checkpointListeners) {
        listener->checkpointComplete(db);
    }
}

void SQLite::SharedData::incrementCommit(const string& commitHash) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    commitCount++;
    commitTransactionInfo(commitCount);
    lastCommittedHash.store(commitHash);
}

void SQLite::SharedData::prepareTransactionInfo(uint64_t commitID, const string& query, const string& hash, uint64_t dbCountAtTransactionStart) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    _preparedTransactions.insert_or_assign(commitID, make_tuple(query, hash, dbCountAtTransactionStart));
}

void SQLite::SharedData::commitTransactionInfo(uint64_t commitID) {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    _committedTransactions.insert(_preparedTransactions.extract(commitID));
}

map<uint64_t, tuple<string, string, uint64_t>> SQLite::SharedData::popCommittedTransactions() {
    lock_guard<decltype(_internalStateMutex)> lock(_internalStateMutex);
    decltype(_committedTransactions) result;
    result = move(_committedTransactions);
    _committedTransactions.clear();
    return result;
}
