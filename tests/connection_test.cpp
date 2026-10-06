// Tests for the connection layer against a real PostgreSQL server.
// Connects with the standard libpq environment (PGHOST, PGPORT, PGUSER,
// PGPASSWORD, PGDATABASE). Creates and drops tables named omp_test_*.
#include "connection.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		if (!(cond))                                                               \
		{                                                                          \
			std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                                          \
		}                                                                          \
	} while (0)

std::string value(const Query& q, int result = -1, int row = 0, int column = 0)
{
	if (q.results.empty())
	{
		return "<no result>";
	}
	const PGresult* res = q.results[result < 0 ? q.results.size() - 1 : result];
	if (row >= PQntuples(res) || column >= PQnfields(res))
	{
		return "<out of range>";
	}
	return PQgetisnull(res, row, column) ? "<null>" : PQgetvalue(res, row, column);
}

Statement sql(std::string text, std::vector<QueryParam> params = {}, bool prepare = false)
{
	return Statement { std::move(text), std::move(params), prepare };
}

struct Fixture
{
	CompletionQueue done;
	std::unique_ptr<Connection> conn;

	explicit Fixture(const std::string& conninfo = "")
	{
		std::string error;
		PGconn* raw = Connection::open(conninfo, error);
		if (raw == nullptr)
		{
			std::fprintf(stderr, "cannot connect: %s\n", error.c_str());
			std::exit(2);
		}
		conn = std::make_unique<Connection>(1, raw, done);
	}

	void send(std::vector<Statement> statements, bool transaction = false)
	{
		auto q = std::make_unique<Query>();
		q->statements = std::move(statements);
		q->transaction = transaction;
		conn->enqueue(std::move(q));
	}

	// Waits for `count` queries to complete, in order.
	std::vector<QueryPtr> wait(size_t count)
	{
		std::vector<QueryPtr> out;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (out.size() < count && std::chrono::steady_clock::now() < deadline)
		{
			for (QueryPtr& q : done.takeAll())
			{
				out.push_back(std::move(q));
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (out.size() < count)
		{
			std::fprintf(stderr, "timed out waiting for queries\n");
			std::exit(2);
		}
		return out;
	}

	QueryPtr run(std::vector<Statement> statements, bool transaction = false)
	{
		send(std::move(statements), transaction);
		return std::move(wait(1)[0]);
	}

	QueryPtr run(Statement statement) { return run(std::vector<Statement> { std::move(statement) }); }
};

// A plain libpq connection for setting things up behind the tested connection's back.
struct Admin
{
	PGconn* conn = PQconnectdb("");
	~Admin() { PQfinish(conn); }
	std::string exec(const std::string& text)
	{
		PGresult* res = PQexec(conn, text.c_str());
		std::string out = PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 ? PQgetvalue(res, 0, 0) : "";
		PQclear(res);
		return out;
	}
};

void testBasics()
{
	Fixture f;
	QueryPtr q = f.run(sql("SELECT 1 + 1"));
	CHECK(!q->failed());
	CHECK(value(*q) == "2");

	q = f.run(sql("SELECT $1::int + 1, $2::text", { { false, "41" }, { true, {} } }, true));
	CHECK(!q->failed());
	CHECK(value(*q, -1, 0, 0) == "42");
	CHECK(value(*q, -1, 0, 1) == "<null>");

	q = f.run(sql("SELECT * FROM omp_test_does_not_exist"));
	CHECK(q->failed());
	CHECK(q->sqlState == "42P01");
	CHECK(q->results.empty());
}

void testEncoding()
{
	if (std::getenv("PGCLIENTENCODING") == nullptr)
	{
		Fixture f;
		CHECK(value(*f.run(sql("SHOW client_encoding"))) == "WIN1252");
	}
	Fixture utf8("client_encoding=UTF8");
	CHECK(value(*utf8.run(sql("SHOW client_encoding"))) == "UTF8");
}

void testStaleStatements()
{
	Fixture f;
	f.run(sql("DROP TABLE IF EXISTS omp_test_stale"));
	f.run(sql("CREATE TABLE omp_test_stale (a int)"));
	QueryPtr q = f.run(sql("SELECT * FROM omp_test_stale", {}, true));
	CHECK(!q->failed());

	// 0A000 "cached plan must not change result type": re-prepared and retried.
	f.run(sql("ALTER TABLE omp_test_stale ADD COLUMN b int"));
	q = f.run(sql("SELECT * FROM omp_test_stale", {}, true));
	CHECK(!q->failed());
	CHECK(!q->results.empty() && PQnfields(q->results[0]) == 2);

	// 26000: the statement disappeared from the server.
	f.run(sql("DEALLOCATE ALL"));
	q = f.run(sql("SELECT * FROM omp_test_stale", {}, true));
	CHECK(!q->failed());
	f.run(sql("DROP TABLE omp_test_stale"));
}

void testStatementCacheLimit()
{
	Fixture f;
	f.conn->setStatementCacheSize(2);
	f.run(sql("SELECT 1", {}, true));
	f.run(sql("SELECT 2", {}, true));
	f.run(sql("SELECT 3", {}, true));
	QueryPtr q = f.run(sql("SELECT count(*) FROM pg_prepared_statements"));
	CHECK(value(*q) == "2");

	f.conn->setStatementCacheSize(0);
	f.run(sql("SELECT 4", {}, true));
	q = f.run(sql("SELECT count(*) FROM pg_prepared_statements"));
	CHECK(value(*q) == "0");
}

void testTransactions()
{
	Fixture f;
	f.run(sql("DROP TABLE IF EXISTS omp_test_tx"));
	f.run(sql("CREATE TABLE omp_test_tx (a int)"));

	QueryPtr q = f.run({ sql("INSERT INTO omp_test_tx VALUES (1)"), sql("SELECT count(*) FROM omp_test_tx") }, true);
	CHECK(!q->failed());
	CHECK(q->results.size() == 2);
	CHECK(value(*q, 1) == "1");

	q = f.run({ sql("INSERT INTO omp_test_tx VALUES (2)"), sql("SELECT 1/0") }, true);
	CHECK(q->failed());
	CHECK(q->sqlState == "22012");
	CHECK(q->failedSql == "SELECT 1/0");
	CHECK(q->results.empty());
	CHECK(value(*f.run(sql("SELECT count(*) FROM omp_test_tx"))) == "1");
	f.run(sql("DROP TABLE omp_test_tx"));
}

void testReconnectAfterIdleDrop()
{
	Fixture f;
	Admin admin;
	std::string pid = value(*f.run(sql("SELECT pg_backend_pid()")));
	admin.exec("SELECT pg_terminate_backend(" + pid + ")");
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	// The drop is noticed before sending, so this runs on a new connection.
	QueryPtr q = f.run(sql("SELECT pg_backend_pid()"));
	CHECK(!q->failed());
	CHECK(value(*q) != pid);
	CHECK(f.conn->isConnected());
}

void testDropDuringQuery()
{
	Fixture f;
	f.run(sql("DROP SEQUENCE IF EXISTS omp_test_seq"));
	f.run(sql("CREATE SEQUENCE omp_test_seq"));
	// Sequences ignore rollbacks, so only the first run kills its own connection.
	const char* killFirstRun
		= "SELECT CASE WHEN nextval('omp_test_seq') = 1 THEN pg_terminate_backend(pg_backend_pid()) END, pg_sleep(0.5)";

	// A transaction that dropped before COMMIT was rolled back: it runs again.
	QueryPtr q = f.run({ sql("SELECT 1"), sql(killFirstRun) }, true);
	CHECK(!q->failed());
	CHECK(value(*f.run(sql("SELECT last_value FROM omp_test_seq"))) == "2");

	// A single statement may have been applied, so it is not run again.
	f.run(sql("SELECT setval('omp_test_seq', 1, false)"));
	q = f.run(sql(killFirstRun));
	CHECK(q->failed());
	CHECK(!q->sqlState.empty());
	CHECK(value(*f.run(sql("SELECT last_value FROM omp_test_seq"))) == "1");
	f.run(sql("DROP SEQUENCE omp_test_seq"));
}

void testCloseTimeout()
{
	Fixture setup;
	setup.run(sql("DROP TABLE IF EXISTS omp_test_lock"));
	setup.run(sql("CREATE TABLE omp_test_lock (a int)"));

	Admin admin;
	admin.exec("BEGIN");
	admin.exec("LOCK TABLE omp_test_lock");

	Fixture f;
	f.send({ sql("SELECT * FROM omp_test_lock") });
	f.send({ sql("SELECT 1") });
	CHECK(f.conn->pending() == 2);

	auto start = std::chrono::steady_clock::now();
	f.conn->close(std::chrono::milliseconds(500));
	auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed < std::chrono::seconds(5));

	std::vector<QueryPtr> results = f.wait(2);
	CHECK(results[0]->sqlState == "57014"); // query_canceled
	CHECK(results[1]->sqlState == "08003"); // never ran
	CHECK(f.conn->pending() == 0);

	admin.exec("ROLLBACK");
	setup.run(sql("DROP TABLE omp_test_lock"));
}

void testCloseDrainsQueue()
{
	Fixture f;
	for (int i = 0; i < 20; ++i)
	{
		f.send({ sql("SELECT pg_sleep(0.01)") });
	}
	f.conn->close();
	std::vector<QueryPtr> results = f.wait(20);
	bool allOk = true;
	for (const QueryPtr& q : results)
	{
		allOk = allOk && !q->failed();
	}
	CHECK(allOk);
}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "basics", testBasics },
		{ "encoding", testEncoding },
		{ "stale statements", testStaleStatements },
		{ "statement cache limit", testStatementCacheLimit },
		{ "transactions", testTransactions },
		{ "reconnect after idle drop", testReconnectAfterIdleDrop },
		{ "drop during query", testDropDuringQuery },
		{ "close timeout", testCloseTimeout },
		{ "close drains queue", testCloseDrainsQueue },
	};
	for (const auto& test : tests)
	{
		int before = g_failures;
		test.second();
		std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", test.first);
	}
	return g_failures == 0 ? 0 : 1;
}
