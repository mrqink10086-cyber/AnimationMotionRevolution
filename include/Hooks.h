#pragma once

#include "HookInstallation.h"

#include "GameABI.h"
#include "RE/B/BShkbAnimationGraph.h"
#include "RE/H/hkbClipGenerator.h"
#include "RE/H/hkbContext.h"

#include "RE/M/MotionDataContainer.h"

namespace hooks
{
	class hkbClipGenerator
	{
		static constexpr REL::RelocationID ActivateId{ 58602, 59252 };
		static constexpr REL::RelocationID DeactivateId{ 58604, 59254 };

	public:
		static inline REL::Relocation<std::uintptr_t> Activate{ ActivateId };
		static inline REL::Relocation<void (*)(const RE::hkbClipGenerator*)> ComputeStartTime;
		static void ComputeStartTime_Hook(const RE::hkbClipGenerator* a_this, const RE::hkbContext* a_context);

		static inline REL::Relocation<std::uintptr_t> Deactivate{ DeactivateId };
		static inline REL::Relocation<void (*)(const RE::hkbClipGenerator*)> ResetIgnoreStartTime;
		static void ResetIgnoreStartTime_Hook(const RE::hkbClipGenerator* a_this, const RE::hkbContext* a_hkbContext);

	private:
		static const RE::hkaAnimation* GetBoundAnimation(const RE::hkbClipGenerator* a_this)
		{
			return a_this && a_this->binding ? a_this->binding->animation.get() : nullptr;
		}
	};

	class MotionDataContainer
	{
		static constexpr REL::RelocationID ProcessTranslationDataId{ 31812, 32582 };
		static constexpr REL::RelocationID ProcessRotationDataId{ 31813, 32583 };
		static constexpr REL::RelocationID InterpolateRotationId{ 69459, 70836 };

	public:
		// Translation
		static inline REL::Relocation<void (*)(std::uintptr_t*, float, RE::NiPoint3&)> ProcessTranslationData{ ProcessTranslationDataId };
		static void ProcessTranslationData_Hook(RE::MotionDataContainer* a_this, float a_motionTime, RE::NiPoint3& a_translation,
			const RE::BSFixedString* a_clipName, RE::Character* a_character);

		// Rotation
		static inline REL::Relocation<void (*)(std::uintptr_t*, float, RE::NiQuaternion&)> ProcessRotationData{ ProcessRotationDataId };
		static inline REL::Relocation<void (*)(RE::NiQuaternion&, float, const RE::NiQuaternion&, const RE::NiQuaternion&)> InterpolateRotation{ InterpolateRotationId };
		static void ProcessRotationData_Hook(RE::MotionDataContainer* a_this, float a_motionTime, RE::NiQuaternion& a_rotation,
			const RE::BSFixedString* a_clipName, RE::Character* a_character);

	private:
		static RE::BShkbAnimationGraphPtr GetAnimationGraph(RE::Character* a_character)
		{
			return amr::abi::ActiveGraph(a_character);
		};
	};

	class Character
	{
		static constexpr REL::RelocationID ProcessMotionDataId{ 31949, 32703 };

	public:
		static inline REL::Relocation<std::uintptr_t> ProcessMotionData{ ProcessMotionDataId };
	};

	class bhkCharacterStateOnGround
	{
	public:
		static inline REL::Relocation<std::uintptr_t> vTable{ RE::VTABLE_bhkCharacterStateOnGround[0] };

		static inline REL::Relocation<void (*)(RE::bhkCharacterStateOnGround*, RE::bhkCharacterController*)> SimulateStatePhysics;
	};

	class bhkCharacterController
	{
		static constexpr REL::RelocationID UpdateStateAndMovementId{ 76436, 78275 };

	public:
		static inline REL::Relocation<std::uintptr_t>
			UpdateStateAndMovement{ UpdateStateAndMovementId };
	};

	class hkpCharacterContext
	{
	public:
		static RE::hkpCharacterStateType GetCharacterState_Hook(RE::hkpCharacterContext* a_this);
	};

	void SimulateStatePhysics(RE::bhkCharacterStateOnGround* a_this, RE::bhkCharacterController* a_characterController);
	void ClearMotionCache();
	[[nodiscard]] bool HasAnimMotionData(const RE::hkbCharacter* a_hkbCharacter) noexcept;

	[[nodiscard]] static inline amr::HookInstallation PrepareInstallation()
	{
		return amr::HookInstallation{
			reinterpret_cast<std::uintptr_t>(&hkpCharacterContext::GetCharacterState_Hook),
			reinterpret_cast<void*>(&hkbClipGenerator::ComputeStartTime_Hook),
			reinterpret_cast<void*>(&hkbClipGenerator::ResetIgnoreStartTime_Hook),
			reinterpret_cast<void*>(&MotionDataContainer::ProcessTranslationData_Hook),
			reinterpret_cast<void*>(&MotionDataContainer::ProcessRotationData_Hook)
		};
	}

	static inline void Install(amr::HookInstallation& a_installation)
	{
		static std::array<RUNTIME_FUNCTION, 4> functionTables{};
		const auto originals = a_installation.Install(SKSE::GetTrampoline(), {
			bhkCharacterController::UpdateStateAndMovement.address() + 0x7C2,
			hkbClipGenerator::Activate.address() + 0x66E,
			hkbClipGenerator::Deactivate.address() + 0x1A,
			{ Character::ProcessMotionData.address() + 0x298, Character::ProcessMotionData.address() + 0x2AA,
				Character::ProcessMotionData.address() + 0x35C, Character::ProcessMotionData.address() + 0x36D }
		}, functionTables);
		hkbClipGenerator::ComputeStartTime = originals.activate;
		hkbClipGenerator::ResetIgnoreStartTime = originals.deactivate;
		bhkCharacterStateOnGround::SimulateStatePhysics = bhkCharacterStateOnGround::vTable.write_vfunc(8, SimulateStatePhysics);
	}
}
