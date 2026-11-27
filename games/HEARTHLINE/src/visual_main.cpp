#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#include "game.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
using namespace hearthline;
constexpr int kWidth=960;
constexpr int kHeight=640;
constexpr int kTile=28;
constexpr int kHud=72;

void Color(SDL_Renderer* r, int red,int green,int blue,int alpha=255) {
    SDL_SetRenderDrawColor(r,static_cast<Uint8>(red),static_cast<Uint8>(green),
                           static_cast<Uint8>(blue),static_cast<Uint8>(alpha));
}
void Rect(SDL_Renderer* r, int x,int y,int w,int h,int red,int green,int blue) {
    SDL_Rect b{x,y,w,h}; Color(r,red,green,blue); SDL_RenderFillRect(r,&b);
}
void Outline(SDL_Renderer* r,int x,int y,int w,int h,int red,int green,int blue) {
    SDL_Rect b{x,y,w,h}; Color(r,red,green,blue); SDL_RenderDrawRect(r,&b);
}
std::array<int,3> TerrainColor(Tile t) {
    switch(t) {
        case Tile::Grass: return {49,102,73};
        case Tile::Tree: return {26,70,47};
        case Tile::Rock: return {93,104,105};
        case Tile::Water: return {35,86,154};
        case Tile::Ruin: return {92,81,92};
        case Tile::Floor: return {107,115,91};
    }
    return {0,0,0};
}
void Draw(SDL_Renderer* r,const Game& g,int w,int h, bool paused) {
    const auto& s=g.State();
    const auto p=s.player;
    Color(r,13,22,30); SDL_RenderClear(r);
    const int viewH=h-kHud;
    const int worldX=p.x*kTile+kTile/2-w/2;
    const int worldY=p.y*kTile+kTile/2-viewH/2;
    const int startX=std::max(0,worldX/kTile-1);
    const int startY=std::max(0,worldY/kTile-1);
    const int endX=std::min(kMapSize,startX+w/kTile+4);
    const int endY=std::min(kMapSize,startY+viewH/kTile+4);
    auto sx=[&](int x){return x*kTile-worldX;};
    auto sy=[&](int y){return y*kTile-worldY;};
    for(int y=startY;y<endY;++y) for(int x=startX;x<endX;++x) {
        const auto c=TerrainColor(g.Terrain(x,y));
        Rect(r,sx(x),sy(y),kTile,kTile,c[0],c[1],c[2]);
        if((x+y)%5==0) Rect(r,sx(x)+4,sy(y)+18,4,3,c[0]+10,c[1]+13,c[2]+5);
    }
    if(s.bridge) for(int x=kRiverX;x<=kRiverX+1;++x) {
        Rect(r,sx(x),sy(kBridgeY)+3,kTile,kTile-6,157,112,66);
        Rect(r,sx(x),sy(kBridgeY)+8,kTile,2,93,57,43);
    }
    for(int i=0;i<6;++i) {
        const Point a=Game::Resource(i);
        const int x=sx(a.x),y=sy(a.y);
        if(i<3) Rect(r,x+8,y+6,12,16,166,104,50);
        else if(i==3) Rect(r,x+6,y+6,16,15,185,187,172);
        else if(i==4) Rect(r,x+7,y+5,14,18,64,227,203);
        else Rect(r,x+7,y+7,14,14,211,139,102);
    }
    for(const auto& relay:g.Relays()) {
        const int x=sx(relay.at.x),y=sy(relay.at.y);
        Rect(r,x+4,y+4,kTile-8,kTile-8,179,143,220);
        if(relay.direction%2==0) Rect(r,x+8,y+12,12,4,240,224,143);
        else Rect(r,x+12,y+8,4,12,240,224,143);
    }
    {
        const int x=sx(Game::Source().x),y=sy(Game::Source().y);
        Rect(r,x+7,y+7,14,14,74,228,208);
    }
    {
        const int x=sx(Game::Beacon().x),y=sy(Game::Beacon().y);
        Rect(r,x+3,y+3,22,22,s.beacon?255:142,s.beacon?211:130,s.beacon?95:148);
        Rect(r,x+9,y+9,10,10,s.beacon?255:39,s.beacon?251:46,s.beacon?182:57);
    }
    if(!s.guardian_defeated) {
        const int x=sx(Game::Guardian().x),y=sy(Game::Guardian().y);
        Rect(r,x+3,y+3,22,22,177,62,72);
        Rect(r,x+8,y+9,4,4,245,226,181);
        Rect(r,x+16,y+9,4,4,245,226,181);
    }
    if(s.fire) {
        const int x=sx(Game::Beacon().x-2),y=sy(Game::Beacon().y+1);
        Rect(r,x+8,y+11,12,12,228,131,56);
        Rect(r,x+11,y+5,6,12,255,205,95);
    }
    Rect(r,sx(p.x)+4,sy(p.y)+3,20,23,244,215,156);
    Rect(r,sx(p.x)+8,sy(p.y)+7,4,4,30,50,49);
    Rect(r,sx(p.x)+17,sy(p.y)+7,4,4,30,50,49);
    // HUD uses graphics-only indicators; detailed quantities and prompts are in the window title.
    Rect(r,0,h-kHud,w,kHud,17,27,40);
    for(int i=0;i<5;++i) {
        Rect(r,22+i*26,h-48,21,23,i<s.health?201:67,i<s.health?78:69,i<s.health?82:80);
    }
    const int resources[4]={s.inventory.wood,s.inventory.stone,s.inventory.energy,s.inventory.food};
    const int palette[4][3]={{178,112,58},{162,168,160},{60,219,199},{232,164,98}};
    for(int i=0;i<4;++i) {
        const int x=196+i*92;
        Rect(r,x,h-48,15,23,palette[i][0],palette[i][1],palette[i][2]);
        const int bar=std::min(resources[i],15)*4;
        Rect(r,x+21,h-43,bar,13,palette[i][0],palette[i][1],palette[i][2]);
    }
    Rect(r,590,h-48,142,16,49,64,78);
    Rect(r,590,h-48,static_cast<int>(142.0f*s.night_turns/24.0f),16,239,195,104);
    if(s.fire) {
        Rect(r,750,h-48,150,16,49,64,78);
        Rect(r,750,h-48,std::min(s.fuel,24)*6,16,226,139,70);
    }
    if(s.phase!=Phase::Playing || paused) {
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        Color(r,7,10,17,170);
        SDL_Rect panel{w/4,h/3,w/2,h/4};
        SDL_RenderFillRect(r,&panel);
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
        Outline(r,panel.x,panel.y,panel.w,panel.h,
                s.phase==Phase::Won?88:222,s.phase==Phase::Lost?70:193,130);
    }
    SDL_RenderPresent(r);
}
std::string Title(const Game& g,bool paused) {
    const auto& s=g.State();
    std::string phase=s.phase==Phase::Won?"VICTORY - R to restart":s.phase==Phase::Lost?
      "DEFEAT - R to restart":paused?"PAUSED - ESC to resume":
      !s.bridge?"Gather G, move to river, build B":
      !s.guardian_defeated?"Find guardian, V dodge, K attack":
      !s.beacon?"Place P/Q relays, E activate":
      !s.fire?"Build F campfire":"Survive night: R refuel, V wait";
    return "HEARTHLINE | "+phase+" | pos "+std::to_string(s.player.x)+","+
      std::to_string(s.player.y)+" | HP "+std::to_string(s.health)+
      " wood "+std::to_string(s.inventory.wood)+
      " stone "+std::to_string(s.inventory.stone)+
      " energy "+std::to_string(s.inventory.energy)+
      " food "+std::to_string(s.inventory.food)+
      " night "+std::to_string(s.night_turns)+"/24";
}
}


struct App {
    SDL_Window* window=nullptr;
    SDL_Renderer* renderer=nullptr;
    Game game{1};
    unsigned seed=1;
    bool running=true, paused=false, smoke=false, ready=false;
    std::string last_status;
    int frames=0;

    explicit App(unsigned world_seed,bool do_smoke):game(world_seed),seed(world_seed),smoke(do_smoke){}
    bool Init() {
        SDL_SetMainReady();
        if(SDL_Init(SDL_INIT_VIDEO)!=0) {
            std::cerr<<"SDL init failed: "<<SDL_GetError()<<"\n"; return false;
        }
        window=SDL_CreateWindow("HEARTHLINE",SDL_WINDOWPOS_CENTERED,
             SDL_WINDOWPOS_CENTERED,kWidth,kHeight,SDL_WINDOW_RESIZABLE|SDL_WINDOW_SHOWN);
        if(!window){std::cerr<<SDL_GetError()<<"\n";SDL_Quit();return false;}
        renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if(!renderer) renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
        if(!renderer) {std::cerr<<SDL_GetError()<<"\n";Close();return false;}
        return true;
    }
    void Close() {
        if(renderer){SDL_DestroyRenderer(renderer);renderer=nullptr;}
        if(window){SDL_DestroyWindow(window);window=nullptr;}
        SDL_Quit();
    }
    void Key(SDL_Keycode k, SDL_Keymod mods) {
        if(k==SDLK_ESCAPE){paused=!paused;return;}
        if((k==SDLK_n && (mods&KMOD_CTRL)) || k==SDLK_F5){
            game=Game(seed);paused=false;return;
        }
        if(paused || game.State().phase!=Phase::Playing) return;
        if(k==SDLK_UP || k==SDLK_w) (void)game.Move(0,-1);
        else if(k==SDLK_DOWN || k==SDLK_s) (void)game.Move(0,1);
        else if(k==SDLK_LEFT || k==SDLK_a) (void)game.Move(-1,0);
        else if(k==SDLK_RIGHT || k==SDLK_d) (void)game.Move(1,0);
        else if(k==SDLK_g) (void)game.Act(Action::Gather);
        else if(k==SDLK_b) (void)game.Act(Action::BuildBridge);
        else if(k==SDLK_k) (void)game.Act(Action::Attack);
        else if(k==SDLK_v) (void)game.Act(Action::Dodge);
        else if(k==SDLK_p) (void)game.Act(Action::PlaceRelay,{105,64});
        else if(k==SDLK_q) (void)game.Act(Action::PlaceRelay,{107,64});
        else if(k==SDLK_e) (void)game.Act(Action::Interact);
        else if(k==SDLK_f) (void)game.Act(Action::BuildCampfire);
        else if(k==SDLK_r) (void)game.Act(Action::Refuel);
        else if(k==SDLK_h) (void)game.Act(Action::Heal);
        else if(k==SDLK_1) {
            const auto p=game.State().player;
            (void)game.Act(Action::BuildWall,{p.x+1,p.y});
        } else if(k==SDLK_2) {
            const auto p=game.State().player;
            (void)game.Act(Action::RotateRelay,{p.x+1,p.y});
        }
    }
    void Tick() {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) running=false;
            if(e.type==SDL_KEYDOWN) Key(e.key.keysym.sym,static_cast<SDL_Keymod>(e.key.keysym.mod));
        }
        if(!running) return;
        int w=0,h=0;
        SDL_GetRendererOutputSize(renderer,&w,&h);
        const std::string status=Title(game,paused);
        SDL_SetWindowTitle(window,status.c_str());
#if defined(__EMSCRIPTEN__)
        if(status!=last_status) {
            last_status=status;
            EM_ASM({ if(Module.onGameStatus) Module.onGameStatus(UTF8ToString($0)); },
                   status.c_str());
        }
#endif
        Draw(renderer,game,w,h,paused);
#if defined(__EMSCRIPTEN__)
        if(!ready) {
            ready=true;
            EM_ASM({
                if(Module.onGameReady) Module.onGameReady();
            });
        }
#endif
        if(smoke && ++frames>=3) running=false;
    }
};
#if defined(__EMSCRIPTEN__)
App* g_browser_app=nullptr;
extern "C" EMSCRIPTEN_KEEPALIVE void hearthline_command(int key) {
    if(g_browser_app) g_browser_app->Key(static_cast<SDL_Keycode>(key),KMOD_NONE);
}
void BrowserFrame(void* data) {
    auto* app=static_cast<App*>(data);
    app->Tick();
    if(!app->running) {
        emscripten_cancel_main_loop();
        app->Close();
    }
}
#endif

int main(int argc,char** argv) {
    bool smoke=false;
    unsigned seed=1;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--smoke") smoke=true;
        else if(arg=="--seed" && i+1<argc)
            seed=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else if(arg=="--help") {
            std::cout<<"HEARTHLINE graphical prototype\n"
                <<"Arrows/WASD move | G gather | B bridge | K attack | V dodge\n"
                <<"P/Q place relay | E activate | F campfire | R refuel | H heal\n"
                <<"1 wall east | 2 rotate relay east | ESC pause | F5 restart | --smoke\n";
            return 0;
        }
    }
    static App app(seed,smoke);
    if(!app.Init()) return 1;
#if defined(__EMSCRIPTEN__)
    g_browser_app=&app;
    emscripten_set_main_loop_arg(BrowserFrame,&app,0,1);
    // Emscripten owns the main loop until the page is closed.
#else
    while(app.running) {
        app.Tick();
        SDL_Delay(16);
    }
    app.Close();
#endif
    return 0;
}
