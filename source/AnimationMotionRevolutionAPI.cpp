#include "AnimationMotionRevolutionAPI.h"

#include "Hooks.h"

namespace
{
	class AMRInterface final : public AMR_API::IVAMR1
	{
	public:
		[[nodiscard]] bool HasAnimMotionData(
			const RE::hkbCharacter* a_hkbCharacter) const noexcept override
		{
			return hooks::HasAnimMotionData(a_hkbCharacter);
		}
	};

	AMRInterface g_amrInterface;
}

extern "C" DLLEXPORT void* RequestPluginAPI(AMR_API::InterfaceVersion a_interfaceVersion) noexcept
{
	switch (a_interfaceVersion) {
	case AMR_API::InterfaceVersion::V1:
		return std::addressof(g_amrInterface);
	default:
		return nullptr;
	}
}
