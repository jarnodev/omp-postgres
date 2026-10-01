#pragma once

#include <libpq-fe.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IPawnScript;

struct QueryParam
{
	bool isNull = false;
	std::string value;
};

// An argument forwarded to the Pawn callback once the query completes.
struct CallbackArg
{
	bool isString = false;
	int32_t cell = 0;
	std::string str;
};

struct Query
{
	int connectionId = 0;
	std::string sql;
	std::vector<QueryParam> params;

	// Callback target. The generation guards against the script having been
	// unloaded (and maybe another loaded at the same address) in the meantime.
	IPawnScript* script = nullptr;
	uint64_t scriptGeneration = 0;
	std::string callback;
	std::vector<CallbackArg> args;

	// Filled in by the worker thread.
	PGresult* result = nullptr;
	std::string error;
	std::string sqlState;

	~Query()
	{
		if (result)
		{
			PQclear(result);
		}
	}

	bool failed() const { return !error.empty(); }
};

using QueryPtr = std::unique_ptr<Query>;

// Thread-safe hand-off of finished queries from workers back to the main thread.
class CompletionQueue
{
public:
	void push(QueryPtr query);
	std::deque<QueryPtr> takeAll();

private:
	std::mutex mutex_;
	std::deque<QueryPtr> done_;
};

// One PostgreSQL connection with its own worker thread. Queries on a single
// connection run strictly in the order they were sent.
class Connection
{
public:
	Connection(int id, PGconn* conn, CompletionQueue& completions);
	~Connection();

	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	// Opens a connection synchronously. On failure returns nullptr and fills `error`.
	static PGconn* open(const std::string& conninfo, std::string& error);

	void enqueue(QueryPtr query);

	// Lets queued queries finish, then stops the worker thread.
	void close();

	bool isConnected() const { return connected_.load(); }
	size_t pending() const;
	int id() const { return id_; }

private:
	void run();
	void execute(Query& query);
	bool ensureConnected(std::string& error);

	const int id_;
	PGconn* conn_;
	CompletionQueue& completions_;

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<QueryPtr> queue_;
	bool stopping_ = false;
	std::atomic<bool> connected_ { true };
	std::atomic<size_t> inFlight_ { 0 };
	std::thread worker_;
};
