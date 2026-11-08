#pragma once

namespace amr::abi
{
	struct alignas(16) MovementInput
	{
		RE::hkVector4 forward;
		RE::hkVector4 up;
		RE::hkVector4 surfaceNormal;
		RE::hkVector4 currentVelocity;
		RE::hkVector4 desiredVelocity;
		RE::hkVector4 surfaceVelocity;
		float gain;
		float maxVelocityDelta;
	};
	static_assert(sizeof(MovementInput) == 0x70);
	static_assert(offsetof(MovementInput, gain) == 0x60);
	static_assert(offsetof(RE::bhkCharacterController, context) == 0x1E0);
	static_assert(offsetof(RE::bhkCharacterController, wantState) == 0x21C);
	static_assert(offsetof(RE::hkbClipGenerator, binding) == 0xA0);
	static_assert(offsetof(RE::hkbClipGenerator, localTime) == 0xF0);

	inline RE::BShkbAnimationGraphPtr ActiveGraph(RE::Actor* a_actor)
	{
		RE::BSAnimationGraphManagerPtr manager;
		if (!a_actor || !a_actor->GetAnimationGraphManager(manager) || !manager) {
			return {};
		}
		RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
		const auto index = manager->GetRuntimeData().activeGraph;
		return index < manager->graphs.size() ? manager->graphs[index] : RE::BShkbAnimationGraphPtr{};
	}

	inline RE::Character* CharacterFromSink(RE::BSTEventSink<RE::bhkCharacterMoveFinishEvent>* a_sink)
	{
		if (!a_sink) {
			return nullptr;
		}
		const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_sink);
		if (!vtable) {
			return nullptr;
		}
		const auto locator = *reinterpret_cast<const RE::RTTI::CompleteObjectLocator* const*>(vtable - sizeof(void*));
		if (!locator || !locator->typeDescriptor.get()) {
			return nullptr;
		}
		static REL::Relocation<void*> target{ RE::Character::RTTI };
		return static_cast<RE::Character*>(RE::RTDynamicCast(a_sink, locator->offset, locator->typeDescriptor.get(), target.get(), false));
	}

	inline RE::hkpCharacterState* RegisteredState(RE::hkpCharacterContext* a_context, RE::hkpCharacterStateType a_type)
	{
		const auto index = static_cast<std::uint32_t>(a_type);
		return a_context && a_context->stateManager && index < RE::hkpCharacterStateType::kTotal ?
				   a_context->stateManager->registeredState[index] :
				   nullptr;
	}

	inline RE::bhkCharacterStateOnGround* GroundState(RE::hkpCharacterContext* a_context)
	{
		return skyrim_cast<RE::bhkCharacterStateOnGround*>(RegisteredState(a_context, RE::hkpCharacterStateType::kOnGround));
	}

	inline void SimulateStatePhysics(RE::bhkCharacterState* a_state, RE::bhkCharacterController* a_controller)
	{
		if (!a_state || !a_controller) {
			return;
		}
		using function_type = void (*)(RE::bhkCharacterState*, RE::bhkCharacterController*);
		const auto vtable = *reinterpret_cast<const function_type* const*>(a_state);
		vtable[0x08](a_state, a_controller);
	}
}
