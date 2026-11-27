#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace hearthline {

constexpr int kMapSize = 512;
constexpr int kExpeditionSize = 128;
constexpr int kLayerCount = 3;
constexpr int kRiverX = 62;
constexpr int kBridgeY = 64;
constexpr int kCombatTickMs = 50;
constexpr int kAttackCooldownTicks = 8;
constexpr int kDodgeActiveTicks = 6;
constexpr int kDodgeCooldownTicks = 20;
constexpr int kGuardWindupTicks = 12;
constexpr int kGuardRecoveryTicks = 12;

enum class Tile : std::uint8_t { Grass, Tree, Rock, Water, Ruin, Floor };
enum class Phase { Playing, Won, Lost };
enum class GuardMode { Idle, Chase, Windup, Recovery, Return, Defeated };
enum class Structure : std::uint8_t { None, Bridge, Wall, Campfire, Relay };
enum class Action { Gather, BuildBridge, BuildWall, BuildCampfire, PlaceRelay, RotateRelay, Attack, Dodge, Heal, Refuel, Interact, Wait };
struct Point { int x = 0, y = 0; };
struct Inventory { int wood = 0, stone = 0, energy = 0, food = 0; };
struct Input { int dx = 0, dy = 0; bool move = false; };
struct Relay { Point at{}; int direction = 0; };
struct Snapshot {
    Point player{};
    Point guardian{}, guard_target{};
    GuardMode guard_mode = GuardMode::Idle;
    std::uint32_t combat_tick = 0;
    int guard_ticks = 0, attack_ticks = 0, dodge_ticks = 0, dodge_cooldown = 0, hurt_ticks = 0;
    Inventory inventory{};
    int health = 5, guard_health = 3, fuel = 0, night_turns = 0;
    bool bridge = false, beacon = false, fire = false, guardian_defeated = false;
    Phase phase = Phase::Playing;
};
class Game {
public:
    explicit Game(std::uint32_t seed = 1);
    [[nodiscard]] Tile Terrain(int x, int y) const;
    [[nodiscard]] Structure ObjectAt(int x, int y) const;
    [[nodiscard]] bool Discovered(int x, int y) const;
    [[nodiscard]] bool Visible(int x, int y) const;
    [[nodiscard]] bool ResourceAvailable(int index) const;
    [[nodiscard]] bool RotateNearbyRelay();
    [[nodiscard]] int DiscoveredCount() const { return discovered_count_; }
    [[nodiscard]] static Point FireSite() { return {106,65}; }
    [[nodiscard]] bool Walkable(int x, int y) const;
    [[nodiscard]] bool Move(int dx, int dy);
    [[nodiscard]] bool Act(Action action, Point at = {});
    // Exactly one 50 ms combat step. Hosts call this independently of input.
    void TickCombat();
    [[nodiscard]] bool Complete() const { return state_.phase == Phase::Won; }
    [[nodiscard]] const Snapshot& State() const { return state_; }
    [[nodiscard]] const std::vector<Relay>& Relays() const { return relays_; }
    [[nodiscard]] std::uint32_t Seed() const { return seed_; }
    [[nodiscard]] static Point Camp() { return {20, 64}; }
    [[nodiscard]] static Point Guardian() { return {90, 64}; }
    [[nodiscard]] static Point Beacon() { return {108, 64}; }
    [[nodiscard]] static Point Source() { return {103, 64}; }
    [[nodiscard]] static Point Resource(int index);
private:
    void AdvanceTurn();
    void Reveal();
    void Damage();
    bool CombatAttack();
    bool CombatDodge();
    bool LineClear(Point from, Point to) const;
    void StepGuardian(Point target);
    bool Adjacent(Point p) const;
    bool HasStructure(Point p) const;
    std::uint32_t seed_ = 1;
    std::vector<std::uint8_t> terrain_;
    std::vector<Structure> objects_;
    std::vector<std::uint8_t> discovered_;
    int discovered_count_ = 0;
    std::vector<Relay> relays_;
    Snapshot state_{};
    std::array<bool, 6> gathered_{};
};

} // namespace hearthline
