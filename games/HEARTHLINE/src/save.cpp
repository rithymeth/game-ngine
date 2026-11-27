#include "game.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace hearthline {
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint32_t kMagic=0x31534c48u; // HLS1
constexpr std::uint32_t kVersion=1;
constexpr std::size_t kCells=kMapSize*kMapSize;
void Put(Bytes& out,std::uint32_t v) {
    for(unsigned i=0;i<4;++i) out.push_back(static_cast<std::uint8_t>(v>>(8*i)));
}
std::uint32_t Checksum(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash=2166136261u;
    for(auto b:bytes) { hash^=b; hash*=16777619u; }
    return hash;
}
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos=0;
    bool ok=true;
    std::uint32_t U32() {
        if(data.size()-pos<4) { ok=false; return 0; }
        std::uint32_t v=0;
        for(unsigned i=0;i<4;++i) v|=static_cast<std::uint32_t>(data[pos++])<<(8*i);
        return v;
    }
    int Bounded(int max) {
        const auto v=U32();
        if(v>static_cast<std::uint32_t>(max)) { ok=false; return 0; }
        return static_cast<int>(v);
    }
    Point Position() { return {Bounded(kMapSize-1),Bounded(kMapSize-1)}; }
    std::uint8_t Byte() {
        if(pos>=data.size()) { ok=false; return 0; }
        return data[pos++];
    }
};
bool Same(Point a,Point b) { return a.x==b.x && a.y==b.y; }
int Distance(Point a,Point b) { return std::abs(a.x-b.x)+std::abs(a.y-b.y); }
}

std::vector<std::uint8_t> Game::Save() const {
    Bytes out; out.reserve(kMaxSaveBytes);
    auto put=[&](std::uint32_t v){Put(out,v);};
    auto point=[&](Point p){put(p.x);put(p.y);};
    const auto& s=state_;
    put(kMagic); put(kVersion); put(kMapSize); put(seed_);
    point(s.player); put(s.facing); point(s.guardian); point(s.guard_target);
    put(static_cast<std::uint32_t>(s.guard_mode)); put(s.combat_tick);
    put(s.guard_ticks); put(s.attack_ticks); put(s.dodge_ticks); put(s.dodge_cooldown); put(s.hurt_ticks);
    put(s.inventory.wood); put(s.inventory.stone); put(s.inventory.energy); put(s.inventory.food);
    put(s.health); put(s.guard_health); put(s.fuel); put(s.night_turns);
    put(s.bridge); put(s.beacon); put(s.fire); put(s.guardian_defeated);
    put(static_cast<std::uint32_t>(s.phase));
    for(bool taken:gathered_) put(taken);
    put(static_cast<std::uint32_t>(relays_.size()));
    for(const auto& relay:relays_) { point(relay.at); put(relay.direction); }
    out.insert(out.end(),terrain_.begin(),terrain_.end());
    for(auto object:objects_) out.push_back(static_cast<std::uint8_t>(object));
    out.insert(out.end(),discovered_.begin(),discovered_.end());
    put(Checksum(out));
    return out;
}

bool Game::Load(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<kCells*3+4 || bytes.size()>kMaxSaveBytes) return false;
    const auto payload=bytes.first(bytes.size()-4);
    Reader check{bytes.last(4)};
    if(check.U32()!=Checksum(payload)) return false;
    Reader r{payload};
    if(r.U32()!=kMagic || r.U32()!=kVersion || r.U32()!=kMapSize) return false;
    Game next(r.U32());
    auto& s=next.state_;
    s.player=r.Position(); s.facing=r.Bounded(3); s.guardian=r.Position(); s.guard_target=r.Position();
    s.guard_mode=static_cast<GuardMode>(r.Bounded(static_cast<int>(GuardMode::Defeated)));
    s.combat_tick=r.U32(); s.guard_ticks=r.Bounded(kGuardWindupTicks);
    s.attack_ticks=r.Bounded(kAttackCooldownTicks); s.dodge_ticks=r.Bounded(kDodgeActiveTicks);
    s.dodge_cooldown=r.Bounded(kDodgeCooldownTicks); s.hurt_ticks=r.Bounded(6);
    s.inventory={r.Bounded(1000000),r.Bounded(1000000),r.Bounded(2),r.Bounded(9)};
    s.health=r.Bounded(5); s.guard_health=r.Bounded(3); s.fuel=r.Bounded(12000000); s.night_turns=r.Bounded(24);
    s.bridge=r.Bounded(1)!=0; s.beacon=r.Bounded(1)!=0; s.fire=r.Bounded(1)!=0;
    s.guardian_defeated=r.Bounded(1)!=0; s.phase=static_cast<Phase>(r.Bounded(static_cast<int>(Phase::Lost)));
    for(auto& taken:next.gathered_) taken=r.Bounded(1)!=0;
    const int count=r.Bounded(2);
    for(int i=0;i<count;++i) {
        const Point p=r.Position(); const int direction=r.Bounded(3);
        if(p.y!=64 || (p.x!=105 && p.x!=107) ||
           std::any_of(next.relays_.begin(),next.relays_.end(),[&](const Relay& a){return Same(a.at,p);})) return false;
        next.relays_.push_back({p,direction});
    }
    if(!r.ok || payload.size()-r.pos!=3*kCells) return false;
    for(auto& tile:next.terrain_) {
        const auto value=r.Byte();
        if(value!=tile) {
            if((tile!=static_cast<std::uint8_t>(Tile::Tree) && tile!=static_cast<std::uint8_t>(Tile::Rock)) ||
               value!=static_cast<std::uint8_t>(Tile::Grass)) return false;
            ++next.harvested_count_;
        }
        tile=value;
    }
    int bridges=0,relays=0,fires=0,caches=0;
    for(std::size_t i=0;i<kCells;++i) {
        const Point p{static_cast<int>(i%kMapSize),static_cast<int>(i/kMapSize)};
        const auto initial=next.objects_[i];
        const auto value=r.Byte();
        if(value>static_cast<std::uint8_t>(Structure::StoneFloor)) return false;
        const auto object=static_cast<Structure>(value);
        if(initial==Structure::SupplyCache) {
            if(object!=Structure::SupplyCache && object!=Structure::OpenedCache) return false;
        } else if(object==Structure::SupplyCache || object==Structure::OpenedCache) return false;
        switch(object) {
        case Structure::Bridge:
            if(!s.bridge || p.y!=kBridgeY || (p.x!=kRiverX && p.x!=kRiverX+1)) return false;
            ++bridges; break;
        case Structure::Campfire:
            if(!s.fire || !Same(p,FireSite())) return false;
            ++fires; break;
        case Structure::Relay:
            if(std::none_of(next.relays_.begin(),next.relays_.end(),[&](const Relay& a){return Same(a.at,p);})) return false;
            ++relays; break;
        case Structure::Wall: case Structure::StoneFloor:
            if(!next.ConstructionSite(p)) return false;
            break;
        case Structure::OpenedCache: ++next.opened_caches_; ++caches; break;
        case Structure::SupplyCache: ++caches; break;
        case Structure::None: break;
        }
        next.objects_[i]=object;
    }
    if(bridges!=(s.bridge?2:0) || fires!=(s.fire?1:0) || relays!=count || caches!=4) return false;
    next.discovered_count_=0;
    for(auto& seen:next.discovered_) {
        seen=r.Byte(); if(seen>1) return false;
        next.discovered_count_+=seen;
    }
    if(!r.ok || r.pos!=payload.size()) return false;
    if(!next.Walkable(s.player.x,s.player.y) || !next.Walkable(s.guardian.x,s.guardian.y) ||
       Distance(s.guardian,Guardian())>8 || (!s.guardian_defeated && Same(s.guardian,s.player))) return false;
    if(s.guardian_defeated!=(s.guard_health==0) || s.guardian_defeated!=(s.guard_mode==GuardMode::Defeated)) return false;
    if((s.guard_mode==GuardMode::Windup || s.guard_mode==GuardMode::Recovery) && s.guard_ticks==0) return false;
    if(s.beacon && (!s.guardian_defeated || count!=2 ||
       std::any_of(next.relays_.begin(),next.relays_.end(),[](const Relay& a){return a.direction!=0;}))) return false;
    if((count>0 && !s.guardian_defeated) || (s.fire && !s.beacon) ||
       (!s.fire && (s.fuel!=0 || s.night_turns!=0)) ||
       ((s.phase==Phase::Lost)!=(s.health==0)) ||
       (s.phase==Phase::Won && s.night_turns!=24) ||
       (s.phase==Phase::Playing && s.night_turns>=24)) return false;
    const int revealed=next.discovered_count_;
    next.Reveal();
    if(next.discovered_count_!=revealed) return false;
    *this=std::move(next); // No live state is modified until every check succeeds.
    return true;
}

bool Game::SaveFile(const std::string& path) const {
    const auto bytes=Save();
    const auto temp=path+".tmp";
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);
    if(!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    out.close();
    std::error_code ec;
    if(out) {
        std::filesystem::rename(temp,path,ec);
        if(!ec) return true;
    }
    std::filesystem::remove(temp,ec);
    return false;
}

bool Game::LoadFile(const std::string& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in) return false;
    const auto size=in.tellg();
    if(size<0 || size>static_cast<std::streamoff>(kMaxSaveBytes)) return false;
    Bytes bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) return false;
    return Load(bytes);
}
} // namespace hearthline
