#pragma once

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace logger = SKSE::log;

// THE ONLY "IS SHE DEAD?" TEST IN THIS PLUGIN. Never call Actor::IsDead() - in a VR build it answers TRUE for some
// living NPCs (restrained captives: Actor::IsDead() returns TRUE for RESTRAINED actors). The life
// state is a plain field read through the runtime-proof accessor. Dead = EXACTLY kDying (1) or kDead (2): the field has
// read garbage in VR for a living NPC, so never test `!= kAlive` - a garbage read falls through to ALIVE.
// (Copied from the VRCD AddOn's PCH.h, with AsActorState() so the same line is correct on SE/AE/VR after M0.)
namespace sbko
{
    [[nodiscard]] inline bool IsDeadNow(RE::Actor* a_actor) noexcept
    {
        if (!a_actor) return true;
        const auto* st = a_actor->AsActorState();
        if (!st) return true;
        switch (st->GetLifeState()) {
            case RE::ACTOR_LIFE_STATE::kDying:
            case RE::ACTOR_LIFE_STATE::kDead:
                return true;
            default:
                return false;
        }
    }
}
