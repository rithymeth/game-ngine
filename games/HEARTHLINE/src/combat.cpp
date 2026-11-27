#include "game.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace hearthline {
namespace {
constexpr int kGuardLeash=8;
constexpr int kAggroRange=8;
constexpr int kStepTicks=5;
int Distance(Point a,Point b) { return std::abs(a.x-b.x)+std::abs(a.y-b.y); }
void Countdown(int& ticks) { if(ticks>0) --ticks; }
}

void Game::Damage() {
    auto& s=state_;
    s.health=std::max(0,s.health-1);
    s.hurt_ticks=6;
    if(s.health==0) s.phase=Phase::Lost;
}

bool Game::LineClear(Point from,Point to) const {
    const int dx=std::abs(to.x-from.x),dy=-std::abs(to.y-from.y);
    const int sx=from.x<to.x?1:-1,sy=from.y<to.y?1:-1;
    int error=dx+dy;
    while(Distance(from,to)>0) {
        const int e=2*error;
        Point next=from;
        if(e>=dy) { error+=dy; next.x+=sx; }
        if(e<=dx) { error+=dx; next.y+=sy; }
        if(next.x!=from.x && next.y!=from.y &&
           !Walkable(next.x,from.y) && !Walkable(from.x,next.y)) return false;
        if(!Walkable(next.x,next.y)) return false;
        from=next;
    }
    return true;
}

bool Game::CombatAttack() {
    auto& s=state_;
    if(s.guardian_defeated || s.attack_ticks!=0) return false;
    s.attack_ticks=kAttackCooldownTicks;
    if(Distance(s.player,s.guardian)<=2 && LineClear(s.player,s.guardian)) {
        --s.guard_health;
        if(s.guard_health==0) {
            s.guardian_defeated=true;
            s.guard_mode=GuardMode::Defeated;
            s.guard_ticks=0;
        }
    }
    return true; // A missed swing still consumes the cooldown.
}

bool Game::CombatDodge() {
    auto& s=state_;
    if(s.guardian_defeated) return false;
    if(s.dodge_cooldown!=0) return false;
    s.dodge_ticks=kDodgeActiveTicks;
    s.dodge_cooldown=kDodgeCooldownTicks;
    return true;
}

void Game::StepGuardian(Point target) {
    auto& s=state_;
    // Bounded breadth-first search within the guardian's leash. The guardian
    // never enters the player's tile, water, walls or terrain obstacles.
    constexpr int side=2*kGuardLeash+1;
    constexpr int cells=side*side;
    const Point home=Guardian();
    auto index=[&](Point p) { return (p.y-home.y+kGuardLeash)*side+p.x-home.x+kGuardLeash; };
    auto point=[&](int i) { return Point{home.x-kGuardLeash+i%side,home.y-kGuardLeash+i/side}; };
    std::array<int,cells> first{},queue{};
    first.fill(-1);
    int head=0,tail=0;
    const int start=index(s.guardian);
    first[start]=start; queue[tail++]=start;
    int best=start,best_distance=Distance(s.guardian,target);
    constexpr Point directions[]={{1,0},{0,1},{-1,0},{0,-1}};
    while(head<tail) {
        const int current=queue[head++];
        const Point at=point(current);
        const int d=Distance(at,target);
        if(d<best_distance) { best=current; best_distance=d; }
        if(d==0) break;
        for(auto dir:directions) {
            const Point next{at.x+dir.x,at.y+dir.y};
            if(Distance(next,home)>kGuardLeash || Distance(next,s.player)==0 ||
               !Walkable(next.x,next.y)) continue;
            const int n=index(next);
            if(first[n]>=0) continue;
            first[n]=current==start?n:first[current];
            queue[tail++]=n;
        }
    }
    if(best!=start) s.guardian=point(first[best]);
}

void Game::TickCombat() {
    auto& s=state_;
    if(s.phase!=Phase::Playing) return;
    ++s.combat_tick;
    const bool evading=s.dodge_ticks>0;
    Countdown(s.attack_ticks); Countdown(s.dodge_ticks);
    Countdown(s.dodge_cooldown); Countdown(s.hurt_ticks);
    if(s.guardian_defeated) return;
    if(s.guard_mode==GuardMode::Windup) {
        if(--s.guard_ticks==0) {
            if(!evading && Distance(s.player,s.guard_target)<=1 &&
               Distance(s.player,s.guardian)<=3 && LineClear(s.guardian,s.player)) Damage();
            s.guard_mode=GuardMode::Recovery;
            s.guard_ticks=kGuardRecoveryTicks;
        }
        return;
    }
    if(s.guard_mode==GuardMode::Recovery) {
        if(--s.guard_ticks==0) s.guard_mode=GuardMode::Idle;
        return;
    }
    const bool engaged=s.bridge && Distance(s.player,Guardian())<=kGuardLeash+2 &&
        Distance(s.player,s.guardian)<=kAggroRange;
    if(engaged && Distance(s.player,s.guardian)<=2 && LineClear(s.guardian,s.player)) {
        s.guard_mode=GuardMode::Windup;
        s.guard_target=s.player;
        s.guard_ticks=kGuardWindupTicks;
        return;
    }
    const auto next=engaged?GuardMode::Chase:
        (Distance(s.guardian,Guardian())==0?GuardMode::Idle:GuardMode::Return);
    if(next!=s.guard_mode) { s.guard_mode=next; s.guard_ticks=0; }
    if(next==GuardMode::Idle) return;
    if(s.guard_ticks>0) { --s.guard_ticks; return; }
    StepGuardian(engaged?s.player:Guardian());
    s.guard_ticks=kStepTicks-1;
}

} // namespace hearthline
