#include "component.hpp"

#include <cstdarg>
#include <cstdio>

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
	// GMX: the gamemode reconnects in OnGameModeInit, so don't leak the old handles.
	closeAll();
}

void PostgresComponent::closeAll()
{
	// Closing waits for each connection's queue to drain (pending saves).
	connections_.clear();
	pendingQueries_.clear();
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
}

uint64_t PostgresComponent::generationOf(IPawnScript* script) const
{
	auto it = scripts_.find(script);
	return it == scripts_.end() ? 0 : it->second;
}

int PostgresComponent::connect(const std::string& conninfo, std::string& error)
{
	PGconn* conn = Connection::open(conninfo, error);
	if (conn == nullptr)
	{
		error = trimmed(error);
		return 0;
	}
	int id = nextConnectionId_++;
	connections_.emplace(id, std::make_unique<Connection>(id, conn, completions_));
	return id;
}

bool PostgresComponent::disconnect(int id)
{
	return connections_.erase(id) > 0;
}

Connection* PostgresComponent::connection(int id)
{
	auto it = connections_.find(id);
	return it == connections_.end() ? nullptr : it->second.get();
}

int PostgresComponent::newPendingQuery(int connectionId, IPawnScript* owner, std::string sql)
{
	int id = nextPendingQueryId_++;
	PendingQuery& q = pendingQueries_[id];
	q.connectionId = connectionId;
	q.owner = owner;
	q.sql = std::move(sql);
	return id;
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
		logError("[postgres] Query failed (%s): %s | callback: %s | query: %s", query.sqlState.c_str(),
			error.c_str(), query.callback.empty() ? "-" : query.callback.c_str(), query.sql.c_str());
		if (scriptAlive)
		{
			script->Call("OnPgError", DefaultReturnValue_True, query.connectionId, StringView(query.sqlState),
				StringView(error), StringView(query.callback), StringView(query.sql));
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
	cell heap = script->GetHEA();
	int err = AMX_ERR_NONE;
	for (auto it = query.args.rbegin(); it != query.args.rend() && err == AMX_ERR_NONE; ++it)
	{
		err = it->isString
			? script->PushString(nullptr, nullptr, StringView(it->str), false, false)
			: script->Push(it->cell);
	}

	activeResult_ = query.result;
	cell ret = 0;
	if (err == AMX_ERR_NONE)
	{
		err = script->Exec(&ret, index);
	}
	activeResult_ = nullptr;
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
