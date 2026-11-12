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
    template <class Data, class Runtime = std::monostate>
    class MotionCache
    {
    public:
        using Snapshot = std::shared_ptr<const Data>;
        using RuntimeOwner = std::shared_ptr<Runtime>;
        using Generation = std::uint64_t;
        static constexpr std::size_t shardCount = 16;

        struct Ticket
        {
            const void* character = nullptr;
            const void* graph = nullptr;
            Generation instance = 0;
            Generation generation = 0;
            explicit operator bool() const noexcept { return instance != 0; }
        };

        void RegisterCharacter(const void* a_character)
        {
            typename CharacterMap::node_type retired;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            shard.lifetimes.insert_or_assign(a_character, Lifetime{ nullptr, NextInstance() });
            retired = shard.characters.extract(a_character);
        }

        void UnregisterCharacter(const void* a_character)
        {
            typename CharacterMap::node_type retired;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            shard.lifetimes.erase(a_character);
            retired = shard.characters.extract(a_character);
        }

        [[nodiscard]] Ticket Begin(const void* a_character, const void* a_graph)
        {
            typename CharacterMap::node_type retired;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto life = shard.lifetimes.find(a_character);
            if (life == shard.lifetimes.end()) {
                return {};
            }
            if (life->second.graph != a_graph) {
                life->second = { a_graph, NextInstance() };
                retired = shard.characters.extract(a_character);
            }
            return { a_character, a_graph, life->second.instance, GetGeneration() };
        }

        [[nodiscard]] Ticket GetTicket(const void* a_character, const void* a_graph)
        {
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            const auto life = shard.lifetimes.find(a_character);
            if (life == shard.lifetimes.end() || life->second.graph != a_graph) {
                return {};
            }
            return { a_character, a_graph, life->second.instance, GetGeneration() };
        }

        struct BindingView
        {
            Snapshot data;
            RuntimeOwner runtime;
            const void* animation = nullptr;
        };

        [[nodiscard]] Generation GetGeneration() const noexcept
        {
            return generation.load(std::memory_order_acquire);
        }

        void Activate(Ticket a_ticket,
            std::string_view a_clip, const void* a_animation, Snapshot a_data,
            const void* a_generator = nullptr)
        {
            const auto a_character = a_ticket.character;
            const auto a_graph = a_ticket.graph;
            auto runtime = std::make_shared<Runtime>();
            CharacterEntry incoming;
            incoming.graph = a_graph;
            incoming.clips.emplace(std::string(a_clip), std::vector<Binding>{ { a_animation, a_generator, a_data, runtime } });
            CharacterEntry retired;
            std::vector<Binding> retiredBindings;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (!IsCurrent(shard, a_ticket)) {
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
            return { binding.data, binding.runtime, binding.animation };
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
            return binding == bindings.rend() ? BindingView{} : BindingView{ binding->data, binding->runtime, binding->animation };
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
            return binding == bindings.rend() ? BindingView{} : BindingView{ binding->data, binding->runtime, binding->animation };
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

        void Deactivate(Ticket a_ticket, std::string_view a_clip, const void* a_animation,
            const void* a_generator = nullptr)
        {
            const auto a_character = a_ticket.character;
            const auto a_graph = a_ticket.graph;
            Snapshot retired;
            RuntimeOwner retiredRuntime;
            CharacterEntry retiredCharacter;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (!IsCurrent(shard, a_ticket)) {
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
                retiredRuntime = std::move(binding->runtime);
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

        std::vector<RuntimeOwner> DeactivateGenerator(Ticket a_ticket, std::string_view a_clip,
            const void* a_generator)
        {
            const auto a_character = a_ticket.character;
            const auto a_graph = a_ticket.graph;
            std::vector<RuntimeOwner> retiredRuntimes;
            std::vector<Binding> retiredBindings;
            CharacterEntry retiredCharacter;
            auto& shard = GetShard(a_character);
            std::lock_guard guard(shard.mutex);
            if (!IsCurrent(shard, a_ticket)) {
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
        void RetireCharacter(Ticket a_ticket, Visitor&& a_visitor)
        {
            const auto a_character = a_ticket.character;
            typename CharacterMap::node_type retired;
            auto& shard = GetShard(a_character);
            {
                std::lock_guard guard(shard.mutex);
                if (!IsCurrent(shard, a_ticket)) {
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
            std::array<std::unique_lock<std::mutex>, shardCount> guards;
            for (std::size_t i = 0; i < shardCount; ++i) {
                guards[i] = std::unique_lock(shards[i].mutex);
            }
            generation.fetch_add(1, std::memory_order_release);
            for (std::size_t i = 0; i < shardCount; ++i) {
                retiredCharacters[i].swap(shards[i].characters);
            }
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
        using ClipMap = std::unordered_map<std::string, std::vector<Binding>, StringHash, std::equal_to<>>;
        struct CharacterEntry
        {
            const void* graph = nullptr;
            ClipMap clips;
        };
        using CharacterMap = std::unordered_map<const void*, CharacterEntry>;
        struct Lifetime
        {
            const void* graph;
            Generation instance;
        };
        struct alignas(64) Shard
        {
            std::mutex mutex;
            CharacterMap characters;
            std::unordered_map<const void*, Lifetime> lifetimes;
        };

        Generation NextInstance() noexcept
        {
            return nextInstance.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        bool IsCurrent(const Shard& a_shard, const Ticket& a_ticket) const
        {
            const auto life = a_shard.lifetimes.find(a_ticket.character);
            return a_ticket && a_ticket.generation == GetGeneration() && life != a_shard.lifetimes.end() &&
                life->second.instance == a_ticket.instance && life->second.graph == a_ticket.graph;
        }

        Shard& GetShard(const void* a_character) noexcept
        {
            const auto address = reinterpret_cast<std::uintptr_t>(a_character);
            return shards[((address >> 4) ^ (address >> 13)) & (shardCount - 1)];
        }

        std::array<Shard, shardCount> shards;
        std::atomic<Generation> generation{ 0 };
        std::atomic<Generation> nextInstance{ 0 };
    };
}
