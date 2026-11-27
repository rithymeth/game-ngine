#include "game.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace hearthline;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << " failed\n"; \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

void To(Game& g,int x,int y) {
    int guard=10000;
    while(g.State().player.x!=x && --guard>0) {
        const int dx=g.State().player.x<x?1:-1;
        CHECK(g.Move(dx,0));
    }
    while(g.State().player.y!=y && --guard>0) {
        const int dy=g.State().player.y<y?1:-1;
        CHECK(g.Move(0,dy));
    }
    CHECK(guard>0);
}
void Tick(Game& g,int ticks) {
    for(int i=0;i<ticks;++i) g.TickCombat();
}
void Windup(Game& g) {
    for(int i=0;i<120 && g.State().guard_mode!=GuardMode::Windup;++i) g.TickCombat();
    CHECK(g.State().guard_mode==GuardMode::Windup);
    CHECK(g.State().guard_ticks==kGuardWindupTicks);
}
void CheckRoundTrip(const Game& original) {
    const auto bytes=original.Save();
    Game restored(9999);
    CHECK(restored.Load(bytes));
    CHECK(restored.Save()==bytes);
    CHECK(restored.Seed()==original.Seed());
    CHECK(restored.DiscoveredCount()==original.DiscoveredCount());
    CHECK(restored.HarvestedCount()==original.HarvestedCount());
    CHECK(restored.OpenedCacheCount()==original.OpenedCacheCount());
}
void FixChecksum(std::vector<std::uint8_t>& bytes) {
    std::uint32_t hash=2166136261u;
    for(std::size_t i=0;i<bytes.size()-4;++i) { hash^=bytes[i]; hash*=16777619u; }
    for(unsigned i=0;i<4;++i) bytes[bytes.size()-4+i]=static_cast<std::uint8_t>(hash>>(8*i));
}
void TestConstructionAndSave(unsigned seed) {
    Game g(seed);
    for(int i=0;i<6;++i) { To(g,Game::Resource(i).x-1,64); CHECK(g.Act(Action::Gather)); }
    CHECK(!g.Act(Action::BuildWall,g.BuildTarget())); // protected road
    CHECK(!g.Act(Action::BuildFloor,g.State().player));
    bool built=false;
    for(int x=30;x<=35;++x) {
        To(g,x,64);
        if(g.Terrain(x,61)!=Tile::Grass) continue;
        To(g,x,62);
        CHECK(g.BuildTarget().x==x && g.BuildTarget().y==61);
        const auto target=g.BuildTarget();
        const int wood=g.State().inventory.wood,stone=g.State().inventory.stone;
        CHECK(g.Act(Action::BuildWall,target)); // building allowed before beacon
        CHECK(g.State().inventory.wood==wood-2);
        CHECK(!g.Walkable(x,61));
        CHECK(!g.Move(0,-1));
        CHECK(!g.Act(Action::BuildFloor,target));
        CheckRoundTrip(g);
        CHECK(g.Act(Action::BuildWall,target));
        CHECK(g.State().inventory.wood==wood);
        CHECK(g.Walkable(x,61));
        CHECK(g.Act(Action::BuildFloor,target));
        CHECK(g.State().inventory.stone==stone-1);
        CHECK(g.ObjectAt(x,61)==Structure::StoneFloor && g.Walkable(x,61));
        CheckRoundTrip(g);
        CHECK(g.Act(Action::BuildFloor,target));
        CHECK(g.State().inventory.stone==stone);
        const auto before=g.BuildTarget();
        for(int i=0;i<4;++i) g.RotateBuildFacing();
        CHECK(g.BuildTarget().x==before.x && g.BuildTarget().y==before.y);
        built=true; break;
    }
    CHECK(built);
    const auto good=g.Save();
    auto bad=good;
    bad[100]^=1;
    CHECK(!g.Load(bad)); CHECK(g.Save()==good);
    CHECK(!g.Load(std::span<const std::uint8_t>(good).first(good.size()-1))); CHECK(g.Save()==good);
    bad=good; bad[4]=2; FixChecksum(bad); // unknown format version
    CHECK(!g.Load(bad)); CHECK(g.Save()==good);
    bad=good; bad[16]=255; bad[17]=255; FixChecksum(bad); // out-of-bounds player
    CHECK(!g.Load(bad)); CHECK(g.Save()==good);
    bad.assign(kMaxSaveBytes+1,0);
    CHECK(!g.Load(bad)); CHECK(g.Save()==good);
    if(seed==1) {
        const std::string path="hearthline-test-save.bin";
        std::error_code ec;
        std::filesystem::remove(path,ec);
        CHECK(g.SaveFile(path));
        Game restored(33);
        CHECK(restored.LoadFile(path)); CHECK(restored.Save()==good);
        CHECK(g.Move(0,1)); CHECK(g.SaveFile(path));
        CHECK(restored.LoadFile(path)); CHECK(restored.Save()==g.Save());
        { std::ofstream out(path,std::ios::binary|std::ios::trunc); out<<"broken"; }
        const auto previous=restored.Save();
        CHECK(!restored.LoadFile(path)); CHECK(restored.Save()==previous);
        std::filesystem::remove(path,ec);
        CHECK(!restored.LoadFile(path));
        std::cout<<"Save file bytes: "<<good.size()<<"\n";
    }
}
void TestExploration(unsigned seed) {
    Game g(seed);
    // Authored starting path and side caches are traversable on every seed.
    const auto& caches=Game::Caches();
    const auto first=caches[0].at;
    CHECK(g.ObjectAt(first.x,first.y)==Structure::SupplyCache);
    CHECK(!g.Discovered(first.x,first.y));
    CHECK(!g.Act(Action::Interact));
    auto claim=[&](const SupplyCache& cache) {
        To(g,cache.at.x,64);
        To(g,cache.at.x,cache.at.y);
        CHECK(g.Discovered(cache.at.x,cache.at.y));
        CHECK(g.ObjectAt(cache.at.x,cache.at.y)==Structure::SupplyCache);
        const auto before=g.State().inventory;
        CHECK(g.Act(Action::Interact));
        CHECK(g.ObjectAt(cache.at.x,cache.at.y)==Structure::OpenedCache);
        CHECK(g.State().inventory.wood==before.wood+cache.loot.wood);
        CHECK(g.State().inventory.stone==before.stone+cache.loot.stone);
        CHECK(g.State().inventory.food==before.food+cache.loot.food);
        CHECK(!g.Act(Action::Interact));
        To(g,cache.at.x,64);
    };
    claim(caches[0]); claim(caches[1]);
    To(g,61,64); CHECK(g.Act(Action::BuildBridge));
    claim(caches[2]);
    // Explicitly defeat the guardian before reaching the final cache.
    To(g,82,64); Windup(g);
    for(int i=0;i<3;++i) { CHECK(g.Act(Action::Attack)); Tick(g,kAttackCooldownTicks); }
    CHECK(g.State().guardian_defeated);
    claim(caches[3]);
    CHECK(g.OpenedCacheCount()==4);
    CheckRoundTrip(g);
    CHECK(g.State().inventory.wood==7);
    CHECK(g.State().inventory.stone==7);
    CHECK(g.State().inventory.food==6);

    // Find a reachable natural resource just off the safe road, away from
    // starter pickups. A gather changes that exact tile and adds material.
    Game wild(seed);
    bool tested=false;
    for(int x=30;x<61 && !tested;++x) {
        To(wild,x,64);
        for(int dy:{-1,1}) {
            const int y=64+2*dy;
            const auto tile=wild.Terrain(x,y+dy);
            if(tile!=Tile::Tree && tile!=Tile::Rock) continue;
            To(wild,x,y);
            const auto before=wild.State().inventory;
            CHECK(wild.Act(Action::Gather));
            CHECK(wild.HarvestedCount()==1);
            CHECK(wild.Terrain(x,y+dy)==Tile::Grass);
            CHECK(wild.Walkable(x,y+dy));
            CHECK(wild.State().inventory.wood==before.wood+(tile==Tile::Tree?2:0));
            CHECK(wild.State().inventory.stone==before.stone+(tile==Tile::Rock?2:0));
            To(wild,x,64);
            tested=true; break;
        }
    }
    CHECK(tested);
    CheckRoundTrip(wild);
}
void Test(unsigned seed) {
    Game g(seed);
    CHECK(g.State().player.x==20);
    for(int i=0;i<6;i++) {
        To(g,Game::Resource(i).x-1,64);
        CHECK(g.ResourceAvailable(i));
        CHECK(g.Act(Action::Gather));
        CHECK(!g.ResourceAvailable(i));
    }
    CHECK(g.State().inventory.wood==12);
    CHECK(g.State().inventory.energy==2);
    CHECK(!g.Act(Action::BuildBridge));
    CHECK(!g.Act(Action::BuildWall,g.State().player));
    To(g,61,64);
    CHECK(!g.Move(1,0));
    CHECK(g.Act(Action::BuildBridge));
    CHECK(g.ObjectAt(62,64)==Structure::Bridge);
    CHECK(g.ObjectAt(63,64)==Structure::Bridge);
    CHECK(g.Move(1,0));
    CHECK(g.Move(1,0));
    To(g,82,64);
    CHECK(g.Act(Action::Attack)); // miss consumes cooldown
    CHECK(g.State().guard_health==3);
    CHECK(!g.Act(Action::Attack));
    Windup(g);
    CHECK(g.State().guardian.x==84 && g.State().guardian.y==64);
    CHECK(g.State().guard_target.x==82 && g.State().guard_target.y==64);
    CHECK(g.State().attack_ticks==0);
    CheckRoundTrip(g);

    Game hit=g;
    Tick(hit,kGuardWindupTicks-1);
    CHECK(hit.State().health==5);
    hit.TickCombat();
    CHECK(hit.State().health==4);
    CHECK(hit.State().guard_mode==GuardMode::Recovery);
    CHECK(hit.Act(Action::Heal));
    CHECK(hit.State().health==5 && hit.State().inventory.food==2);
    Game evade=g;
    Tick(evade,kGuardWindupTicks-2);
    CHECK(evade.Act(Action::Dodge));
    CHECK(!evade.Act(Action::Dodge));
    Tick(evade,2);
    CHECK(evade.State().health==5);
    CHECK(evade.State().guard_mode==GuardMode::Recovery);
    Tick(evade,kDodgeCooldownTicks-2);
    CHECK(evade.State().dodge_cooldown==0);
    CHECK(evade.Act(Action::Dodge));
    Game early=g;
    CHECK(early.Act(Action::Dodge));
    Tick(early,kGuardWindupTicks);
    CHECK(early.State().health==4); // mistimed dodge expires before impact
    Game retreat=g;
    To(retreat,79,64);
    Tick(retreat,120);
    CHECK(retreat.State().health==5);
    CHECK(retreat.State().guardian.x==Game::Guardian().x);
    CHECK(retreat.State().guard_mode==GuardMode::Idle);
    Game idle=g;
    Tick(idle,240);
    CHECK(idle.State().phase==Phase::Lost); // enemy attacks even without input
    CheckRoundTrip(idle);
    const auto frozen=idle.State().combat_tick;
    Tick(idle,10);
    CHECK(idle.State().combat_tick==frozen);

    CHECK(g.Act(Action::Attack));
    CHECK(g.State().guard_health==2);
    CHECK(!g.Act(Action::Attack));
    Tick(g,kAttackCooldownTicks);
    CHECK(g.Act(Action::Attack));
    CHECK(g.State().guard_health==1);
    CHECK(g.Act(Action::Dodge));
    Tick(g,kAttackCooldownTicks);
    CHECK(g.State().health==5);
    CHECK(g.Act(Action::Attack));
    CHECK(g.State().guardian_defeated);
    CHECK(g.State().guard_mode==GuardMode::Defeated);
    Tick(g,100);
    CHECK(g.State().health==5);
    To(g,104,64);
    CHECK(!g.Act(Action::PlaceRelay,{104,64}));
    CHECK(!g.Act(Action::PlaceRelay,{107,64}));
    CHECK(g.Act(Action::PlaceRelay,{105,64}));
    CHECK(g.Relays()[0].direction==3);
    CheckRoundTrip(g);
    CHECK(g.RotateNearbyRelay());
    CHECK(g.Relays()[0].direction==0);
    To(g,106,64);
    CHECK(g.Act(Action::PlaceRelay,{107,64}));
    To(g,108,64);
    CHECK(!g.Act(Action::Interact));
    CHECK(g.RotateNearbyRelay());
    CHECK(g.Act(Action::Interact));
    CHECK(!g.RotateNearbyRelay());
    CHECK(g.State().beacon);
    CHECK(g.Act(Action::BuildCampfire));
    CHECK(g.ObjectAt(Game::FireSite().x,Game::FireSite().y)==Structure::Campfire);
    Game cold=g;
    for(int i=0;i<24 && cold.State().phase==Phase::Playing;++i) CHECK(cold.Act(Action::Wait));
    CHECK(cold.State().phase==Phase::Lost);
    const auto night=g.State().night_turns;
    Tick(g,200);
    CHECK(g.State().night_turns==night); // survival remains action-based
    constexpr int kMaxNightActions=24;
    for(int n=0; n<kMaxNightActions && g.State().phase==Phase::Playing; ++n) {
        if(g.State().fuel<3 && g.State().inventory.wood>0)
            CHECK(g.Act(Action::Refuel));
        else CHECK(g.Act(Action::Wait));
    }
    CHECK(g.Complete());
    CheckRoundTrip(g);
}
int main(){
    for(unsigned seed=1;seed<=64;++seed) { Test(seed); TestExploration(seed); TestConstructionAndSave(seed); }
    Game g(7);
    CHECK(g.Terrain(62,64)==Tile::Water);
    CHECK(g.Discovered(20,64));
    CHECK(!g.Discovered(80,64));
    const int before=g.DiscoveredCount();
    To(g,35,64);
    CHECK(g.DiscoveredCount()>before);
    CHECK(g.Discovered(20,64));
    CHECK(!g.Visible(20,64));
    CHECK(!g.Walkable(-1,64));
    CHECK(!g.Act(Action::BuildBridge));
    CHECK(!g.Act(Action::PlaceRelay,{105,64}));
    std::cout<<"Hearthline 64 seeded expeditions, exploration, construction and persistence scenarios passed\n";
}
