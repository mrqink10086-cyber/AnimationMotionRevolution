#include "Hooks.h"

#include "AnimMotionHandler.h"
#include "MotionCache.h"
#include "Settings.h"
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
#include "TrueHUDIntegration.h"
#endif

#include "utils/Logger.h"
#include "utils/Trampoline.h"

#include "RE/B/bhkCharProxyController.h"
#include "RE/B/bhkCharRigidBodyController.h"
#include "RE/B/bhkPickData.h"
#include "RE/C/CFilter.h"
#include "RE/H/hkpCharacterProxy.h"
#include "RE/H/hkpCharacterRigidBody.h"
#include "RE/H/hkpConvexVerticesShape.h"
#include "RE/H/hkpListShape.h"
#include "RE/M/MotionDataContainer.h"

namespace hooks
{
	namespace
	{
		constexpr float kMotionTimeEpsilon = 1.0e-4F;
		constexpr float kVectorEpsilon = 1.0e-5F;

		const RE::BSFixedString& GetWarpingGraphVariableName()
		{
			static const RE::BSFixedString name{ "AMR_IsAnimationWarpingEnabled" };
			return name;
		}

		void SetWarpingGraphVariable(const RE::ActorHandle& a_actor, bool a_enabled)
		{
			if (const auto actor = a_actor.get()) {
				// The BDI variable is an optional behavior integration. A failed setter
				// must not disable the underlying root-motion warp.
				actor->SetGraphVariableBool(GetWarpingGraphVariableName(), a_enabled);
			}
		}

		struct ParsedAnimation
		{
			explicit ParsedAnimation(const RE::hkaAnimation* a_animation) : animation(a_animation) {}
			RE::hkRefPtr<const RE::hkaAnimation> animation;
			AnimMotionData motion;
			bool hasVerticalMotion{ false };
		};

		struct TranslationRuntimeState
		{
			bool initialized{ false };
			bool wasAttacking{ false };
			bool wasWarping{ false };
			bool groundProbeInitialized{ false };
			bool wasGroundBlocked{ false };
			float previousMotionTime{ -1.0F };
			float warpScale{ -1.0F };
			std::size_t activeWarpSegmentIndex{ std::numeric_limits<std::size_t>::max() };
			RE::ActorHandle owner{};
			RE::ActorHandle target{};
			RE::NiPoint3 blockedOffset{};
			RE::NiPoint3 lastAuthored{};
			RE::NiPoint3 lastWarped{};
			RE::NiPoint3 lastOutput{};
		};

		struct BindingRuntime
		{
				// Runtime state belongs to one activation. The mutex protects only snapshot
				// and commit; engine calls run outside it. A revision check prevents concurrent
				// callbacks from overwriting a newer transition. A binding view pins the
				// runtime after deactivation so an in-flight callback can finish safely.
			std::mutex mutex;
				std::uint64_t revision{ 0 };
			TranslationRuntimeState state;
		};

		using Cache = amr::MotionCache<ParsedAnimation, RE::hkRefPtr<const RE::hkbBehaviorGraph>, BindingRuntime>;

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

		bool HasMotion(const ParsedAnimation& a_data)
		{
			return !a_data.motion.translationList.empty() || !a_data.motion.rotationList.empty();
		}

		bool HasVerticalMotion(const std::vector<Translation>& a_motion);

		bool AnyWarping(const RE::ActorHandle& a_actor)
		{
			if (!a_actor) {
				return false;
			}
				try {
					for (const auto& runtime : GetCache().RuntimeSnapshot()) {
						if (!runtime) {
							continue;
						}
						std::lock_guard guard(runtime->mutex);
						const auto& state = runtime->state;
						if (state.wasWarping && state.owner == a_actor) {
							return true;
						}
					}
				} catch (...) {
					// This is only a behavior-graph diagnostic variable. A failed snapshot
					// must not interfere with motion processing.
				}
			return false;
		}

		void UpdateWarpingForRemovedBindings(const std::vector<Cache::RuntimeOwner>& a_runtimes)
		{
			for (const auto& runtime : a_runtimes) {
				if (!runtime) {
					continue;
				}
				RE::ActorHandle owner{};
				bool wasWarping = false;
				{
					std::lock_guard guard(runtime->mutex);
					owner = runtime->state.owner;
					wasWarping = runtime->state.wasWarping;
				}
				if (wasWarping && owner) {
					SetWarpingGraphVariable(owner, AnyWarping(owner));
				}
			}
		}

		const RE::hkaAnimation* GetBoundAnimation(const RE::hkbClipGenerator* a_clip)
		{
			return a_clip && a_clip->binding && a_clip->binding->animation ?
					   a_clip->binding->animation.get() :
					   nullptr;
		}

			void ComputeStartTimeHook(
			const RE::hkbClipGenerator* a_clip,
			const RE::hkbContext* a_context)
		{
			const auto* boundAnimation = GetBoundAnimation(a_clip);
			const auto* hkbCharacter = a_context ? a_context->character : nullptr;
			if (boundAnimation && hkbCharacter) {
				Cache* activeCache = nullptr;
				Cache::Generation generation{};
				try {
					auto& cache = GetCache();
					activeCache = &cache;
					generation = cache.GetGeneration();
					auto parsed = cache.FindParsed(boundAnimation);
					if (!parsed) {
						auto fresh = std::make_shared<ParsedAnimation>(boundAnimation);
						for (const auto& track : boundAnimation->annotationTracks) {
							for (const auto& annotation : track.annotations) {
								const auto annotationText = annotation.text.c_str();
								const std::string_view text = annotationText ? std::string_view(annotationText) : std::string_view{};
								const bool warpControl = IsWarpControlAnnotation(text);
								if (warpControl) {
									fresh->motion.MarkExplicitWarpControl();
								}
								const auto annotationData = ParseAnnotation(annotation);
								if (const auto* translation = std::get_if<Translation>(&annotationData)) {
									fresh->motion.Add(*translation);
								} else if (const auto* rotation = std::get_if<Rotation>(&annotationData)) {
									fresh->motion.Add(*rotation);
								} else if (const auto* warp = std::get_if<Warp>(&annotationData)) {
									if (!fresh->motion.Add(*warp)) {
										logger::warn("Ignoring warp control with a non-finite timestamp in animation '{}': '{}'", a_clip->animationName.c_str(), text);
									}
								} else if (const auto* warpEnd = std::get_if<WarpEnd>(&annotationData)) {
									if (!fresh->motion.Add(*warpEnd)) {
										logger::warn("Ignoring warp control with a non-finite timestamp in animation '{}': '{}'", a_clip->animationName.c_str(), text);
									}
								} else if (warpControl) {
									logger::warn("Ignoring malformed warp control in animation '{}' at {}: '{}'", a_clip->animationName.c_str(), annotation.time, text);
								}
								if (IsCombatWarpBoundary(text)) {
									fresh->motion.AddCombatWarpBoundary(annotation.time);
								}
							}
						}
						fresh->motion.Finalize();
							fresh->hasVerticalMotion = HasVerticalMotion(fresh->motion.translationList);
						parsed = cache.Remember(boundAnimation, std::move(fresh), generation);
					}
						const auto previous = cache.FindGeneratorBinding(
							hkbCharacter, hkbCharacter->behaviorGraph.get(), ClipName(a_clip->name), a_clip);
						RE::ActorHandle previousOwner{};
						bool previousWasWarping = false;
						if (previous.runtime) {
							std::lock_guard guard(previous.runtime->mutex);
							previousOwner = previous.runtime->state.owner;
							previousWasWarping = previous.runtime->state.wasWarping;
						}
						bool bindingReplaced = false;
					if (parsed && HasMotion(*parsed)) {
							bindingReplaced = previous.data && previous.data->animation.get() != boundAnimation;
						RE::hkRefPtr<const RE::hkbBehaviorGraph> graph(hkbCharacter->behaviorGraph.get());
						cache.Activate(hkbCharacter, graph.get(), std::move(graph), ClipName(a_clip->name),
							boundAnimation, std::move(parsed), generation, a_clip);
						} else if (parsed) {
							const auto retired = cache.DeactivateGenerator(
								hkbCharacter, hkbCharacter->behaviorGraph.get(),
								ClipName(a_clip->name), generation, a_clip);
							UpdateWarpingForRemovedBindings(retired);
						}
						if (bindingReplaced && previousWasWarping && previousOwner) {
							SetWarpingGraphVariable(previousOwner, AnyWarping(previousOwner));
					}
				} catch (const std::exception& error) {
					if (activeCache) {
						activeCache->RetireCharacter(hkbCharacter, generation, [](const Cache::RuntimeOwner& runtime) {
							RE::ActorHandle owner{};
							{
								std::lock_guard guard(runtime->mutex);
								owner = runtime->state.owner;
							}
							if (owner) {
								SetWarpingGraphVariable(owner, AnyWarping(owner));
							}
						});
					}
					logger::error("Cannot cache animation {}: {}", a_clip->animationName.c_str(), error.what());
				}
			}
				hkbClipGenerator::ComputeStartTime(a_clip);
		}

		void ResetIgnoreStartTimeHook(
			const RE::hkbClipGenerator* a_clip,
			const RE::hkbContext* a_context)
		{
			const auto* boundAnimation = GetBoundAnimation(a_clip);
			const auto* hkbCharacter = a_context ? a_context->character : nullptr;
			if (boundAnimation && hkbCharacter) {
				auto& cache = GetCache();
				const auto generation = cache.GetGeneration();
				auto binding = cache.FindBinding(hkbCharacter, hkbCharacter->behaviorGraph.get(), ClipName(a_clip->name), boundAnimation, a_clip);
				RE::ActorHandle owner{};
				if (binding.runtime) {
					std::lock_guard guard(binding.runtime->mutex);
					owner = binding.runtime->state.owner;
				}
				cache.Deactivate(hkbCharacter, hkbCharacter->behaviorGraph.get(), ClipName(a_clip->name), boundAnimation, generation, a_clip);
				if (owner) {
					SetWarpingGraphVariable(owner, AnyWarping(owner));
				}
			}
			hkbClipGenerator::ResetIgnoreStartTime(a_clip);
		}

		RE::NiPoint3 LocalToWorld(const RE::NiPoint3& a_local, float a_yaw)
		{
			const float sine = std::sin(a_yaw);
			const float cosine = std::cos(a_yaw);
			return {
				a_local.x * cosine + a_local.y * sine,
				-a_local.x * sine + a_local.y * cosine,
				a_local.z
			};
		}

		float HorizontalLength(const RE::NiPoint3& a_vector)
		{
			return std::hypot(a_vector.x, a_vector.y);
		}

		WarpLimits GetDefaultWarpLimits()
		{
			return {
				.lowerLimit = settings::motionWarping::defaultMinimumScale,
				.upperLimit = settings::motionWarping::defaultMaximumScale,
				.maximumAngleDegrees =
					settings::motionWarping::defaultMaximumAngleDegrees,
				.maximumDistance = std::numeric_limits<float>::infinity()
			};
		}

		bool HasVerticalMotion(const std::vector<Translation>& a_motion)
		{
			return std::any_of(
				a_motion.begin(),
				a_motion.end(),
				[](const Translation& a_key) {
					return std::abs(a_key.delta.z) > kVectorEpsilon;
				});
		}

		struct ActiveWarpSegment
		{
			const AnimMotionData::WarpSegment* segment{ nullptr };
			std::size_t index{ std::numeric_limits<std::size_t>::max() };
		};

		ActiveWarpSegment GetActiveWarpSegment(
			const std::vector<AnimMotionData::WarpSegment>& a_segments,
			float a_motionTime)
		{
			const auto found = std::ranges::upper_bound(
				a_segments,
				a_motionTime,
				{},
				[](const AnimMotionData::WarpSegment& a_segment) {
					return a_segment.startTime;
				});
			if (found == a_segments.begin()) {
				return {};
			}

			const auto segment = std::prev(found);
			// Segments are half-open. A marker at this exact timestamp owns the
			// frame, while the final endpoint has no active segment.
			if (a_motionTime < segment->startTime || a_motionTime >= segment->endTime) {
				return {};
			}

			return {
				std::addressof(*segment),
				static_cast<std::size_t>(segment - a_segments.begin())
			};
		}

		void ResetTranslationRuntime(
				TranslationRuntimeState& a_state,
			RE::Character* a_character)
		{
			a_state = {};
			a_state.initialized = true;
			a_state.wasAttacking = a_character->IsAttacking();
			a_state.previousMotionTime = -1.0F;
			a_state.owner = a_character->GetHandle();
			a_state.target = a_character->GetActorRuntimeData().currentCombatTarget;
		}

		struct WarpEvaluation
		{
			bool applied{ false };
			float scale{ 1.0F };
			RE::ActorHandle target{};
		};

		WarpEvaluation EvaluateWarpSegment(
			const AnimMotionData::WarpSegment& a_segment,
			std::size_t a_segmentIndex,
				const TranslationRuntimeState& a_state,
			RE::Character* a_character,
			const RE::NiPoint3& a_origin,
			const RE::NiPoint3& a_authoredCursor,
			float a_originYaw,
			bool a_allowCachedScale)
		{
			WarpEvaluation result{};
			const bool animationOptIn =
				a_segment.kind == AnimMotionData::WarpSegmentKind::kExplicit;
			const bool defaultCombatWarp =
				a_segment.kind == AnimMotionData::WarpSegmentKind::kDefaultCombat &&
				a_character->IsAttacking() &&
				settings::motionWarping::enableForAttackAnimations;
			if (!animationOptIn && !defaultCombatWarp) {
				return result;
			}

			const WarpLimits limits = animationOptIn ? a_segment.limits : GetDefaultWarpLimits();
			result.target = a_character->GetActorRuntimeData().currentCombatTarget;
			auto target = result.target.get();
			if (!target || target->IsDead() ||
				target->GetParentCell() != a_character->GetParentCell()) {
				return result;
			}

			const RE::NiPoint3 targetOffset = target->GetPosition() - a_origin;
			const float targetDistance = HorizontalLength(targetOffset);
			if (targetDistance > limits.maximumDistance) {
				return result;
			}

			if (a_allowCachedScale && a_state.wasWarping && a_state.warpScale >= 0.0F &&
				a_state.activeWarpSegmentIndex == a_segmentIndex &&
				a_state.target == result.target) {
				result.applied = true;
				result.scale = a_state.warpScale;
				return result;
			}

			// The exact segment end was sampled once during clip activation, and the
			// current authored cursor is already available. A normal boundary uses the
			// full segment; a late reactivation uses only what remains. Both are O(1).
			const RE::NiPoint3 authoredWindow =
				a_segment.endTranslation - a_authoredCursor;
			const float authoredDistance = HorizontalLength(authoredWindow);
			if (authoredDistance < settings::motionWarping::minimumAuthoredDistance ||
				authoredDistance <= kVectorEpsilon) {
				return result;
			}

			const RE::NiPoint3 authoredWorld = LocalToWorld(authoredWindow, a_originYaw);
			if (targetDistance > kVectorEpsilon) {
				const float cosine = std::clamp(
					(authoredWorld.x * targetOffset.x + authoredWorld.y * targetOffset.y) /
						(authoredDistance * targetDistance),
					-1.0F,
					1.0F);
				const float angle =
					std::acos(cosine) * 180.0F / std::numbers::pi_v<float>;
				if (angle > limits.maximumAngleDegrees) {
					return result;
				}
			}

			const float desiredDistance = std::max(
				0.0F,
				targetDistance - settings::motionWarping::stopDistance);
			const float requestedScale = desiredDistance / authoredDistance;
			result.applied = true;
			result.scale = std::clamp(
				requestedScale,
				limits.lowerLimit,
				limits.upperLimit);
			logger::info(
				"[AMR-DIAG][warp-scale] actor=0x{:08X} target=0x{:08X} explicit={} segmentIndex={} segment=({}, {}) limits=({}, {}) maximumAngle={} maximumDistance={} targetDistance={} authoredWindowDistance={} desiredDistance={} requestedScale={} scale={}",
				a_character->GetFormID(),
				target->GetFormID(),
				animationOptIn,
				a_segmentIndex,
				a_segment.startTime,
				a_segment.endTime,
				limits.lowerLimit,
				limits.upperLimit,
				limits.maximumAngleDegrees,
				limits.maximumDistance,
				targetDistance,
				authoredDistance,
				desiredDistance,
				requestedScale,
				result.scale);
			return result;
		}

		RE::NiPoint3 ApplyWarpEvaluation(
			RE::NiPoint3 a_delta,
			const WarpEvaluation& a_evaluation)
		{
			if (a_evaluation.applied) {
				a_delta.x *= a_evaluation.scale;
				a_delta.y *= a_evaluation.scale;
			}
			return a_delta;
		}

		RE::NiPoint3 WarpTranslation(
			const RE::NiPoint3& a_translation,
			const AnimMotionData& a_motionData,
			float a_motionTime,
				TranslationRuntimeState& a_state,
			RE::Character* a_character,
			ActiveWarpSegment& a_activeSegment,
			bool& a_warpApplied,
			bool& a_warpSegmentChanged)
		{
			const auto previousSegmentIndex = a_state.activeWarpSegmentIndex;
			a_activeSegment = GetActiveWarpSegment(a_motionData.warpSegments, a_motionTime);
			a_warpSegmentChanged = a_activeSegment.index != previousSegmentIndex;

			const float currentTime = std::max(0.0F, a_motionTime);
			float timeCursor = a_state.previousMotionTime >= 0.0F ?
								   std::max(0.0F, a_state.previousMotionTime) :
								   0.0F;
			const bool firstUpdate = a_state.previousMotionTime < 0.0F;
			RE::NiPoint3 authoredCursor =
				firstUpdate ? a_motionData.initialTranslation : a_state.lastAuthored;
			// A non-zero cumulative value at t=0 is a baseline, not elapsed segment
			// motion. Preserve it unscaled and integrate only changes after t=0.
			RE::NiPoint3 mappedDelta = firstUpdate ? a_motionData.initialTranslation : RE::NiPoint3{};
			const auto actorPosition = a_character->GetPosition();
			const float actorYaw = a_character->GetAngleZ();

			WarpEvaluation finalEvaluation{};
			std::size_t finalEvaluationIndex = std::numeric_limits<std::size_t>::max();
			auto segmentIt = std::ranges::upper_bound(
				a_motionData.warpSegments,
				timeCursor,
				{},
				[](const AnimMotionData::WarpSegment& a_segment) {
					return a_segment.endTime;
				});

			for (; segmentIt != a_motionData.warpSegments.end() && timeCursor < currentTime;
				 ++segmentIt) {
				if (segmentIt->endTime <= timeCursor) {
					continue;
				}
				if (segmentIt->startTime > timeCursor) {
					if (segmentIt->startTime >= currentTime) {
						break;
					}
					mappedDelta += segmentIt->startTranslation - authoredCursor;
					authoredCursor = segmentIt->startTranslation;
					timeCursor = segmentIt->startTime;
				}
				if (segmentIt->startTime >= currentTime) {
					break;
				}

				const float pieceEndTime = std::min(currentTime, segmentIt->endTime);
				if (pieceEndTime <= timeCursor) {
					continue;
				}
				const RE::NiPoint3 pieceEndTranslation =
					segmentIt->endTime <= currentTime ? segmentIt->endTranslation : a_translation;
				const auto segmentIndex =
					static_cast<std::size_t>(segmentIt - a_motionData.warpSegments.begin());
				const RE::NiPoint3 evaluationOrigin =
					actorPosition + LocalToWorld(mappedDelta, actorYaw);
				const auto evaluation = EvaluateWarpSegment(
					*segmentIt,
					segmentIndex,
					a_state,
					a_character,
					evaluationOrigin,
					authoredCursor,
					actorYaw,
					segmentIndex == previousSegmentIndex);
				mappedDelta += ApplyWarpEvaluation(
					pieceEndTranslation - authoredCursor,
					evaluation);
				authoredCursor = pieceEndTranslation;
				timeCursor = pieceEndTime;
				finalEvaluation = evaluation;
				finalEvaluationIndex = segmentIndex;
			}

			if (timeCursor < currentTime) {
				mappedDelta += a_translation - authoredCursor;
				authoredCursor = a_translation;
			}

			if (a_activeSegment.segment &&
				finalEvaluationIndex != a_activeSegment.index) {
				const RE::NiPoint3 evaluationOrigin =
					actorPosition + LocalToWorld(mappedDelta, actorYaw);
				finalEvaluation = EvaluateWarpSegment(
					*a_activeSegment.segment,
					a_activeSegment.index,
					a_state,
					a_character,
					evaluationOrigin,
					authoredCursor,
					actorYaw,
					a_activeSegment.index == previousSegmentIndex);
				finalEvaluationIndex = a_activeSegment.index;
			}

			a_warpApplied = a_activeSegment.segment && finalEvaluation.applied;
			a_state.activeWarpSegmentIndex = a_activeSegment.index;
			a_state.warpScale = a_warpApplied ? finalEvaluation.scale : -1.0F;
			a_state.target = finalEvaluation.target;
			a_state.lastAuthored = a_translation;
			return a_state.lastWarped + mappedDelta;
		}

		struct GroundProbeResult
		{
			bool hasGround{ true };
			bool hasWorld{ false };
			float startZ{ 0.0F };
			float endZ{ 0.0F };
			float hitFraction{ 1.0F };
			float worldScale{ 0.0F };
			std::uint32_t filterInfo{ 0 };
			RE::NiPoint3 hitPosition{};
		};

		struct ControllerRadius
		{
			std::uint32_t rootShapeType{ 0 };
			float halfExtentX{ 0.0F };
			float halfExtentY{ 0.0F };
			float convexMargin{ 0.0F };
			float inverseScale{ 0.0F };
			float world{ 0.0F };
		};

		ControllerRadius GetCharacterControllerRadius(RE::Character* a_character)
		{
			ControllerRadius result{};
			if (const auto* controller = a_character->GetCharController()) {
				const RE::hkpShape* shape = nullptr;
				if (const auto* proxyController =
						skyrim_cast<const RE::bhkCharProxyController*>(controller)) {
					if (const auto* proxy = proxyController->GetCharacterProxy();
						proxy && proxy->shapePhantom) {
						shape = proxy->shapePhantom->collidable.shape;
					}
				} else if (const auto* rigidController =
							   skyrim_cast<const RE::bhkCharRigidBodyController*>(controller)) {
					const auto* characterRigidBody = static_cast<const RE::hkpCharacterRigidBody*>(
						rigidController->charRigidBody.referencedObject.get());
					if (characterRigidBody && characterRigidBody->character) {
						shape = characterRigidBody->character->collidable.shape;
					}
				}

				if (!shape) {
					return result;
				}
				result.rootShapeType = static_cast<std::uint32_t>(shape->type);

				if (const auto* list = skyrim_cast<const RE::hkpListShape*>(shape);
					list && !list->childInfo.empty()) {
					shape = list->childInfo.front().shape;
				}

				if (const auto* convex =
						skyrim_cast<const RE::hkpConvexVerticesShape*>(shape)) {
					result.halfExtentX = std::abs(convex->aabbHalfExtents.quad.m128_f32[0]);
					result.halfExtentY = std::abs(convex->aabbHalfExtents.quad.m128_f32[1]);
					result.convexMargin = convex->radius;
					result.inverseScale = RE::bhkWorld::GetWorldScaleInverse();
					result.world =
						std::max(result.halfExtentX, result.halfExtentY) * result.inverseScale;
					if (std::isfinite(result.world) && result.world > kVectorEpsilon) {
						return result;
					}
				}
			}
			return {};
		}

		GroundProbeResult ProbeGroundAt(
			RE::Character* a_character,
			const RE::NiPoint3& a_position)
		{
			GroundProbeResult result{};
			const auto* cell = a_character->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return result;
			}
			result.hasWorld = true;

			// Skyrim positions are converted to Havok space with GetWorldScale().
			// GetWorldScaleInverse() converts in the opposite direction and places
			// the ray thousands of Havok units away from the destination.
			const float scale = RE::bhkWorld::GetWorldScale();
			const float startZ = a_position.z + settings::edgeProtection::startHeight;
			const float endZ = startZ - settings::edgeProtection::downwardRange;
			result.startZ = startZ;
			result.endZ = endZ;
			result.worldScale = scale;

			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4{
				a_position.x * scale,
				a_position.y * scale,
				startZ * scale,
				0.0F
			};
			pick.rayInput.to = RE::hkVector4{
				a_position.x * scale,
				a_position.y * scale,
				endZ * scale,
				0.0F
			};
			RE::CFilter actorFilter{};
			a_character->GetCollisionFilterInfo(actorFilter);
			const auto collisionGroup = static_cast<std::uint16_t>(actorFilter.filter >> 16);
			pick.rayInput.filterInfo.filter =
				(static_cast<std::uint32_t>(collisionGroup) << 16) |
				static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);
			result.filterInfo = pick.rayInput.filterInfo.filter;

			world->PickObject(pick);
			result.hasGround = pick.rayOutput.HasHit();
			result.hitFraction = pick.rayOutput.hitFraction;
			if (result.hasGround) {
				result.hitPosition = {
					a_position.x,
					a_position.y,
					startZ + (endZ - startZ) * result.hitFraction
				};
			}
			return result;
		}

			void ApplyTranslationModifiers(
				const AnimMotionData& a_motionData,
					bool a_hasVerticalMotion,
				float a_motionTime,
				TranslationRuntimeState& a_state,
				RE::Character* a_character,
				RE::NiPoint3& a_translation,
				std::string_view a_clipName)
		{
				auto& state = a_state;
				const bool hasVerticalMotion = a_hasVerticalMotion;
			const bool resetRuntime = !state.initialized ||
									  a_motionTime + kMotionTimeEpsilon < state.previousMotionTime;
			if (resetRuntime) {
				ResetTranslationRuntime(state, a_character);
				const auto& authoredEnd = a_motionData.translationList.back().delta;
				logger::info(
					"[AMR-DIAG][motion-start] actor=0x{:08X} clip='{}' time={} keys={} authoredEnd=({}, {}, {}) position=({}, {}, {}) verticalMotion={} attackEdgeProtection={} defaultAttackWarping={} attacking={}",
					a_character->GetFormID(),
					a_clipName,
					a_motionTime,
					a_motionData.translationList.size(),
					authoredEnd.x,
					authoredEnd.y,
					authoredEnd.z,
					a_character->GetPositionX(),
					a_character->GetPositionY(),
					a_character->GetPositionZ(),
					hasVerticalMotion,
					settings::edgeProtection::enableForAttackAnimations,
					settings::motionWarping::enableForAttackAnimations,
					a_character->IsAttacking());
			}

			if (state.previousMotionTime >= 0.0F &&
				std::abs(a_motionTime - state.previousMotionTime) <= kMotionTimeEpsilon) {
				a_translation = state.lastOutput;
				return;
			}

			const bool isAttacking = a_character->IsAttacking();
			ActiveWarpSegment activeWarpSegment{};
			bool warpApplied = false;
			bool warpSegmentChanged = false;
			const RE::NiPoint3 warped = WarpTranslation(
				a_translation,
				a_motionData,
				a_motionTime,
				state,
				a_character,
				activeWarpSegment,
				warpApplied,
				warpSegmentChanged);
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
			const bool warpSegmentEnabled =
				activeWarpSegment.segment &&
				(activeWarpSegment.segment->kind ==
						 AnimMotionData::WarpSegmentKind::kExplicit ||
					(activeWarpSegment.segment->kind ==
						 AnimMotionData::WarpSegmentKind::kDefaultCombat &&
					 settings::motionWarping::enableForAttackAnimations &&
					 isAttacking));
			const bool visualizeWarpActivation =
				warpSegmentEnabled &&
				(warpSegmentChanged || (warpApplied && !state.wasWarping));
			if (visualizeWarpActivation) {
				const RE::NiPoint3 authoredSegment =
					activeWarpSegment.segment->endTranslation -
					activeWarpSegment.segment->startTranslation;
				RE::NiPoint3 warpedSegment = authoredSegment;
				if (warpApplied) {
					warpedSegment.x *= state.warpScale;
					warpedSegment.y *= state.warpScale;
				}
				const auto actorPosition = a_character->GetPosition();
				const float actorYaw = a_character->GetAngleZ();
				truehud::DrawMotionWarp(
					actorPosition,
					authoredSegment,
					warpApplied ? std::addressof(warpedSegment) : nullptr,
					actorYaw);
			}
#endif
			if (state.previousMotionTime >= 0.0F &&
				(warpApplied != state.wasWarping || warpSegmentChanged)) {
				logger::info(
					"[AMR-DIAG][warp] clip='{}' state={} attacking={} time={}",
					a_clipName,
					warpApplied ? "applied" : "inactive",
					isAttacking,
					a_motionTime);
			}
			state.wasAttacking = isAttacking;
			state.wasWarping = warpApplied;
			RE::NiPoint3 output = warped - state.blockedOffset;
			const RE::NiPoint3 localDelta = output - state.lastOutput;
			const RE::NiPoint3 intendedLocalDelta = warped - state.lastWarped;

			if (settings::edgeProtection::enableForAttackAnimations && isAttacking &&
				!hasVerticalMotion &&
				HorizontalLength(localDelta) >=
					settings::edgeProtection::minimumHorizontalDelta) {
				const RE::NiPoint3 worldDelta =
					LocalToWorld(localDelta, a_character->GetAngleZ());
				const RE::NiPoint3 predictedCenter = a_character->GetPosition() + worldDelta;
				RE::NiPoint3 boundaryDirection =
					LocalToWorld(intendedLocalDelta, a_character->GetAngleZ());
				float boundaryDirectionLength = HorizontalLength(boundaryDirection);
				if (boundaryDirectionLength <= kVectorEpsilon) {
					boundaryDirection = worldDelta;
					boundaryDirectionLength = HorizontalLength(boundaryDirection);
				}
				const auto controllerRadius = GetCharacterControllerRadius(a_character);
				RE::NiPoint3 probePosition = predictedCenter;
				if (boundaryDirectionLength > kVectorEpsilon && controllerRadius.world > 0.0F) {
					probePosition.x += boundaryDirection.x / boundaryDirectionLength * controllerRadius.world;
					probePosition.y += boundaryDirection.y / boundaryDirectionLength * controllerRadius.world;
				}

				const auto probe = ProbeGroundAt(a_character, probePosition);
#ifdef AMR_ENABLE_TRUEHUD_DEBUG
				if (settings::edgeProtection::debugDraw) {
					const RE::NiPoint3 rayStart{
						probePosition.x, probePosition.y, probe.startZ
					};
					const RE::NiPoint3 rayEnd{
						probePosition.x, probePosition.y, probe.endZ
					};
					truehud::DrawGroundProbe(
						predictedCenter,
						probePosition,
						rayStart,
						rayEnd,
						probe.hasGround ? std::addressof(probe.hitPosition) : nullptr);
				}
#endif
				const bool groundBlocked = !probe.hasGround;
				if (!state.groundProbeInitialized ||
					groundBlocked != state.wasGroundBlocked) {
					logger::info(
						"[AMR-DIAG][ground] clip='{}' state={} center=({}, {}, {}) boundary=({}, {}, {}) rootShapeType={} convexHalfExtents=({}, {}) convexMargin={} inverseScale={} controllerRadiusWorld={} rayZ=({}, {}) hitFraction={} scale={} filter=0x{:08X} world={}",
						a_clipName,
						groundBlocked ? "blocked" : "clear",
						predictedCenter.x,
						predictedCenter.y,
						predictedCenter.z,
						probePosition.x,
						probePosition.y,
						probePosition.z,
						controllerRadius.rootShapeType,
						controllerRadius.halfExtentX,
						controllerRadius.halfExtentY,
						controllerRadius.convexMargin,
						controllerRadius.inverseScale,
						controllerRadius.world,
						probe.startZ,
						probe.endZ,
						probe.hitFraction,
						probe.worldScale,
						probe.filterInfo,
						probe.hasWorld);
				}
				state.groundProbeInitialized = true;
				state.wasGroundBlocked = groundBlocked;

				if (groundBlocked) {
					state.blockedOffset.x += localDelta.x;
					state.blockedOffset.y += localDelta.y;
					output.x = state.lastOutput.x;
					output.y = state.lastOutput.y;
				}
			}

			state.previousMotionTime = a_motionTime;
			state.lastWarped = warped;
			state.lastOutput = output;
			a_translation = output;
		}

		void ProcessTranslationDataHook(
			RE::MotionDataContainer* a_container,
			float a_motionTime,
			RE::NiPoint3& a_translation,
			const RE::BSFixedString* a_clipName,
			RE::Character* a_character)
		{
				const auto activeGraph = amr::abi::ActiveGraph(a_character);
				auto* hkbCharacter = activeGraph ? std::addressof(activeGraph->characterInstance) : nullptr;
				const auto binding = hkbCharacter && a_clipName ?
				GetCache().LookupBinding(hkbCharacter, hkbCharacter->behaviorGraph.get(), ClipName(*a_clipName)) : Cache::BindingView{};
			if (binding.data && binding.runtime && a_character &&
				binding.data->motion.SampleTranslation(a_motionTime, a_translation)) {
					const auto authoredTranslation = a_translation;
					RE::ActorHandle previousOwner{};
					RE::ActorHandle currentOwner{};
					bool updateWarpingVariable = false;
					for (;;) {
						TranslationRuntimeState state;
						std::uint64_t revision = 0;
							RE::ActorHandle ownerBefore{};
							bool wasWarpingBefore = false;
						{
							std::lock_guard guard(binding.runtime->mutex);
							state = binding.runtime->state;
							revision = binding.runtime->revision;
						}
							ownerBefore = state.owner;
							wasWarpingBefore = state.wasWarping;

						// This transition may query actor state and physics. Keep the runtime
						// mutex unlocked during all engine calls; retry if another callback
						// commits first.
						a_translation = authoredTranslation;
						ApplyTranslationModifiers(
							binding.data->motion,
								binding.data->hasVerticalMotion,
							a_motionTime,
							state,
							a_character,
							a_translation,
							a_clipName->c_str());

						{
							std::lock_guard guard(binding.runtime->mutex);
							if (revision != binding.runtime->revision) {
								continue;
							}
							binding.runtime->state = std::move(state);
							++binding.runtime->revision;
								currentOwner = binding.runtime->state.owner;
								previousOwner = ownerBefore;
								updateWarpingVariable =
									wasWarpingBefore != binding.runtime->state.wasWarping ||
									ownerBefore != binding.runtime->state.owner;
						}
						break;
				}
					if (updateWarpingVariable) {
						if (previousOwner) {
							SetWarpingGraphVariable(previousOwner, AnyWarping(previousOwner));
						}
						if (currentOwner && currentOwner != previousOwner) {
							SetWarpingGraphVariable(currentOwner, AnyWarping(currentOwner));
						}
					}
				return;
			}

			const bool hasVanillaMotion = a_container &&
				(a_container->translationDataPtr & ~std::uintptr_t{ 1 }) &&
				a_container->translationSegCount >
				static_cast<std::uint32_t>(a_container->IsTranslationDataAligned());
			if (hasVanillaMotion) {
				MotionDataContainer::ProcessTranslationData(
					std::addressof(a_container->translationDataPtr),
					a_motionTime,
					a_translation);
				return;
			}

			a_translation = {};
		}

		void ProcessRotationDataHook(
			RE::MotionDataContainer* a_container,
			float a_motionTime,
			RE::NiQuaternion& a_rotation,
			const RE::BSFixedString* a_clipName,
			RE::Character* a_character)
		{
				const auto activeGraph = amr::abi::ActiveGraph(a_character);
				auto* hkbCharacter = activeGraph ? std::addressof(activeGraph->characterInstance) : nullptr;
				const auto binding = hkbCharacter && a_clipName ?
				GetCache().LookupBinding(hkbCharacter, hkbCharacter->behaviorGraph.get(), ClipName(*a_clipName)) : Cache::BindingView{};
			if (binding.data && binding.data->motion.SampleRotation(a_motionTime, a_rotation,
					[](RE::NiQuaternion& a_out, float a_progress, const RE::NiQuaternion& a_previous, const RE::NiQuaternion& a_current) {
						MotionDataContainer::InterpolateRotation(a_out, a_progress, a_previous, a_current);
					})) {
				return;
			}

			const bool hasVanillaMotion = a_container &&
				(a_container->rotationDataPtr & ~std::uintptr_t{ 1 }) &&
				a_container->rotationSegCount >
				static_cast<std::uint32_t>(a_container->IsRotationDataAligned());
			if (hasVanillaMotion) {
				MotionDataContainer::ProcessRotationData(
					std::addressof(a_container->rotationDataPtr),
					a_motionTime,
					a_rotation);
				return;
			}

			a_rotation = RE::NiQuaternion{ 1.0F, 0.0F, 0.0F, 0.0F };
		}

	}

		void hkbClipGenerator::ComputeStartTime_Hook(
			const RE::hkbClipGenerator* a_this,
			const RE::hkbContext* a_context)
		{
			ComputeStartTimeHook(a_this, a_context);
		}

		void hkbClipGenerator::ResetIgnoreStartTime_Hook(
			const RE::hkbClipGenerator* a_this,
			const RE::hkbContext* a_context)
		{
			ResetIgnoreStartTimeHook(a_this, a_context);
		}

		void MotionDataContainer::ProcessTranslationData_Hook(
			RE::MotionDataContainer* a_this,
			float a_motionTime,
			RE::NiPoint3& a_translation,
			const RE::BSFixedString* a_clipName,
			RE::Character* a_character)
		{
			ProcessTranslationDataHook(a_this, a_motionTime, a_translation, a_clipName, a_character);
		}

		void MotionDataContainer::ProcessRotationData_Hook(
			RE::MotionDataContainer* a_this,
			float a_motionTime,
			RE::NiQuaternion& a_rotation,
			const RE::BSFixedString* a_clipName,
			RE::Character* a_character)
		{
			ProcessRotationDataHook(a_this, a_motionTime, a_rotation, a_clipName, a_character);
		}

		void ClearMotionCache()
		{
			GetCache().Clear();
		}

		bool HasAnimMotionData(const RE::hkbCharacter* a_hkbCharacter) noexcept
		{
			return a_hkbCharacter && GetCache().HasActive(a_hkbCharacter, [](const ParsedAnimation& a_data) {
				return HasMotion(a_data);
			});
		}
}
