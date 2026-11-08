#include "Hooks.h"
#include "Settings.h"

#include "utils/Logger.h"

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	if (a_skse->RuntimeVersion() != REL::Version{ 1, 6, 1170, 0 } || a_skse->SKSEVersion() < REL::Version{ 2, 2, 6, 0 })
	{
		return false;
	}

	const SKSE::PluginDeclaration* plugin = SKSE::PluginDeclaration::GetSingleton();

	if (!logger::init(plugin->GetName()))
	{
		return false;
	}

	logger::info("Loading {} {}...", plugin->GetName(), plugin->GetVersion().string());

	SKSE::Init(a_skse);

	settings::Init(std::string(plugin->GetName()) + ".ini");

	logger::set_level(settings::debug::logLevel, settings::debug::logLevel);

	if (!SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
		if (message->type == SKSE::MessagingInterface::kPreLoadGame ||
			message->type == SKSE::MessagingInterface::kNewGame) {
			hooks::ClearMotionCache();
		}
	})) {
		logger::error("Cannot register motion cache lifecycle listener");
		return false;
	}

	hooks::Install();

	logger::set_level(logger::level::info, logger::level::info);
	logger::info("Succesfully loaded!");

	logger::set_level(settings::debug::logLevel, settings::debug::logLevel);

	return true;
}
