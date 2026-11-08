#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>

namespace amr
{
    // Runtime is created once per activation, while Data is shared by the parsed
    // LRU. Callers own runtime synchronization and may retain a BindingView after
    // deactivation; that view keeps both the immutable data and mutable state alive.
    template <class Data, class GraphOwner = std::shared_ptr<const void>, class Runtime = std::monostate>
    class MotionCache
    {
    public:
        using Snapshot = std::shared_ptr<const Data>;
        using RuntimeOwner = std::shared_ptr<Runtime>;
        using Generation = std::uint64_t;
        static constexpr std::size_t parsedCapacity = 32;
        static constexpr std::size_t shardCount = 16;

        struct BindingView
        {
            Snapshot data;
            RuntimeOwner runtime;
        };

        [[nodiscard]] Generation GetGeneration() const noexcept
        {
            return generation.load(std::memory_order_acquire);
        }

        [[nodiscard]] Snapshot FindParsed(const void* a_animation)
        {
            std::lock_guard guard(parsedMutex);
            const auto it = std::find_if(parsed.begin(), parsed.end(),
                [=](const Parsed& a_entry) { return a_entry.animation == a_animation; });
            if (it == parsed.end()) {
                return {};
            }
            it->lastUse = ++clock;
            return it->data;
        }

        [[nodiscard]] Snapshot Remember(const void* a_animation, Snapshot a_data, Generation a_generation)
        {
            Snapshot retired;
            std::lock_guard guard(parsedMutex);
            if (GetGeneration() != a_generation) {
                return {};
            }
            const auto it = std::find_if(parsed.begin(), parsed.end(),
                [=](const Parsed& a_entry) { return a_entry.animation == a_animation; });
            if (it != parsed.end()) {
                it->lastUse = ++clock;
                return it->data;
            }
            if (parsed.size() == parsedCapacity) {
                const auto oldest = std::min_element(parsed.begin(), parsed.end(),
                    [](const Parsed& a, const Parsed& b) { return a.lastUse < b.lastUse; });
                retired = std::move(oldest->data);
                *oldest = Parsed{ a_animation, a_data, ++clock };
            } else {
                parsed.push_back(Parsed{ a_animation, a_data, ++clock });
            }
            return a_data;
        }

        void Activate(const void* a_character, const void* a_graph, GraphOwner a_graphOwner,
            std::string_view a_clip, const void* a_animation, Snapshot a_data, Generation a_generation,
            const void* a_generator = nullptr)
        {
            auto runtime = std::make_shared<Runtime>();
            CharacterEntry incoming;
            incoming.graph = a_graph;
            incoming.owner = std::move(a_graphOwner);
            incoming.clips.emplace(std::string(a_clip), std::vector<Binding>{ { a_animation, a_generator, a_data, runtime } });
            CharacterEntry retired;
            std::vector<Binding> retiredBindings;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (GetGeneration() != a_generation) {
                return;
            }
            const auto [entry, inserted] = shard.characters.try_emplace(a_character);
            auto& character = entry->second;
            if (inserted || character.graph != a_graph) {
                retired = std::move(character);
                character = std::move(incoming);
                return;
            }
            auto& clips = character.clips;
            auto clip = clips.find(a_clip);
            if (clip == clips.end()) {
                clips.insert(incoming.clips.extract(incoming.clips.begin()));
            } else {
                auto& bindings = clip->second;
                if (a_generator) {
                    // Reserve before mutating so an allocation failure leaves the
                    // existing binding stack intact. Same-animation activations
                    // remain counted; changed animations replace this generator's
                    // former records.
                    bindings.reserve(bindings.size() + 1);
                    retiredBindings.reserve(bindings.size());
                    for (auto binding = bindings.begin(); binding != bindings.end();) {
                        if (binding->generator == a_generator && binding->animation != a_animation) {
                            retiredBindings.push_back(std::move(*binding));
                            binding = bindings.erase(binding);
                        } else {
                            ++binding;
                        }
                    }
                }
                clip->second.push_back(Binding{ a_animation, a_generator, a_data, std::move(runtime) });
            }
        }

        [[nodiscard]] Snapshot Lookup(const void* a_character, const void* a_graph, std::string_view a_clip)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end() || character->second.graph != a_graph) {
                return {};
            }
            const auto clip = character->second.clips.find(a_clip);
            return clip == character->second.clips.end() || clip->second.empty() ? Snapshot{} : clip->second.back().data;
        }

        [[nodiscard]] BindingView LookupBinding(const void* a_character, const void* a_graph, std::string_view a_clip)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end() || character->second.graph != a_graph) {
                return {};
            }
            const auto clip = character->second.clips.find(a_clip);
            if (clip == character->second.clips.end() || clip->second.empty()) {
                return {};
            }
            const auto& binding = clip->second.back();
            return { binding.data, binding.runtime };
        }

        [[nodiscard]] BindingView FindBinding(const void* a_character, const void* a_graph, std::string_view a_clip,
            const void* a_animation, const void* a_generator)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end() || character->second.graph != a_graph) {
                return {};
            }
            const auto clip = character->second.clips.find(a_clip);
            if (clip == character->second.clips.end()) {
                return {};
            }
            const auto& bindings = clip->second;
            const auto binding = std::find_if(bindings.rbegin(), bindings.rend(), [=](const Binding& a_entry) {
                return a_entry.animation == a_animation && a_entry.generator == a_generator;
            });
            return binding == bindings.rend() ? BindingView{} : BindingView{ binding->data, binding->runtime };
        }

        [[nodiscard]] BindingView FindGeneratorBinding(const void* a_character, const void* a_graph,
            std::string_view a_clip, const void* a_generator)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end() || character->second.graph != a_graph) {
                return {};
            }
            const auto clip = character->second.clips.find(a_clip);
            if (clip == character->second.clips.end()) {
                return {};
            }
            const auto& bindings = clip->second;
            const auto binding = std::find_if(bindings.rbegin(), bindings.rend(), [=](const Binding& a_entry) {
                return a_entry.generator == a_generator;
            });
            return binding == bindings.rend() ? BindingView{} : BindingView{ binding->data, binding->runtime };
        }

        template <class Predicate>
        [[nodiscard]] bool HasActive(const void* a_character, Predicate&& a_predicate)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end()) {
                return false;
            }
            for (const auto& [clipName, bindings] : character->second.clips) {
                (void)clipName;
                for (const auto& binding : bindings) {
                    if (binding.data && a_predicate(*binding.data)) {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] std::vector<RuntimeOwner> RuntimeSnapshot()
        {
            std::vector<RuntimeOwner> result;
            for (auto& shard : shards) {
                std::lock_guard guard(shard.mutex);
                for (const auto& [character, entry] : shard.characters) {
                    (void)character;
                    for (const auto& [clipName, bindings] : entry.clips) {
                        (void)clipName;
                        for (const auto& binding : bindings) {
                            result.push_back(binding.runtime);
                        }
                    }
                }
            }
            return result;
        }

        void Deactivate(const void* a_character, const void* a_graph, std::string_view a_clip, const void* a_animation,
            Generation a_generation, const void* a_generator = nullptr)
        {
            Snapshot retired;
            CharacterEntry retiredCharacter;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (GetGeneration() != a_generation) {
                return;
            }
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end() || character->second.graph != a_graph) {
                return;
            }
            auto& clips = character->second.clips;
            const auto clip = clips.find(a_clip);
            if (clip == clips.end()) {
                return;
            }
            auto& bindings = clip->second;
            const auto binding = std::find_if(bindings.rbegin(), bindings.rend(),
                [=](const Binding& a_entry) {
                    return a_entry.animation == a_animation && a_entry.generator == a_generator;
                });
            if (binding != bindings.rend()) {
                retired = std::move(binding->data);
                bindings.erase(std::next(binding).base());
            }
            if (bindings.empty()) {
                clips.erase(clip);
                if (clips.empty()) {
                    retiredCharacter = std::move(character->second);
                    shard.characters.erase(character);
                }
            }
        }

        std::vector<RuntimeOwner> DeactivateGenerator(const void* a_character, const void* a_graph, std::string_view a_clip,
            Generation a_generation, const void* a_generator)
        {
            std::vector<RuntimeOwner> retiredRuntimes;
            std::vector<Binding> retiredBindings;
            CharacterEntry retiredCharacter;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (GetGeneration() != a_generation) {
                return retiredRuntimes;
            }
            const auto character = shard.characters.find(a_character);
            if (character == shard.characters.end()) {
                return retiredRuntimes;
            }
            if (character->second.graph != a_graph) {
                std::size_t bindingCount = 0;
                for (const auto& [name, bindings] : character->second.clips) {
                    (void)name;
                    bindingCount += bindings.size();
                }
                retiredRuntimes.reserve(bindingCount);
                for (const auto& [name, bindings] : character->second.clips) {
                    (void)name;
                    for (const auto& binding : bindings) {
                        if (binding.runtime) {
                            retiredRuntimes.push_back(binding.runtime);
                        }
                    }
                }
                retiredCharacter = std::move(character->second);
                shard.characters.erase(character);
                return retiredRuntimes;
            }
            auto& clips = character->second.clips;
            const auto clip = clips.find(a_clip);
            if (clip == clips.end()) {
                return retiredRuntimes;
            }
            auto& bindings = clip->second;
            retiredRuntimes.reserve(bindings.size());
            retiredBindings.reserve(bindings.size());
            for (auto binding = bindings.begin(); binding != bindings.end();) {
                if (binding->generator == a_generator) {
                    if (binding->runtime) {
                        retiredRuntimes.push_back(binding->runtime);
                    }
                    retiredBindings.push_back(std::move(*binding));
                    binding = bindings.erase(binding);
                } else {
                    ++binding;
                }
            }
            if (bindings.empty()) {
                clips.erase(clip);
                if (clips.empty()) {
                    retiredCharacter = std::move(character->second);
                    shard.characters.erase(character);
                }
            }
            return retiredRuntimes;
        }

        template <class Visitor>
        void RetireCharacter(const void* a_character, Generation a_generation, Visitor&& a_visitor)
        {
            typename CharacterMap::node_type retired;
            auto& shard = GetShard(a_character);
            {
                std::lock_guard guard(shard.mutex);
                if (GetGeneration() != a_generation) {
                    return;
                }
                const auto character = shard.characters.find(a_character);
                if (character == shard.characters.end()) {
                    return;
                }
                retired = shard.characters.extract(character);
            }
            if (retired) {
                for (const auto& [name, bindings] : retired.mapped().clips) {
                    (void)name;
                    for (const auto& binding : bindings) {
                        if (binding.runtime) {
                            a_visitor(binding.runtime);
                        }
                    }
                }
            }
        }

        void Clear()
        {
            std::array<CharacterMap, shardCount> retiredCharacters;
            std::vector<Parsed> retiredParsed;
            std::array<std::unique_lock<std::mutex>, shardCount> guards;
            for (std::size_t i = 0; i < shardCount; ++i) {
                guards[i] = std::unique_lock(shards[i].mutex);
            }
            std::unique_lock parsedGuard(parsedMutex);
            generation.fetch_add(1, std::memory_order_release);
            for (std::size_t i = 0; i < shardCount; ++i) {
                retiredCharacters[i].swap(shards[i].characters);
            }
            retiredParsed.swap(parsed);
            clock = 0;
        }

    private:
        struct StringHash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view a_string) const noexcept
            {
                return std::hash<std::string_view>{}(a_string);
            }
        };
        struct Binding
        {
            const void* animation;
            const void* generator;
            Snapshot data;
            RuntimeOwner runtime;
        };
        struct Parsed
        {
            const void* animation;
            Snapshot data;
            std::uint64_t lastUse;
        };
        using ClipMap = std::unordered_map<std::string, std::vector<Binding>, StringHash, std::equal_to<>>;
        struct CharacterEntry
        {
            const void* graph = nullptr;
            GraphOwner owner;
            ClipMap clips;
        };
        using CharacterMap = std::unordered_map<const void*, CharacterEntry>;
        struct alignas(64) Shard
        {
            std::mutex mutex;
            CharacterMap characters;
        };

        Shard& GetShard(const void* a_character) noexcept
        {
            const auto address = reinterpret_cast<std::uintptr_t>(a_character);
            return shards[((address >> 4) ^ (address >> 13)) & (shardCount - 1)];
        }

        std::array<Shard, shardCount> shards;
        std::mutex parsedMutex;
        std::vector<Parsed> parsed;
        std::uint64_t clock = 0;
        std::atomic<Generation> generation{ 0 };
    };
}
