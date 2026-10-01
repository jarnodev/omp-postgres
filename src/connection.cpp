#include "connection.hpp"

void CompletionQueue::push(QueryPtr query)
{
	std::lock_guard<std::mutex> lock(mutex_);
	done_.push_back(std::move(query));
}

std::deque<QueryPtr> CompletionQueue::takeAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::deque<QueryPtr> out;
	out.swap(done_);
	return out;
}

PGconn* Connection::open(const std::string& conninfo, std::string& error)
{
	PGconn* conn = PQconnectdb(conninfo.c_str());
	if (conn == nullptr)
	{
		error = "out of memory";
		return nullptr;
	}
	if (PQstatus(conn) != CONNECTION_OK)
	{
		error = PQerrorMessage(conn);
		PQfinish(conn);
		return nullptr;
	}
	// SA-MP clients send Windows-1252 bytes, not UTF-8. Let the server convert,
	// unless the conninfo already chose an encoding.
	if (conninfo.find("client_encoding") == std::string::npos)
	{
		PQsetClientEncoding(conn, "WIN1252");
	}
	return conn;
}

Connection::Connection(int id, PGconn* conn, CompletionQueue& completions)
	: id_(id)
	, conn_(conn)
	, completions_(completions)
{
	worker_ = std::thread(&Connection::run, this);
}

Connection::~Connection()
{
	close();
	if (conn_)
	{
		PQfinish(conn_);
		conn_ = nullptr;
	}
}

void Connection::enqueue(QueryPtr query)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(std::move(query));
	}
	wake_.notify_one();
}

void Connection::close()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_ && !worker_.joinable())
		{
			return;
		}
		stopping_ = true;
	}
	wake_.notify_one();
	if (worker_.joinable())
	{
		worker_.join();
	}
}

size_t Connection::pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size() + inFlight_.load();
}

void Connection::run()
{
	for (;;)
	{
		QueryPtr query;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			// Drain the queue before stopping, so saves sent during shutdown still land.
			if (queue_.empty())
			{
				return;
			}
			query = std::move(queue_.front());
			queue_.pop_front();
			inFlight_ = 1;
		}
		execute(*query);
		completions_.push(std::move(query));
		inFlight_ = 0;
	}
}

bool Connection::ensureConnected(std::string& error)
{
	if (PQstatus(conn_) == CONNECTION_OK)
	{
		return true;
	}
	PQreset(conn_);
	if (PQstatus(conn_) == CONNECTION_OK)
	{
		PQsetClientEncoding(conn_, "WIN1252");
		connected_ = true;
		return true;
	}
	connected_ = false;
	error = PQerrorMessage(conn_);
	return false;
}

void Connection::execute(Query& query)
{
	if (!ensureConnected(query.error))
	{
		query.sqlState = "08006"; // connection_failure
		return;
	}

	std::vector<const char*> values;
	values.reserve(query.params.size());
	for (const QueryParam& p : query.params)
	{
		values.push_back(p.isNull ? nullptr : p.value.c_str());
	}

	// Text-format parameters; Postgres infers the types from the statement.
	PGresult* res = PQexecParams(conn_, query.sql.c_str(), static_cast<int>(values.size()), nullptr,
		values.data(), nullptr, nullptr, 0);

	ExecStatusType status = PQresultStatus(res);
	if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
	{
		query.result = res;
		return;
	}

	const char* state = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : nullptr;
	query.sqlState = state ? state : "";
	// The primary message is one line; the full message adds "LINE 1: ..." context.
	const char* primary = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : nullptr;
	query.error = primary ? primary : PQerrorMessage(conn_);
	if (query.error.empty())
	{
		query.error = "unknown error";
	}
	if (res)
	{
		PQclear(res);
	}
	if (PQstatus(conn_) != CONNECTION_OK)
	{
		connected_ = false;
	}
}
