#include "component.hpp"

#include <cstdarg>
#include <cstdio>
#include <unordered_map>

PostgresComponent* PostgresComponent::instance_ = nullptr;

namespace
{
// Postgres messages end in a newline, which looks odd in the server log.
std::string trimmed(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
	{
		s.pop_back();
	}
	return s;
}

// Handles are 32-bit Pawn cells. The counter wraps after 2^32 handles (weeks on a
// busy server), skipping 0, which scripts treat as invalid, and ids still in use.
template <typename Map>
int allocateId(uint32_t& next, const Map& used)
{
	int id;
	do
	{
		id = static_cast<int>(next++);
	} while (id == 0 || used.count(id) != 0);
	return id;
}
}

PostgresComponent::PostgresComponent()
{
	instance_ = this;
}

PostgresComponent::~PostgresComponent()
{
	closeAll();
	if (pawn_)
	{
		pawn_->getEventDispatcher().removeEventHandler(this);
	}
	if (core_)
	{
		core_->getEventDispatcher().removeEventHandler(this);
	}
	instance_ = nullptr;
}

void PostgresComponent::onLoad(ICore* core)
{
	core_ = core;
	core_->getEventDispatcher().addEventHandler(this);
	core_->printLn("Postgres component loaded (libpq %d).", PQlibVersion());
}

void PostgresComponent::onInit(IComponentList* components)
{
	pawn_ = components->queryComponent<IPawnComponent>();
	if (pawn_ == nullptr)
	{
		core_->logLn(LogLevel::Error, "[postgres] Pawn component not found; natives are unavailable.");
		return;
	}
	pawn_->getEventDispatcher().addEventHandler(this);
}

void PostgresComponent::onFree(IComponent* component)
{
	if (component == pawn_)
	{
		pawn_ = nullptr;
		scripts_.clear();
	}
}

void PostgresComponent::reset()
{
	// GMX: the gamemode's connections were already closed when it unloaded;
	// filterscripts stay loaded and keep theirs.
}

void PostgresComponent::closeAll()
{
	// Closing waits for each connection's queue to drain (pending saves).
	connections_.clear();
	pendingQueries_.clear();
	pendingTransactions_.clear();
	// Callbacks of what just finished have nobody left to receive them.
	completions_.takeAll();
}

void PostgresComponent::onAmxLoad(IPawnScript& script)
{
	scripts_[&script] = nextGeneration_++;
	registerNatives(script);
}

void PostgresComponent::onAmxUnload(IPawnScript& script)
{
	scripts_.erase(&script);
	for (auto it = pendingQueries_.begin(); it != pendingQueries_.end();)
	{
		it = it->second.owner == &script ? pendingQueries_.erase(it) : std::next(it);
	}
	for (auto it = pendingTransactions_.begin(); it != pendingTransactions_.end();)
	{
		it = it->second.owner == &script ? pendingTransactions_.erase(it) : std::next(it);
	}
	// Erasing waits for each connection's queue to drain, so saves sent from
	// OnGameModeExit/OnFilterScriptExit still land.
	for (auto it = connections_.begin(); it != connections_.end();)
	{
		it = it->second.owner == &script ? connections_.erase(it) : std::next(it);
	}
}

uint64_t PostgresComponent::generationOf(IPawnScript* script) const
{
	auto it = scripts_.find(script);
	return it == scripts_.end() ? 0 : it->second;
}

int PostgresComponent::connect(IPawnScript* owner, const std::string& conninfo, std::string& error)
{
	PGconn* conn = Connection::open(conninfo, error);
	if (conn == nullptr)
	{
		error = trimmed(error);
		return 0;
	}
	int id = allocateId(nextConnectionId_, connections_);
	connections_.emplace(id, OwnedConnection { owner, std::make_unique<Connection>(id, conn, completions_) });
	return id;
}

bool PostgresComponent::disconnect(int id)
{
	return connections_.erase(id) > 0;
}

Connection* PostgresComponent::connection(int id)
{
	auto it = connections_.find(id);
	return it == connections_.end() ? nullptr : it->second.connection.get();
}

int PostgresComponent::newPendingQuery(int connectionId, IPawnScript* owner, std::string sql)
{
	int id = allocateId(nextPendingQueryId_, pendingQueries_);
	PendingQuery& q = pendingQueries_[id];
	q.connectionId = connectionId;
	q.owner = owner;
	q.statement.sql = std::move(sql);
	q.statement.prepare = true;
	checkPendingLeak();
	return id;
}

void PostgresComponent::checkPendingLeak()
{
	size_t count = pendingQueries_.size() + pendingTransactions_.size();
	if (count < pendingWarnAt_)
	{
		return;
	}
	pendingWarnAt_ *= 2;
	// The SQL built most often is the likeliest culprit.
	std::unordered_map<std::string, size_t> bySql;
	const std::string* worst = nullptr;
	size_t worstCount = 0;
	for (const auto& entry : pendingQueries_)
	{
		size_t n = ++bySql[entry.second.statement.sql];
		if (n > worstCount)
		{
			worstCount = n;
			worst = &entry.second.statement.sql;
		}
	}
	logError("[postgres] %zu queries/transactions were built but never sent or discarded (missing pg_send, "
			 "pg_discard or pg_tx_commit?). Most common: %zu x \"%s\"",
		count, worstCount, worst ? worst->c_str() : "-");
}

PendingQuery* PostgresComponent::pendingQuery(int id)
{
	auto it = pendingQueries_.find(id);
	return it == pendingQueries_.end() ? nullptr : &it->second;
}

void PostgresComponent::dropPendingQuery(int id)
{
	pendingQueries_.erase(id);
}

int PostgresComponent::newPendingTransaction(int connectionId, IPawnScript* owner)
{
	int id = allocateId(nextPendingTransactionId_, pendingTransactions_);
	PendingTransaction& tx = pendingTransactions_[id];
	tx.connectionId = connectionId;
	tx.owner = owner;
	checkPendingLeak();
	return id;
}

PendingTransaction* PostgresComponent::pendingTransaction(int id)
{
	auto it = pendingTransactions_.find(id);
	return it == pendingTransactions_.end() ? nullptr : &it->second;
}

void PostgresComponent::dropPendingTransaction(int id)
{
	pendingTransactions_.erase(id);
}

const PGresult* PostgresComponent::activeResult() const
{
	if (activeQuery_ == nullptr || activeIndex_ < 0 || activeIndex_ >= activeResultCount())
	{
		return nullptr;
	}
	return activeQuery_->results[activeIndex_];
}

int PostgresComponent::activeResultCount() const
{
	return activeQuery_ ? static_cast<int>(activeQuery_->results.size()) : 0;
}

bool PostgresComponent::selectResult(int index)
{
	if (activeQuery_ == nullptr || index < 0 || index >= activeResultCount())
	{
		return false;
	}
	activeIndex_ = index;
	return true;
}

void PostgresComponent::send(QueryPtr query)
{
	Connection* conn = connection(query->connectionId);
	if (conn)
	{
		conn->enqueue(std::move(query));
	}
}

void PostgresComponent::onTick(Microseconds, TimePoint)
{
	for (QueryPtr& query : completions_.takeAll())
	{
		deliver(*query);
	}
}

void PostgresComponent::deliver(Query& query)
{
	IPawnScript* script = query.script;
	bool scriptAlive = script && generationOf(script) == query.scriptGeneration;

	if (query.failed())
	{
		std::string error = trimmed(query.error);
		// OnPgError returns 0 when the script handled the error and doesn't want it logged.
		bool log = true;
		if (scriptAlive)
		{
			log = script->Call("OnPgError", DefaultReturnValue_True, query.connectionId, StringView(query.sqlState),
					  StringView(error), StringView(query.callback), StringView(query.failedSql))
				!= 0;
		}
		if (log)
		{
			logError("[postgres] %s failed (%s): %s | callback: %s | query: %s",
				query.transaction ? "Transaction" : "Query", query.sqlState.c_str(), error.c_str(),
				query.callback.empty() ? "-" : query.callback.c_str(), query.failedSql.c_str());
		}
		return;
	}

	if (!scriptAlive || query.callback.empty())
	{
		return;
	}

	int index;
	if (script->FindPublic(query.callback.c_str(), &index) != AMX_ERR_NONE)
	{
		logError("[postgres] Callback \"%s\" does not exist (missing forward/public?).", query.callback.c_str());
		return;
	}

	// Pawn pushes arguments right to left.
	AMX* amx = script->GetAMX();
	cell heap = script->GetHEA();
	cell stack = amx->stk;
	int paramCount = amx->paramcount;
	int err = AMX_ERR_NONE;
	for (auto it = query.args.rbegin(); it != query.args.rend() && err == AMX_ERR_NONE; ++it)
	{
		err = it->isString
			? script->PushString(nullptr, nullptr, StringView(it->str), false, false)
			: script->Push(it->cell);
	}

	activeQuery_ = &query;
	activeIndex_ = static_cast<int>(query.results.size()) - 1;
	cell ret = 0;
	if (err == AMX_ERR_NONE)
	{
		err = script->Exec(&ret, index);
	}
	else
	{
		// Exec would have popped them; otherwise the script's next call receives them.
		amx->stk = stack;
		amx->paramcount = paramCount;
	}
	activeQuery_ = nullptr;
	script->Release(heap);

	if (err != AMX_ERR_NONE)
	{
		script->PrintError(err);
	}
}

void PostgresComponent::logError(const char* fmt, ...)
{
	if (core_ == nullptr)
	{
		return;
	}
	va_list args;
	va_start(args, fmt);
	core_->vlogLn(LogLevel::Error, fmt, args);
	va_end(args);
}

COMPONENT_ENTRY_POINT()
{
	return new PostgresComponent();
}
