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
	std::string sql;
	std::vector<QueryParam> params;
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

	// Connections. Ids start at 1 so 0 can mean "invalid" in Pawn.
	int connect(const std::string& conninfo, std::string& error);
	bool disconnect(int id);
	Connection* connection(int id);

	// Queries under construction.
	int newPendingQuery(int connectionId, IPawnScript* owner, std::string sql);
	PendingQuery* pendingQuery(int id);
	void dropPendingQuery(int id);
	void send(QueryPtr query);

	// Result of the query whose callback is currently running, if any.
	const PGresult* activeResult() const { return activeResult_; }

	void logError(const char* fmt, ...);

private:
	void deliver(Query& query);
	void closeAll();

	static PostgresComponent* instance_;

	ICore* core_ = nullptr;
	IPawnComponent* pawn_ = nullptr;

	CompletionQueue completions_;
	std::map<int, std::unique_ptr<Connection>> connections_;
	int nextConnectionId_ = 1;

	std::unordered_map<int, PendingQuery> pendingQueries_;
	int nextPendingQueryId_ = 1;

	std::unordered_map<IPawnScript*, uint64_t> scripts_;
	uint64_t nextGeneration_ = 1;

	const PGresult* activeResult_ = nullptr;
};

// Defined in natives.cpp.
void registerNatives(IPawnScript& script);
