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

enum class Tile : std::uint8_t { Grass, Tree, Rock, Water, Ruin, Floor };
enum class Phase { Playing, Won, Lost };
enum class Structure { Bridge, Wall, Campfire, Relay };
enum class Action { Gather, BuildBridge, BuildWall, BuildCampfire, PlaceRelay, RotateRelay, Attack, Dodge, Heal, Refuel, Interact };
struct Point { int x = 0, y = 0; };
struct Inventory { int wood = 0, stone = 0, energy = 0, food = 0; };
struct Input { int dx = 0, dy = 0; bool move = false; };
struct Relay { Point at{}; int direction = 0; };
struct Snapshot {
    Point player{};
    Inventory inventory{};
    int health = 5, guard_health = 3, fuel = 0, night_turns = 0;
    bool bridge = false, beacon = false, fire = false, guardian_defeated = false;
    Phase phase = Phase::Playing;
};
class Game {
public:
    explicit Game(std::uint32_t seed = 1);
    [[nodiscard]] Tile Terrain(int x, int y) const;
    [[nodiscard]] bool Walkable(int x, int y) const;
    [[nodiscard]] bool Move(int dx, int dy);
    [[nodiscard]] bool Act(Action action, Point at = {});
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
    bool Adjacent(Point p) const;
    bool HasStructure(Point p) const;
    std::uint32_t seed_ = 1;
    std::vector<std::uint8_t> terrain_;
    std::vector<Relay> relays_;
    std::vector<Point> walls_;
    Snapshot state_{};
    int dodge_turns_ = 0, attack_cooldown_ = 0, enemy_cooldown_ = 0;
    std::array<bool, 6> gathered_{};
};

} // namespace hearthline
