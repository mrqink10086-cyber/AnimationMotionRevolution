#include "Hooks.h"

#include "AnimMotionHandler.h"
#include "MotionCache.h"
#include "utils/Logger.h"

namespace hooks
{
    namespace
    {
        struct ParsedAnimation
        {
            explicit ParsedAnimation(const RE::hkaAnimation* a_animation) : animation(a_animation) {}

            RE::hkRefPtr<const RE::hkaAnimation> animation;
            AnimMotionData motion;
        };

        using Cache = amr::MotionCache<ParsedAnimation, RE::hkRefPtr<const RE::hkbBehaviorGraph>>;

        Cache& GetCache()
        {
            static Cache cache;
            return cache;
        }

        std::string_view ClipName(const RE::BSFixedString& a_name)
        {
            return { a_name.c_str(), a_name.length() };
        }

        std::string_view ClipName(const RE::hkStringPtr& a_name)
        {
            const auto text = a_name.c_str();
            return text ? std::string_view(text) : std::string_view{};
        }

        Cache::Snapshot ParseAnimation(const RE::hkaAnimation* a_animation)
        {
            auto result = std::make_shared<ParsedAnimation>(a_animation);
            for (const auto& track : a_animation->annotationTracks) {
                for (const auto& annotation : track.annotations) {
                    auto parsed = ParseAnnotation(annotation);
                    if (const auto translation = std::get_if<Translation>(&parsed)) {
                        result->motion.Add(translation);
                    } else if (const auto rotation = std::get_if<Rotation>(&parsed)) {
                        result->motion.Add(rotation);
                    }
                }
                if (!result->motion.translationList.empty() || !result->motion.rotationList.empty()) {
                    break;
                }
            }
            result->motion.Finalize();
            return result;
        }
    }

    void ClearMotionCache()
    {
        GetCache().Clear();
    }

    void hkbClipGenerator::ComputeStartTime_Hook(const RE::hkbClipGenerator* a_this, const RE::hkbContext* a_context)
    {
        auto& cache = GetCache();
        const auto generation = cache.GetGeneration();
        const auto animation = GetBoundAnimation(a_this);
        const auto character = a_context ? a_context->character : nullptr;
        if (animation && character) {
            try {
                auto data = cache.FindParsed(animation);
                if (!data) {
                    data = ParseAnimation(animation);
                    const auto& motion = data->motion;
                    if (!motion.translationList.empty() && motion.translationList.back().time != animation->duration) {
                        logger::warn("Animation={} ends at {}, while custom translation ends at {}",
                            a_this->animationName.c_str(), animation->duration, motion.translationList.back().time);
                    }
                    if (!motion.rotationList.empty() && motion.rotationList.back().time != animation->duration) {
                        logger::warn("Animation={} ends at {}, while custom rotation ends at {}",
                            a_this->animationName.c_str(), animation->duration, motion.rotationList.back().time);
                    }
                    data = cache.Remember(animation, std::move(data), generation);
                }
                if (data) {
                    RE::hkRefPtr<const RE::hkbBehaviorGraph> graph(character->behaviorGraph.get());
                    const auto graphIdentity = graph.get();
                    cache.Activate(character, graphIdentity, std::move(graph), ClipName(a_this->name),
                        animation, std::move(data), generation, a_this);
                }
            } catch (const std::exception& error) {
                logger::error("Cannot cache animation {}: {}", a_this->animationName.c_str(), error.what());
            }
        }
        ComputeStartTime(a_this);
    }

    void hkbClipGenerator::ResetIgnoreStartTime_Hook(const RE::hkbClipGenerator* a_this, const RE::hkbContext* a_context)
    {
        auto& cache = GetCache();
        const auto generation = cache.GetGeneration();
        const auto animation = GetBoundAnimation(a_this);
        const auto character = a_context ? a_context->character : nullptr;
        if (animation && character) {
            cache.Deactivate(character, character->behaviorGraph.get(), ClipName(a_this->name), animation, generation, a_this);
        }
        ResetIgnoreStartTime(a_this);
    }

    void MotionDataContainer::ProcessTranslationData_Hook(RE::MotionDataContainer* a_this, float a_motionTime,
        RE::NiPoint3& a_translation, const RE::BSFixedString* a_clipName, RE::Character* a_character)
    {
        const auto graph = GetAnimationGraph(a_character);
        const auto character = graph ? &graph->characterInstance : nullptr;
        const auto data = character && a_clipName ?
            GetCache().Lookup(character, character->behaviorGraph.get(), ClipName(*a_clipName)) : Cache::Snapshot{};
        if (data && data->motion.SampleTranslation(a_motionTime, a_translation)) {
            const auto controller = a_character->GetCharController();
            if (controller) {
                if (const auto state = amr::abi::GroundState(&controller->context)) {
                    state->unk10 = a_translation.z == 0.0F;
                }
            }
            return;
        }
        if (a_this && (a_this->translationDataPtr & ~std::uintptr_t{ 1 }) &&
            a_this->translationSegCount > static_cast<std::uint32_t>(a_this->IsTranslationDataAligned())) {
            ProcessTranslationData(&a_this->translationDataPtr, a_motionTime, a_translation);
        } else {
            a_translation = RE::NiPoint3{ 0.0F, 0.0F, 0.0F };
        }
    }

    void MotionDataContainer::ProcessRotationData_Hook(RE::MotionDataContainer* a_this, float a_motionTime,
        RE::NiQuaternion& a_rotation, const RE::BSFixedString* a_clipName, RE::Character* a_character)
    {
        const auto graph = GetAnimationGraph(a_character);
        const auto character = graph ? &graph->characterInstance : nullptr;
        const auto data = character && a_clipName ?
            GetCache().Lookup(character, character->behaviorGraph.get(), ClipName(*a_clipName)) : Cache::Snapshot{};
        if (data && data->motion.SampleRotation(a_motionTime, a_rotation,
                [](RE::NiQuaternion& out, float progress, const RE::NiQuaternion& previous, const RE::NiQuaternion& current) {
                    InterpolateRotation(out, progress, previous, current);
                })) {
            return;
        }
        if (a_this && (a_this->rotationDataPtr & ~std::uintptr_t{ 1 }) &&
            a_this->rotationSegCount > static_cast<std::uint32_t>(a_this->IsRotationDataAligned())) {
            ProcessRotationData(&a_this->rotationDataPtr, a_motionTime, a_rotation);
        } else {
            a_rotation = RE::NiQuaternion{ 1.0F, 0.0F, 0.0F, 0.0F };
        }
    }

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
            RE::BSAnimationGraphEvent event{};
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
