#include "production_storage.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace ProductionViewer::Storage
{
	namespace
	{
		// UCrGameInstance::ServerSessionName — offset within UCrGameInstance,
		// not exposed by the generated SDK (folded into trailing padding bytes).
		constexpr std::ptrdiff_t kServerSessionNameOffset = 0x250;

		std::mutex     g_mutex;
		std::string    g_dataDir;     // <Plugins>\<pluginName>
		std::string    g_sessionName;
		nlohmann::json g_data;
		bool           g_loaded = false;

		// The plugin DLL's own directory — used as the base for the per-plugin
		// data folder. Resolved via the address of this function rather than a
		// stored DllMain HMODULE.
		std::string GetModuleDirectory()
		{
			HMODULE module = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&GetModuleDirectory), &module);

			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);

			return std::filesystem::path(path).parent_path().string();
		}

		// Replaces filesystem-unsafe characters so a session name can be used
		// directly as a file name.
		std::string SanitizeFileName(const std::string& name)
		{
			std::string result = name;
			for (char& c : result)
			{
				switch (c)
				{
					case '<': case '>': case ':': case '"':
					case '/': case '\\': case '|': case '?': case '*':
						c = '_';
						break;
					default:
						break;
				}
			}
			return result.empty() ? "Default" : result;
		}

		std::string GetDataPath()
		{
			return g_dataDir + "\\" + SanitizeFileName(g_sessionName) + ".json";
		}

		// Must run on the game thread — touches UWorld/UObject state.
		std::string ResolveSessionName()
		{
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
				{
					LOG_DEBUG("Storage: ResolveSessionName - no world loaded.");
					return "";
				}

				SDK::UCrGameInstance* gameInstance = reinterpret_cast<SDK::UCrGameInstance*>(SDK::UGameplayStatics::GetGameInstance(world));
				if (!gameInstance)
				{
					LOG_DEBUG("Storage: ResolveSessionName - no game instance.");
					return "";
				}

				// UCrGameInstance::ServerSessionName — not present in the generated SDK
				// (folded into the trailing padding bytes), but populated on load even
				// for single-player sessions. Read directly via its known offset.
				auto* serverSessionName = reinterpret_cast<SDK::FString*>(
					reinterpret_cast<std::uint8_t*>(gameInstance) + kServerSessionNameOffset);

				std::string sessionName = serverSessionName->ToString();
				LOG_DEBUG("Storage: ResolveSessionName resolved '%s'.", sessionName.c_str());
				return sessionName;
			}
			catch (const std::exception& e)
			{
				LOG_WARN("Storage: ResolveSessionName threw: %s", e.what());
				return "";
			}
			catch (...)
			{
				LOG_WARN("Storage: ResolveSessionName threw an unknown exception.");
				return "";
			}
		}

		// Splits a dot-separated path into an RFC 6901 JSON pointer.
		nlohmann::json::json_pointer ToJsonPointer(const std::string& path)
		{
			std::string pointer;
			pointer.reserve(path.size() + 1);

			for (char c : path)
				pointer += (c == '.') ? '/' : c;

			return nlohmann::json::json_pointer("/" + pointer);
		}
	}

	void Initialize(IPluginSelf* self)
	{
		const std::string pluginName = (self && self->name) ? self->name : "ProductionViewer";
		const std::string moduleDir = GetModuleDirectory();
		g_dataDir = moduleDir + "\\" + pluginName;

		LOG_DEBUG("Storage: module directory '%s'.", moduleDir.c_str());
		LOG_DEBUG("Storage: data directory '%s'.", g_dataDir.c_str());

		std::error_code ec;
		std::filesystem::create_directories(g_dataDir, ec);
		if (ec)
			LOG_WARN("Storage: failed to create data directory '%s': %s", g_dataDir.c_str(), ec.message().c_str());
		else
			LOG_DEBUG("Storage: data directory ready '%s'.", g_dataDir.c_str());
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		LOG_DEBUG("Storage: shutting down - discarding session '%s'.", g_sessionName.c_str());
		g_data.clear();
		g_sessionName.clear();
		g_loaded = false;
	}

	bool Reload()
	{
		std::string sessionName = ResolveSessionName();
		if (sessionName.empty())
		{
			LOG_DEBUG("Storage: no active session - data not loaded.");
			std::lock_guard<std::mutex> lock(g_mutex);
			g_loaded = false;
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		g_sessionName = std::move(sessionName);
		g_data = nlohmann::json::object();

		const std::string dataPath = GetDataPath();
		LOG_DEBUG("Storage: resolved session name '%s'.", g_sessionName.c_str());
		LOG_DEBUG("Storage: reading data file '%s'.", dataPath.c_str());

		std::ifstream file(dataPath);
		if (file.is_open())
		{
			try
			{
				file >> g_data;
				LOG_DEBUG("Storage: parsed data file '%s'.", dataPath.c_str());
			}
			catch (...)
			{
				LOG_WARN("Storage: failed to parse '%s' - starting with empty data.", dataPath.c_str());
				g_data = nlohmann::json::object();
			}
		}
		else
		{
			LOG_DEBUG("Storage: no existing data file at '%s' - starting with empty data.", dataPath.c_str());
		}

		g_loaded = true;
		LOG_INFO("Storage: loaded session '%s' (%s).", g_sessionName.c_str(), dataPath.c_str());
		return true;
	}

	bool IsLoaded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_loaded;
	}

	std::string GetSessionName()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_sessionName;
	}

	nlohmann::json Get(const std::string& path, const nlohmann::json& defaultValue)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded)
			return defaultValue;

		const auto pointer = ToJsonPointer(path);
		if (!g_data.contains(pointer))
			return defaultValue;

		return g_data.at(pointer);
	}

	void Set(const std::string& path, const nlohmann::json& value)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded)
			return;

		g_data[ToJsonPointer(path)] = value;
	}

	void Save()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded)
			return;

		const std::string dataPath = GetDataPath();
		std::ofstream file(dataPath);
		if (file.is_open())
			file << g_data.dump(2);
		else
			LOG_WARN("Storage: failed to write '%s'.", dataPath.c_str());
	}
}
