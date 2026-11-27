#include "game.h"
#include <algorithm>
#include <cmath>

namespace hearthline {
namespace {
constexpr Point kResources[] = {{21, 64}, {22, 64}, {23, 64}, {24, 64}, {25, 64}, {26, 64}};
std::uint32_t Hash(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16; return x;
}
int Distance(Point a, Point b) { return std::abs(a.x-b.x) + std::abs(a.y-b.y); }
bool Inside(Point p) { return p.x >= 0 && p.x < kMapSize && p.y >= 0 && p.y < kMapSize; }
}

Point Game::Resource(int index) {
    return index < 0 || index >= 6 ? Point{-1,-1} : kResources[index];
}

Game::Game(std::uint32_t seed) : seed_(seed), terrain_(kMapSize * kMapSize) {
    for (int y=0; y<kMapSize; ++y) for (int x=0; x<kMapSize; ++x) {
        const auto r=Hash(seed ^ Hash(static_cast<std::uint32_t>(x)*73856093u) ^
                          Hash(static_cast<std::uint32_t>(y)*19349663u)) % 100u;
        Tile tile = r < 10 ? Tile::Tree : r < 14 ? Tile::Rock : Tile::Grass;
        if (x>=kRiverX && x<=kRiverX+1) tile=Tile::Water;
        if (x>=99 && x<=113 && y>=57 && y<=71) tile=Tile::Ruin;
        if (y>=62 && y<=66 && x>=15 && x<=113) tile=Tile::Floor;
        if (x>=kRiverX && x<=kRiverX+1) tile=Tile::Water;
        terrain_[static_cast<std::size_t>(y)*kMapSize+x] = static_cast<std::uint8_t>(tile);
    }
    state_.player=Camp();
}
Tile Game::Terrain(int x,int y) const {
    if (!Inside({x,y})) return Tile::Water;
    return static_cast<Tile>(terrain_[static_cast<std::size_t>(y)*kMapSize+x]);
}
bool Game::HasStructure(Point p) const {
    return std::find_if(walls_.begin(),walls_.end(),[&](Point q){return q.x==p.x && q.y==p.y;}) != walls_.end();
}
bool Game::Walkable(int x,int y) const {
    const Point p{x,y};
    if(!Inside(p) || HasStructure(p)) return false;
    const Tile t=Terrain(x,y);
    if(t==Tile::Water) return state_.bridge && y==kBridgeY && (x==kRiverX || x==kRiverX+1);
    return t==Tile::Grass || t==Tile::Floor || t==Tile::Ruin;
}
bool Game::Adjacent(Point p) const { return Distance(state_.player,p)<=1; }
void Game::AdvanceTurn() {
    if (state_.phase!=Phase::Playing) return;
    if(attack_cooldown_>0) --attack_cooldown_;
    if(enemy_cooldown_>0) --enemy_cooldown_;
    if(dodge_turns_>0) --dodge_turns_;
    if(!state_.beacon || !state_.fire) return;
    ++state_.night_turns;
    if(state_.fuel>0) --state_.fuel;
    if(state_.fuel==0 && state_.night_turns%5==0) {
        state_.health=std::max(0,state_.health-1);
        if(state_.health==0) state_.phase=Phase::Lost;
    }
    if(state_.night_turns>=24 && state_.health>0) state_.phase=Phase::Won;
}
bool Game::Move(int dx,int dy) {
    if(state_.phase!=Phase::Playing || std::abs(dx)+std::abs(dy)!=1) return false;
    Point next{state_.player.x+dx,state_.player.y+dy};
    if(!Walkable(next.x,next.y)) return false;
    if(!state_.guardian_defeated && Distance(next,Guardian())<=1) return false;
    state_.player=next;
    AdvanceTurn();
    return true;
}
bool Game::Act(Action action, Point at) {
    if(state_.phase!=Phase::Playing) return false;
    auto& s=state_;
    bool accepted=false;
    switch(action) {
    case Action::Gather:
        for(int i=0;i<6;++i) if(!gathered_[i] && Adjacent(Resource(i))) {
            gathered_[i]=true;
            if(i<3) s.inventory.wood+=4;
            else if(i==3) s.inventory.stone+=4;
            else if(i==4) s.inventory.energy+=2;
            else s.inventory.food+=3;
            accepted=true; break;
        }
        break;
    case Action::BuildBridge:
        if(!s.bridge && s.player.x<=kRiverX-1 && std::abs(s.player.y-kBridgeY)<=1 && s.inventory.wood>=4) {
            s.inventory.wood-=4; s.bridge=true; accepted=true;
        }
        break;
    case Action::BuildWall:
        if(Adjacent(at) && Walkable(at.x,at.y) && Distance(at,Beacon())>2 && s.inventory.wood>=2) {
            s.inventory.wood-=2; walls_.push_back(at); accepted=true;
        }
        break;
    case Action::BuildCampfire:
        if(!s.fire && s.beacon && Distance(s.player,Beacon())<=4 && s.inventory.wood>=2 && s.inventory.stone>=2) {
            s.inventory.wood-=2; s.inventory.stone-=2; s.fire=true; accepted=true;
        }
        break;
    case Action::PlaceRelay:
        if(Distance(s.player,Beacon())<=9 && s.inventory.energy>=1 &&
           at.y==64 && at.x>=104 && at.x<=107 &&
           std::none_of(relays_.begin(),relays_.end(),[&](const Relay& r){return Distance(r.at,at)==0;})) {
            s.inventory.energy-=1; relays_.push_back({at,0}); accepted=true;
        }
        break;
    case Action::RotateRelay:
        for(auto& r:relays_) if(Distance(r.at,at)==0 && Adjacent(at)) {
            r.direction=(r.direction+1)%4; accepted=true; break;
        }
        break;
    case Action::Attack:
        if(!s.guardian_defeated && Distance(s.player,Guardian())<=3 && attack_cooldown_==0) {
            --s.guard_health; attack_cooldown_=2; accepted=true;
            if(s.guard_health<=0) s.guardian_defeated=true;
            else if(dodge_turns_==0 && enemy_cooldown_==0) {
                --s.health; enemy_cooldown_=2;
                if(s.health<=0) s.phase=Phase::Lost;
            }
        }
        break;
    case Action::Dodge: dodge_turns_=3; accepted=true; break;
    case Action::Heal:
        if(s.inventory.food>0 && s.health<5) {
            --s.inventory.food; s.health=std::min(5,s.health+2); accepted=true;
        }
        break;
    case Action::Refuel:
        if(s.fire && Distance(s.player,Beacon())<=4 && s.inventory.wood>0) {
            --s.inventory.wood; s.fuel+=12; accepted=true;
        }
        break;
    case Action::Interact:
        if(!s.beacon && s.guardian_defeated && Distance(s.player,Beacon())<=4 && relays_.size()==2) {
            // Both relays face east (0) and form a continuous two-node circuit.
            const bool first=std::any_of(relays_.begin(),relays_.end(),[](const Relay& r){return r.at.x==105 && r.at.y==64 && r.direction==0;});
            const bool second=std::any_of(relays_.begin(),relays_.end(),[](const Relay& r){return r.at.x==107 && r.at.y==64 && r.direction==0;});
            if(first && second) { s.beacon=true; accepted=true; }
        }
        break;
    }
    if(accepted && s.phase==Phase::Playing) AdvanceTurn();
    return accepted;
}

} // namespace hearthline
