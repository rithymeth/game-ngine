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
             <<"k attack, v dodge, p relay at 105, q relay at 107, e activate, x exit\n";
    char command=' ';
    while(game.State().phase==Phase::Playing && std::cin>>command) {
        const auto pos=game.State().player;
        switch(command) {
            case 'w': game.Move(0,-1); break; case 's': game.Move(0,1); break;
            case 'a': game.Move(-1,0); break; case 'd': game.Move(1,0); break;
            case 'g': game.Act(Action::Gather); break;
            case 'b': game.Act(Action::BuildBridge); break;
            case 'f': game.Act(Action::BuildCampfire); break;
            case 'r': game.Act(Action::Refuel); break;
            case 'h': game.Act(Action::Heal); break;
            case 'k': game.Act(Action::Attack); break;
            case 'v': game.Act(Action::Dodge); break;
            case 'p': game.Act(Action::PlaceRelay,{105,64}); break;
            case 'q': game.Act(Action::PlaceRelay,{107,64}); break;
            case 'e': game.Act(Action::Interact); break;
            case 'x': return 0;
        }
        const auto& s=game.State();
        std::cout<<"pos=("<<s.player.x<<","<<s.player.y<<") hp="<<s.health
                 <<" wood="<<s.inventory.wood<<" stone="<<s.inventory.stone
                 <<" energy="<<s.inventory.energy<<" food="<<s.inventory.food
                 <<" bridge="<<s.bridge<<" guardian="<<s.guardian_defeated
                 <<" beacon="<<s.beacon<<" night="<<s.night_turns
                 <<" fuel="<<s.fuel<<" phase="<<static_cast<int>(s.phase)<<"\n";
    }
    return game.Complete()?0:1;
}
