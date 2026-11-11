#include "Hooks.h"
#include "PhysicsEvents.h"

#include <algorithm>

namespace hooks
{
    RE::hkpCharacterStateType hkpCharacterContext::GetCharacterState_Hook(RE::hkpCharacterContext* a_this)
    {
        if (!a_this) {
            return RE::hkpCharacterStateType::kOnGround;
        }
        const auto state = amr::abi::GroundState(a_this);
        return !state || state->unk10 ? a_this->currentState : RE::hkpCharacterStateType::kSwimming;
    }

    void SimulateStatePhysics(RE::bhkCharacterStateOnGround* a_this, RE::bhkCharacterController* a_characterController)
    {
        if (!a_this || !a_characterController) {
            return;
        }
        const RE::NiPointer<RE::bhkCharacterController> controllerOwner(a_characterController);
        RE::NiPointer<RE::Actor> character;
        {
            RE::BSSpinLockGuard sourceGuard(a_characterController->lock);
            for (auto sink : a_characterController->sinks) {
                if (std::find(a_characterController->pendingUnregisters.begin(), a_characterController->pendingUnregisters.end(), sink) !=
                    a_characterController->pendingUnregisters.end()) {
                    continue;
                }
                if (const auto candidate = amr::abi::CharacterFromSink(sink)) {
                    character = candidate->GetHandle().get();
                    if (character) {
                        break;
                    }
                }
            }
        }
        if (const auto graph = amr::abi::ActiveGraph(character.get())) {
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* source = graph.get();
            const auto event = amr::MakePhysicsUpdateEvent(character.get());
            source->SendEvent(&event);
        }

        const RE::NiPointer<RE::bhkWorld> world(a_characterController->GetHavokWorld());
        if (!world) {
            return;
        }
        RE::hkVector4 position;
        a_characterController->GetPosition(position, true);
        a_characterController->fallStartHeight = position.quad.m128_f32[2] * 69.991249F;
        RE::CFilter collisionFilterInfo;
        a_characterController->GetCollisionFilterInfo(collisionFilterInfo);
        RE::bhkPickData pickData;
        pickData.rayInput.from = position;
        pickData.rayInput.to = position - RE::hkVector4{ 0.0F, 0.0F, 0.5F, 0.0F };
        pickData.rayInput.filterInfo = collisionFilterInfo;
        world->PickObject(pickData);
        if (pickData.rayOutput.rootCollidable) {
            if (pickData.rayOutput.normal.quad.m128_f32[2] < a_characterController->zNormal) {
                a_characterController->flags.set(RE::CHARACTER_FLAGS::kSupport);
            }
        } else {
            a_characterController->wantState = RE::hkpCharacterStateType::kInAir;
            a_characterController->flags.reset(RE::CHARACTER_FLAGS::kSupport);
        }
        RE::hkVector4 desiredVelocity = a_characterController->velocityMod;
        std::swap(desiredVelocity.quad.m128_f32[0], desiredVelocity.quad.m128_f32[1]);
        amr::abi::MovementInput movementUtil{};
        movementUtil.forward = a_characterController->forwardVec;
        movementUtil.up = RE::hkVector4{ 0.0F, 0.0F, 1.0F, 0.0F };
        movementUtil.surfaceNormal = a_characterController->supportNorm;
        movementUtil.currentVelocity = a_characterController->outVelocity;
        movementUtil.desiredVelocity = desiredVelocity;
        movementUtil.surfaceVelocity = a_characterController->surfaceInfo.surfaceVelocity;
        movementUtil.gain = 1.0F;
        movementUtil.maxVelocityDelta = 500.0F;
        static REL::Relocation<void (*)(const amr::abi::MovementInput&, RE::hkVector4&)> calculateMovement{ REL::ID(81002) };
        calculateMovement(movementUtil, a_characterController->outVelocity);
        a_characterController->outVelocity = a_characterController->outVelocity + a_characterController->surfaceInfo.surfaceVelocity;
        static REL::Relocation<const RE::hkVector4* (*)(const RE::bhkWorld*)> getGravity{ REL::ID(77838) };
        if (const auto gravity = getGravity(world.get())) {
            a_characterController->outVelocity = a_characterController->outVelocity + *gravity * a_characterController->stepInfo.deltaTime;
        }
        if (a_characterController->wantState != RE::hkpCharacterStateType::kTotal) {
            a_characterController->context.currentState = a_characterController->wantState;
            a_characterController->wantState = RE::hkpCharacterStateType::kTotal;
        }
        const auto stateType = a_characterController->context.currentState;
        if (stateType != RE::hkpCharacterStateType::kOnGround) {
            const auto state = skyrim_cast<RE::bhkCharacterState*>(amr::abi::RegisteredState(&a_characterController->context, stateType));
            amr::abi::SimulateStatePhysics(state, a_characterController);
        }
    }
}
