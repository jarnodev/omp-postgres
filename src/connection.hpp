#pragma once

#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct IPawnScript;

struct QueryParam
{
	bool isNull = false;
	std::string value;
};

// One SQL statement with its parameters.
struct Statement
{
	std::string sql;
	std::vector<QueryParam> params;
	// Built with pg_new_query: the SQL is usually fixed text, so it is worth
	// preparing once per connection. pg_query SQL often isn't, so it never is.
	bool prepare = false;
};

// An argument forwarded to the Pawn callback once the query completes.
struct CallbackArg
{
	bool isString = false;
	int32_t cell = 0;
	std::string str;
};

// A unit of work for a connection's worker: one statement, or several run
// together in a transaction.
struct Query
{
	int connectionId = 0;
	std::vector<Statement> statements;
	bool transaction = false;

	// Callback target. The generation guards against the script having been
	// unloaded (and maybe another loaded at the same address) in the meantime.
	IPawnScript* script = nullptr;
	uint64_t scriptGeneration = 0;
	std::string callback;
	std::vector<CallbackArg> args;

	// Filled in by the worker thread: one result per statement on success,
	// otherwise the error and the SQL that caused it.
	std::vector<PGresult*> results;
	std::string error;
	std::string sqlState;
	std::string failedSql;

	Query() = default;
	Query(const Query&) = delete;
	Query& operator=(const Query&) = delete;
	~Query() { clearResults(); }

	void clearResults()
	{
		for (PGresult* res : results)
		{
			PQclear(res);
		}
		results.clear();
	}

	bool failed() const { return !error.empty(); }
};

using QueryPtr = std::unique_ptr<Query>;

// Thread-safe hand-off of finished queries from workers back to the main thread.
class CompletionQueue
{
public:
	void push(QueryPtr query);
	// Called every server tick; an empty vector costs no allocation.
	std::vector<QueryPtr> takeAll();

private:
	std::mutex mutex_;
	std::vector<QueryPtr> done_;
};

// One PostgreSQL connection with its own worker thread. Queries on a single
// connection run strictly in the order they were sent.
class Connection
{
public:
	static constexpr int DefaultStatementCacheSize = 100;
	// While closing, a query that runs longer than this is cancelled and the
	// queries behind it fail without running, so a lock wait or a dead network
	// can't freeze the server during a GMX, script unload or shutdown.
	static constexpr std::chrono::milliseconds DefaultCloseTimeout { 10000 };

	Connection(int id, PGconn* conn, CompletionQueue& completions);
	~Connection();

	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	// Opens a connection synchronously. On failure returns nullptr and fills `error`.
	// Defaults (client_encoding, connect_timeout) are libpq parameters, so they
	// survive PQreset and anything in `conninfo` overrides them.
	static PGconn* open(const std::string& conninfo, std::string& error);

	void enqueue(QueryPtr query);

	// Lets queued queries finish, then stops the worker thread (see DefaultCloseTimeout).
	void close(std::chrono::milliseconds timeout = DefaultCloseTimeout);

	// The state seen by the last query; a connection that dropped while idle
	// still reports true until the next query notices.
	bool isConnected() const { return connected_.load(); }
	size_t pending() const;
	int id() const { return id_; }

	// Maximum number of prepared statements kept on the server; 0 disables preparing.
	void setStatementCacheSize(int size) { cacheSize_ = size < 0 ? 0 : size; }

private:
	void run();
	void execute(Query& query);
	bool executeOnce(Query& query);
	bool ensureConnected(std::string& error);

	// Runs one statement, through the prepared-statement cache when allowed.
	PGresult* exec(const Statement& statement);
	PGresult* simpleExec(const char* sql);
	void fail(Query& query, PGresult* res, const std::string& sql);
	void rollbackIfNeeded();
	void refreshCancel();
	void discardNotifications();

	// Prepared-statement cache, only touched by the worker thread.
	void forgetStatement(const std::string& sql, bool deallocate);
	void trimStatementCache(size_t limit);
	void flushDeallocations();
	void clearStatementCache();

	const int id_;
	PGconn* conn_;
	CompletionQueue& completions_;

	// Guards everything up to the worker-only state below.
	mutable std::mutex mutex_;
	std::condition_variable wake_;  // worker: a query was queued, or stop
	std::condition_variable idle_;  // close(): the worker finished
	std::deque<QueryPtr> queue_;
	bool stopping_ = false;
	bool finished_ = false;
	bool busy_ = false;
	std::chrono::steady_clock::time_point busySince_ {};
	// Set by close() after a timeout: queued queries fail without running.
	bool abandon_ = false;
	// Cancels the running query from another thread; refreshed on reconnect.
	PGcancel* cancel_ = nullptr;
	std::atomic<bool> connected_ { true };

	// Worker-only state.
	// While the server is down, queries fail fast instead of each waiting out a reconnect.
	std::chrono::steady_clock::time_point nextReconnect_ {};
	// Whether the last exec() ran a statement from the cache, and whether the
	// last failed attempt can safely run again.
	bool lastExecCached_ = false;
	bool retrySafe_ = false;

	struct CachedStatement
	{
		std::string name;
		std::list<std::string>::iterator lru;
	};
	std::atomic<int> cacheSize_ { DefaultStatementCacheSize };
	std::unordered_map<std::string, CachedStatement> statements_;
	std::list<std::string> lru_; // SQL texts, most recently used first
	std::vector<std::string> toDeallocate_;
	uint64_t nextStatement_ = 1;

	std::thread worker_;
};
