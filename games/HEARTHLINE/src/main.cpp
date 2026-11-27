#include "game.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace hearthline;
int main(int argc,char** argv) {
    unsigned seed=1;
    if(argc>1) seed=static_cast<unsigned>(std::strtoul(argv[1],nullptr,10));
    Game game(seed);
    std::cout<<"HEARTHLINE simulation | seed "<<seed<<" | 512x512 / 3 layers\n"
             <<"Commands: w a s d, g gather, b bridge, f fire, r refuel, h heal, "
             <<"k attack, v dodge, . wait, p/q place relay, 2 rotate nearby relay, e activate, x exit\n";
    char command=' ';
    while(game.State().phase==Phase::Playing && std::cin>>command) {
        bool accepted=false;
        switch(command) {
            case 'w': accepted=game.Move(0,-1); break; case 's': accepted=game.Move(0,1); break;
            case 'a': accepted=game.Move(-1,0); break; case 'd': accepted=game.Move(1,0); break;
            case 'g': accepted=game.Act(Action::Gather); break;
            case 'b': accepted=game.Act(Action::BuildBridge); break;
            case 'f': accepted=game.Act(Action::BuildCampfire); break;
            case 'r': accepted=game.Act(Action::Refuel); break;
            case 'h': accepted=game.Act(Action::Heal); break;
            case 'k': accepted=game.Act(Action::Attack); break;
            case 'v': accepted=game.Act(Action::Dodge); break;
            case '.': accepted=game.Act(Action::Wait); break;
            case 'p': accepted=game.Act(Action::PlaceRelay,{105,64}); break;
            case 'q': accepted=game.Act(Action::PlaceRelay,{107,64}); break;
            case '2': accepted=game.RotateNearbyRelay(); break;
            case 'e': accepted=game.Act(Action::Interact); break;
            case 'x': return 0;
        }
        if(!accepted) std::cout<<"Action unavailable.\n";
        for(int tick=0;tick<5;++tick) game.TickCombat();
        const auto& s=game.State();
        std::cout<<"pos=("<<s.player.x<<","<<s.player.y<<") hp="<<s.health
                 <<" wood="<<s.inventory.wood<<" stone="<<s.inventory.stone
                 <<" energy="<<s.inventory.energy<<" food="<<s.inventory.food
                 <<" bridge="<<s.bridge<<" guardian="<<s.guardian_defeated
                 <<" guard_hp="<<s.guard_health<<" guard_pos="<<s.guardian.x<<","<<s.guardian.y
                 <<" attack_cd="<<s.attack_ticks<<" guard_mode="<<static_cast<int>(s.guard_mode)
                 <<" caches="<<game.OpenedCacheCount()<<" harvested="<<game.HarvestedCount()
                 <<" beacon="<<s.beacon<<" night="<<s.night_turns
                 <<" fuel="<<s.fuel<<" phase="<<static_cast<int>(s.phase)<<"\n";
    }
    return game.Complete()?0:1;
}
