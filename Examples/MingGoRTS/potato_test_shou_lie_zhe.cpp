#include "Campaign/Characters/Character.h"
#include "Gameplay/Doctrine/Doctrine.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path repo = fs::path(__FILE__)
                              .parent_path()
                              .parent_path()
                              .parent_path();

    Potato::Campaign::CharacterLibrary characters;
    const auto loaded = Potato::Campaign::CharacterLibrary::Load(
        repo / "assets" / "characters", characters);
    Check(loaded.ok, "character assets load");

    const auto* character = characters.Find("shou_lie_zhe");
    Check(character != nullptr, "首獵者 is registered");
    if (character == nullptr) return 1;
    Check(character->name == "首獵者" && character->deck.size() == 3 &&
              character->deck.front() == "ord_tonfa_lock",
          "首獵者 starts with the signature 雙棍合鎖 card");

    const auto doctrine = Potato::Gameplay::DoctrineLibrary::Load(
        (repo / "assets" / "doctrine" / "core.json").string());
    Check(doctrine.ok(), "doctrine assets load");
    if (doctrine.ok()) {
        const auto* signature = doctrine.value.Find("ord_tonfa_lock");
        Check(signature != nullptr && signature->name == "雙棍合鎖" &&
                  signature->trigger ==
                      Potato::Gameplay::TriggerKind::EnemyAdjacent &&
                  signature->condition ==
                      Potato::Gameplay::ConditionKind::CpAtLeast &&
                  signature->conditionParam == 3 &&
                  signature->action == Potato::Gameplay::ActionKind::Hold &&
                  signature->modifier ==
                      Potato::Gameplay::ModifierKind::AttackBoost &&
                  signature->modifierParam == 15,
              "signature card uses the intended close-range control rules");
        Check(Potato::Gameplay::SquadSheet::Build(
                  doctrine.value, character->deck).ok(),
              "首獵者 starting deck resolves in the battle doctrine pool");
    }

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n",
                failures);
    return failures == 0 ? 0 : 1;
}
