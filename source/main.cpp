#include "Hooks.h"
#include "Settings.h"
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
#include "TrueHUDIntegration.h"
#endif

#include "utils/Logger.h"

namespace
{
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
	class DebugVisualizationInputSink final : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static DebugVisualizationInputSink* GetSingleton()
		{
			static DebugVisualizationInputSink singleton;
			return std::addressof(singleton);
		}

		RE::BSEventNotifyControl ProcessEvent(
			RE::InputEvent* const* a_events,
			RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_events) {
				return RE::BSEventNotifyControl::kContinue;
			}

			for (auto* event = *a_events; event; event = event->next) {
				if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
					event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
					continue;
				}
				const auto* button = event->AsButtonEvent();
				if (!button || !button->IsDown()) {
					continue;
				}

				const auto key = button->GetIDCode();
				if (key == static_cast<std::uint32_t>(RE::BSKeyboardDevice::Keys::kPageUp)) {
					const bool visible = truehud::ToggleMotionWarpVisibility();
					logger::info(
						"[AMR-DIAG][TrueHUD] Page Up: motion-warp visualization {}",
						visible ? "shown" : "hidden");
				} else if (key == static_cast<std::uint32_t>(
									  RE::BSKeyboardDevice::Keys::kPageDown)) {
					const bool visible = truehud::ToggleLedgeProtectionVisibility();
					logger::info(
						"[AMR-DIAG][TrueHUD] Page Down: ledge-protection visualization {}",
						visible ? "shown" : "hidden");
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
#endif

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		if (a_message->type == SKSE::MessagingInterface::kPreLoadGame ||
			a_message->type == SKSE::MessagingInterface::kNewGame) {
			hooks::ClearMotionCache();
		}
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
		if (a_message && a_message->type == SKSE::MessagingInterface::kPostLoad) {
			truehud::Initialize();
		}
		if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
			if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
				input->AddEventSink(DebugVisualizationInputSink::GetSingleton());
				logger::info("[AMR-DIAG][TrueHUD] Page Up/Page Down visualization input registered");
			}
		}
#else
		(void)a_message;
#endif
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	if (a_skse->IsEditor() || a_skse->RuntimeVersion() != REL::Version{ 1, 6, 1170, 0 } ||
		a_skse->SKSEVersion() < REL::Version{ 2, 2, 6, 0 }.pack()) {
		return false;
	}

	const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
	if (!logger::init(plugin->GetName())) {
		return false;
	}

	logger::info("Loading {} {}...", plugin->GetName(), plugin->GetVersion().string());

	auto installation = hooks::PrepareInstallation();
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = installation.RequiredSize() });
	if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE", OnSKSEMessage)) {
		logger::critical("Failed to register SKSE messaging listener");
		return false;
	}
	settings::Init("AnimationMotionRevolution.ini");
	logger::set_level(settings::debug::logLevel, settings::debug::logLevel);
	logger::info("Author branch b5be808 synchronized; runtime {}", REL::Module::get().version().string());

	hooks::Install(installation);
	logger::info("Trampoline installed: {} / {} bytes", SKSE::GetTrampoline().allocated_size(), SKSE::GetTrampoline().capacity());

	logger::info("Successfully loaded");
	return true;
}
