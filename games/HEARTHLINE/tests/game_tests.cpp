#include "game.h"
#include <cstdlib>
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
void Test(unsigned seed) {
    Game g(seed);
    CHECK(g.State().player.x==20);
    for(int i=0;i<6;i++) {
        To(g,Game::Resource(i).x-1,64);
        CHECK(g.Act(Action::Gather));
    }
    CHECK(g.State().inventory.wood==12);
    CHECK(g.State().inventory.energy==2);
    To(g,61,64);
    CHECK(!g.Move(1,0));
    CHECK(g.Act(Action::BuildBridge));
    CHECK(g.Move(1,0));
    CHECK(g.Move(1,0));
    To(g,87,64);
    for(int i=0;i<3;++i) {
        CHECK(g.Act(Action::Dodge));
        CHECK(g.Act(Action::Attack));
        if(i<2) CHECK(g.Act(Action::Dodge));
    }
    CHECK(g.State().guardian_defeated);
    To(g,104,64);
    CHECK(g.Act(Action::PlaceRelay,{105,64}));
    To(g,106,64);
    CHECK(g.Act(Action::PlaceRelay,{107,64}));
    To(g,108,64);
    CHECK(g.Act(Action::Interact));
    CHECK(g.State().beacon);
    CHECK(g.Act(Action::BuildCampfire));
    constexpr int kMaxNightActions=24;
    for(int n=0; n<kMaxNightActions && g.State().phase==Phase::Playing; ++n) {
        if(g.State().fuel<3 && g.State().inventory.wood>0)
            CHECK(g.Act(Action::Refuel));
        else CHECK(g.Act(Action::Dodge));
    }
    CHECK(g.Complete());
}
int main(){
    for(unsigned seed : {1u,7u,42u}) Test(seed);
    Game g(7);
    CHECK(g.Terrain(62,64)==Tile::Water);
    CHECK(!g.Walkable(-1,64));
    CHECK(!g.Act(Action::BuildBridge));
    CHECK(!g.Act(Action::PlaceRelay,{105,64}));
    std::cout<<"Hearthline three seeded expeditions passed\n";
}
