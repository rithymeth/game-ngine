#include "game.h"
#include <cassert>
#include <iostream>
using namespace hearthline;
void To(Game& g,int x,int y) {
    int guard=10000;
    while(g.State().player.x!=x && --guard>0) {
        const int dx=g.State().player.x<x?1:-1;
        assert(g.Move(dx,0));
    }
    while(g.State().player.y!=y && --guard>0) {
        const int dy=g.State().player.y<y?1:-1;
        assert(g.Move(0,dy));
    }
    assert(guard>0);
}
void Test(unsigned seed) {
    Game g(seed);
    assert(g.State().player.x==20);
    for(int i=0;i<6;i++) {
        To(g,Resource(i).x-1,64);
        assert(g.Act(Action::Gather));
    }
    assert(g.State().inventory.wood==12);
    assert(g.State().inventory.energy==2);
    To(g,61,64);
    assert(!g.Move(1,0));
    assert(g.Act(Action::BuildBridge));
    assert(g.Move(1,0));
    assert(g.Move(1,0));
    To(g,87,64);
    for(int i=0;i<3;++i) {
        assert(g.Act(Action::Dodge));
        assert(g.Act(Action::Attack));
        if(i<2) assert(g.Act(Action::Dodge));
    }
    assert(g.State().guardian_defeated);
    To(g,104,64);
    assert(g.Act(Action::PlaceRelay,{105,64}));
    To(g,106,64);
    assert(g.Act(Action::PlaceRelay,{107,64}));
    To(g,108,64);
    assert(g.Act(Action::Interact));
    assert(g.State().beacon);
    assert(g.Act(Action::BuildCampfire));
    while(g.State().phase==Phase::Playing) {
        if(g.State().fuel<3 && g.State().inventory.wood>0)
            assert(g.Act(Action::Refuel));
        else assert(g.Act(Action::Dodge));
    }
    assert(g.Complete());
}
int main(){
    for(unsigned seed : {1u,7u,42u}) Test(seed);
    Game g(7);
    assert(g.Terrain(62,64)==Tile::Water);
    assert(!g.Walkable(-1,64));
    assert(!g.Act(Action::BuildBridge));
    assert(!g.Act(Action::PlaceRelay,{105,64}));
    std::cout<<"Hearthline three seeded expeditions passed\n";
}
