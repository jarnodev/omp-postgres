#pragma once

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include "connection.hpp"

#include <map>
#include <unordered_map>

// A query being built from Pawn (pg_new_query + pg_bind_*), not yet sent.
struct PendingQuery
{
	int connectionId = 0;
	IPawnScript* owner = nullptr;
	Statement statement;
};

// A transaction being built from Pawn (pg_tx_begin + pg_tx_add), not yet committed.
struct PendingTransaction
{
	int connectionId = 0;
	IPawnScript* owner = nullptr;
	std::vector<Statement> statements;
};

class PostgresComponent final : public IComponent, public CoreEventHandler, public PawnEventHandler
{
public:
	// Generated once for this component; never change it.
	PROVIDE_UID(0xC0FFEE5A11D0B2E7);

	static PostgresComponent* instance() { return instance_; }

	PostgresComponent();
	~PostgresComponent();

	// IComponent
	StringView componentName() const override { return "Postgres"; }
	SemanticVersion componentVersion() const override
	{
		return SemanticVersion(OMP_POSTGRES_VERSION_MAJOR, OMP_POSTGRES_VERSION_MINOR, OMP_POSTGRES_VERSION_PATCH, 0);
	}
	void onLoad(ICore* core) override;
	void onInit(IComponentList* components) override;
	void onFree(IComponent* component) override;
	void reset() override;
	void free() override { delete this; }

	// CoreEventHandler: delivers finished queries to Pawn on the main thread.
	void onTick(Microseconds elapsed, TimePoint now) override;

	// PawnEventHandler
	void onAmxLoad(IPawnScript& script) override;
	void onAmxUnload(IPawnScript& script) override;

	ICore* core() { return core_; }
	IPawnScript* scriptFor(AMX* amx) { return pawn_ ? pawn_->getScript(amx) : nullptr; }
	uint64_t generationOf(IPawnScript* script) const;

	// Connections. Ids start at 1 so 0 can mean "invalid" in Pawn. A connection
	// belongs to the script that opened it and is closed when that script unloads.
	int connect(IPawnScript* owner, const std::string& conninfo, std::string& error);
	bool disconnect(int id);
	Connection* connection(int id);

	// Queries under construction.
	int newPendingQuery(int connectionId, IPawnScript* owner, std::string sql);
	PendingQuery* pendingQuery(int id);
	void dropPendingQuery(int id);
	int newPendingTransaction(int connectionId, IPawnScript* owner);
	PendingTransaction* pendingTransaction(int id);
	void dropPendingTransaction(int id);
	void send(QueryPtr query);

	// Results of the query whose callback is currently running, if any. A
	// transaction has one per statement; pg_select_result picks which one the
	// pg_get_* natives read (the last one by default).
	const PGresult* activeResult() const;
	int activeResultCount() const;
	bool selectResult(int index);

	void logError(const char* fmt, ...);

private:
	void deliver(Query& query);
	void closeAll();
	void checkPendingLeak();

	static PostgresComponent* instance_;

	ICore* core_ = nullptr;
	IPawnComponent* pawn_ = nullptr;

	CompletionQueue completions_;
	struct OwnedConnection
	{
		IPawnScript* owner = nullptr;
		std::unique_ptr<Connection> connection;
	};
	std::map<int, OwnedConnection> connections_;
	uint32_t nextConnectionId_ = 1;

	std::unordered_map<int, PendingQuery> pendingQueries_;
	uint32_t nextPendingQueryId_ = 1;

	std::unordered_map<int, PendingTransaction> pendingTransactions_;
	uint32_t nextPendingTransactionId_ = 1;
	// Warn when this many queries/transactions were built but never sent or discarded.
	size_t pendingWarnAt_ = 1000;

	std::unordered_map<IPawnScript*, uint64_t> scripts_;
	uint64_t nextGeneration_ = 1;

	const Query* activeQuery_ = nullptr;
	int activeIndex_ = 0;
};

// Defined in natives.cpp.
void registerNatives(IPawnScript& script);
