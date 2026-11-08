#pragma once

#include "utils/Trampoline.h"

#include "GameABI.h"
#include "MotionHookCode.h"
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

	static inline void Install()
	{
		bhkCharacterStateOnGround::SimulateStatePhysics = bhkCharacterStateOnGround::vTable.write_vfunc(8, SimulateStatePhysics);

		// bhkCharacterController::UpdateStateAndMovement
		{
			static std::uintptr_t hookedAddress = bhkCharacterController::UpdateStateAndMovement.address() + 0x7C2;

			utils::AllocExactSizeTrampoline<5>();
			SKSE::GetTrampoline().write_call<5>(hookedAddress, &hkpCharacterContext::GetCharacterState_Hook);
		}

		///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

		// hkbClipGenerator::Activate
		{
			static std::uintptr_t hookedAddress = hkbClipGenerator::Activate.address() + 0x66E;

			struct Hook : Xbyak::CodeGenerator
			{
				Hook()
				{
					Xbyak::Label hookLabel;
					mov(rdx, r15);	// r15 = hkbContext*
					jmp(ptr[rip + hookLabel]);

					L(hookLabel), dq(reinterpret_cast<std::uintptr_t>(&hkbClipGenerator::ComputeStartTime_Hook));
				}
			};

			hkbClipGenerator::ComputeStartTime = utils::WriteCallTrampoline<5>(hookedAddress, Hook());
		}

		// hkbClipGenerator::Deactivate
		{
			static std::uintptr_t hookedAddress = hkbClipGenerator::Deactivate.address() + 0x1A;

			struct Hook : Xbyak::CodeGenerator
			{
				Hook()
				{
					Xbyak::Label hookLabel;
					mov(rdx, rsi);
					jmp(ptr[rip + hookLabel]);

					L(hookLabel), dq(reinterpret_cast<std::uintptr_t>(&hkbClipGenerator::ResetIgnoreStartTime_Hook));
				}
			};

			hkbClipGenerator::ResetIgnoreStartTime = utils::WriteCallTrampoline<5>(hookedAddress, Hook());
		}

		// Character::ProcessMotionData
		{
			static std::uintptr_t translation1HookedAddress = Character::ProcessMotionData.address() + 0x298;
			static std::uintptr_t translation2HookedAddress = Character::ProcessMotionData.address() + 0x2AA;
			static std::uintptr_t rotation1HookedAddress = Character::ProcessMotionData.address() + 0x35C;
			static std::uintptr_t rotation2HookedAddress = Character::ProcessMotionData.address() + 0x36D;

			static std::array<RUNTIME_FUNCTION, 4> functionTables{};
			const auto install = [](std::uintptr_t a_site, void* a_function, RUNTIME_FUNCTION& a_table) {
				amr::MotionHookCode code(a_function);
				code.ready();
				SKSE::AllocTrampoline(utils::GetTrampolineBaseSize<5>() + code.getSize() + 15);
				const auto allocation = reinterpret_cast<std::uintptr_t>(SKSE::GetTrampoline().allocate(code.getSize() + 15));
				const auto address = (allocation + 15) & ~std::uintptr_t{ 15 };
				std::memcpy(reinterpret_cast<void*>(address), code.getCode(), code.getSize());
				a_table = { 0, code.bodySize, code.unwindOffset };
				if (!RtlAddFunctionTable(&a_table, 1, address)) {
					RE::stl::report_and_fail("Cannot register motion hook unwind information");
				}
				SKSE::GetTrampoline().write_call<5>(a_site, address);
			};

			// Translation
			install(translation1HookedAddress, &MotionDataContainer::ProcessTranslationData_Hook, functionTables[0]);
			install(translation2HookedAddress, &MotionDataContainer::ProcessTranslationData_Hook, functionTables[1]);

			//// Rotation
			install(rotation1HookedAddress, &MotionDataContainer::ProcessRotationData_Hook, functionTables[2]);
			install(rotation2HookedAddress, &MotionDataContainer::ProcessRotationData_Hook, functionTables[3]);
		}
	}
}
