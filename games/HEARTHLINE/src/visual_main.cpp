#define SDL_MAIN_HANDLED
#include <SDL.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#include "game.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
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
void TileArt(SDL_Renderer* r,Tile t,int x,int y,int tx,int ty) {
    const auto c=TerrainColor(t);
    Rect(r,x,y,kTile,kTile,c[0],c[1],c[2]);
    const int variant=(tx*17+ty*11)%4;
    if(t==Tile::Tree) {
        Rect(r,x+11,y+17,6,9,104,78,48);
        Rect(r,x+4,y+8,20,12,22,54,44);
        Rect(r,x+7,y+3,14,17,34,90,62);
        Rect(r,x+10,y+1,8,8,69,127,80);
        Rect(r,x+5,y+13,7,5,47,110,67);
    } else if(t==Tile::Rock) {
        Rect(r,x+5,y+10,19,13,51,72,73);
        Rect(r,x+9,y+6,13,13,128,145,137);
        Rect(r,x+9,y+6,8,4,170,184,160);
    } else if(t==Tile::Water) {
        Rect(r,x+2+variant*3,y+8,11,2,69,136,164);
        Rect(r,x+11-variant,y+19,13,2,49,115,146);
    } else if(t==Tile::Ruin || t==Tile::Floor) {
        Rect(r,x,y+13,kTile,2,c[0]-22,c[1]-22,c[2]-20);
        Rect(r,x+(ty%2)*13,y,2,13,c[0]-22,c[1]-22,c[2]-20);
        Rect(r,x+14-(ty%2)*13,y+15,2,13,c[0]-22,c[1]-22,c[2]-20);
        Rect(r,x+3,y+3,5,2,c[0]+14,c[1]+14,c[2]+12);
    } else {
        Rect(r,x+4+variant*3,y+17,2,5,86,131,80);
        Rect(r,x+7+variant*3,y+15,2,4,71,121,73);
        if(variant==1) Rect(r,x+21,y+8,3,3,196,182,101);
    }
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
        if(!g.Discovered(x,y)) continue;
        TileArt(r,g.Terrain(x,y),sx(x),sy(y),x,y);
        if(g.ObjectAt(x,y)==Structure::StoneFloor) {
            Rect(r,sx(x)+1,sy(y)+1,26,26,111,117,120);
            Rect(r,sx(x)+2,sy(y)+2,11,11,150,155,150);
            Rect(r,sx(x)+15,sy(y)+15,11,11,140,145,143);
            Rect(r,sx(x)+2,sy(y)+15,11,11,126,132,132);
            Rect(r,sx(x)+15,sy(y)+2,11,11,135,143,145);
        }
        if(g.ObjectAt(x,y)==Structure::Wall) {
            Rect(r,sx(x)+2,sy(y)+5,24,20,91,62,42);
            Rect(r,sx(x)+2,sy(y)+5,24,4,194,157,97);
            Rect(r,sx(x)+12,sy(y)+9,3,16,63,42,32);
        }
    }
    if(s.bridge) for(int x=kRiverX;x<=kRiverX+1;++x) {
        if(!g.Discovered(x,kBridgeY)) continue;
        Rect(r,sx(x),sy(kBridgeY)+3,kTile,kTile-6,157,112,66);
        Rect(r,sx(x),sy(kBridgeY)+8,kTile,2,93,57,43);
    }
    for(int i=0;i<6;++i) {
        const Point a=Game::Resource(i);
        if(!g.ResourceAvailable(i) || !g.Visible(a.x,a.y)) continue;
        const int x=sx(a.x),y=sy(a.y);
        if(i<3) Rect(r,x+8,y+6,12,16,166,104,50);
        else if(i==3) Rect(r,x+6,y+6,16,15,185,187,172);
        else if(i==4) Rect(r,x+7,y+5,14,18,64,227,203);
        else Rect(r,x+7,y+7,14,14,211,139,102);
    }
    for(const auto& cache:Game::Caches()) {
        const auto p=cache.at;
        if(!g.Discovered(p.x,p.y)) continue;
        const bool opened=g.ObjectAt(p.x,p.y)==Structure::OpenedCache;
        const int x=sx(p.x),y=sy(p.y);
        Rect(r,x+4,y+10,20,14,71,50,39);
        Rect(r,x+5,y+7,18,11,opened?111:169,opened?91:119,opened?62:60);
        Rect(r,x+5,y+(opened?17:11),18,3,215,174,91);
        if(!opened) Rect(r,x+12,y+13,4,5,247,223,137);
    }
    for(int socket:{105,107}) if(g.Discovered(socket,64))
        Outline(r,sx(socket)+4,sy(64)+4,20,20,100,201,187);
    for(const auto& relay:g.Relays()) {
        if(!g.Discovered(relay.at.x,relay.at.y)) continue;
        const int x=sx(relay.at.x),y=sy(relay.at.y);
        Rect(r,x+4,y+4,kTile-8,kTile-8,84,73,124);
        Rect(r,x+7,y+7,14,14,168,133,201);
        const int dx[4]={1,0,-1,0},dy[4]={0,1,0,-1};
        const int ex=x+14+dx[relay.direction]*10,ey=y+14+dy[relay.direction]*10;
        Color(r,245,227,144); SDL_RenderDrawLine(r,x+14,y+14,ex,ey);
        Rect(r,ex-2,ey-2,5,5,255,224,137);
        if(s.beacon) Rect(r,x+24,y+13,12,3,110,243,208);
    }
    if(g.Discovered(Game::Source().x,Game::Source().y)) {
        const int x=sx(Game::Source().x),y=sy(Game::Source().y);
        Rect(r,x+7,y+7,14,14,74,228,208);
    }
    if(g.Discovered(Game::Beacon().x,Game::Beacon().y)) {
        const int x=sx(Game::Beacon().x),y=sy(Game::Beacon().y);
        Rect(r,x+3,y+3,22,22,s.beacon?255:142,s.beacon?211:130,s.beacon?95:148);
        Rect(r,x+9,y+9,10,10,s.beacon?255:39,s.beacon?251:46,s.beacon?182:57);
    }
    if(!s.guardian_defeated && s.guard_mode==GuardMode::Windup) {
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        constexpr Point danger[]={{0,0},{1,0},{-1,0},{0,1},{0,-1}};
        for(auto d:danger) {
            const Point q{s.guard_target.x+d.x,s.guard_target.y+d.y};
            if(!g.Visible(q.x,q.y)) continue;
            SDL_Rect area{sx(q.x)+1,sy(q.y)+1,kTile-2,kTile-2};
            Color(r,232,87,51,95+(kGuardWindupTicks-s.guard_ticks)*8);
            SDL_RenderFillRect(r,&area);
            Outline(r,area.x,area.y,area.w,area.h,244,154,75);
        }
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
    }
    if(!s.guardian_defeated && g.Visible(s.guardian.x,s.guardian.y)) {
        const int x=sx(s.guardian.x),y=sy(s.guardian.y);
        const int bob=static_cast<int>((s.combat_tick/4)%2);
        const bool charging=s.guard_mode==GuardMode::Windup;
        Rect(r,x+4,y+24,20,3,30,32,34);
        Rect(r,x+5,y+7-bob,18,19,96,55,75);
        Rect(r,x+3,y+4-bob,22,16,charging?238:177,charging?113:62,72);
        Rect(r,x+7,y+2-bob,5,6,220,169,116);
        Rect(r,x+17,y+2-bob,5,6,220,169,116);
        Rect(r,x+8,y+10-bob,4,4,245,226,181);
        Rect(r,x+16,y+10-bob,4,4,245,226,181);
        Rect(r,x+2,y-4,24,4,37,32,40);
        Rect(r,x+2,y-4,s.guard_health*8,4,224,80,88);
    }
    if(s.fire && g.Discovered(Game::FireSite().x,Game::FireSite().y)) {
        const int x=sx(Game::FireSite().x),y=sy(Game::FireSite().y);
        Rect(r,x+8,y+11,12,12,228,131,56);
        Rect(r,x+11,y+5,6,12,255,205,95);
    }
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    for(int y=startY;y<endY;++y) for(int x=startX;x<endX;++x) {
        if(g.Visible(x,y)) continue;
        SDL_Rect shadow{sx(x),sy(y),kTile,kTile};
        Color(r,7,14,22,g.Discovered(x,y)?175:255);
        SDL_RenderFillRect(r,&shadow);
    }
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
    const auto target=g.BuildTarget();
    if(g.Visible(target.x,target.y)) {
        Outline(r,sx(target.x)+1,sy(target.y)+1,kTile-2,kTile-2,224,208,148);
    }
    const int px=sx(p.x),py=sy(p.y)-static_cast<int>((s.combat_tick/5)%2);
    if(s.dodge_ticks>0) Outline(r,px,py,kTile,kTile,96,223,214);
    if(s.attack_ticks>=kAttackCooldownTicks-2) {
        const int dir=s.guardian.x<p.x?-1:1;
        const int strike=px+14+dir*18;
        Color(r,255,222,137);
        SDL_RenderDrawLine(r,px+14,py+2,strike,py+11);
        SDL_RenderDrawLine(r,strike,py+11,px+14,py+22);
        SDL_RenderDrawLine(r,px+14,py+3,strike-dir,py+11);
    }
    Rect(r,px+4,py+24,20,3,23,39,36);
    Rect(r,px+7,py+20,6,5,69,49,46);
    Rect(r,px+16,py+20,6,5,69,49,46);
    Rect(r,px+5,py+10,18,12,36,71,72);
    Rect(r,px+8,py+11,12,11,s.hurt_ticks%2?237:68,s.hurt_ticks%2?96:119,115);
    Rect(r,px+8,py+3,12,10,219,181,129);
    Rect(r,px+6,py+1,16,4,52,62,61);
    Rect(r,px+6,py+9,16,4,182,95,61);
    Rect(r,px+10,py+6,2,2,32,44,46);
    Rect(r,px+17,py+6,2,2,32,44,46);
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
void DrawMap(SDL_Renderer* r,const Game& g) {
    constexpr int tiles=kExpeditionSize, scale=4, span=tiles*scale;
    constexpr int left=(kWidth-span)/2, top=(kHeight-span)/2;
    const auto& s=g.State();
    const int x0=std::clamp(s.player.x-tiles/2,0,kMapSize-tiles);
    const int y0=std::clamp(s.player.y-tiles/2,0,kMapSize-tiles);
    const auto sx=[&](int x){return left+(x-x0)*scale;};
    const auto sy=[&](int y){return top+(y-y0)*scale;};
    const auto in=[&](Point p){return p.x>=x0 && p.x<x0+tiles && p.y>=y0 && p.y<y0+tiles;};
    Color(r,11,20,27); SDL_RenderClear(r);
    Rect(r,left-12,top-12,span+24,span+24,36,51,53);
    Rect(r,left,top,span,span,15,25,29);
    for(int y=y0;y<y0+tiles;++y) for(int x=x0;x<x0+tiles;++x) {
        if(!g.Discovered(x,y)) continue;
        auto c=TerrainColor(g.Terrain(x,y));
        switch(g.ObjectAt(x,y)) {
            case Structure::Bridge: c={190,145,80}; break;
            case Structure::Wall: c={144,107,69}; break;
            case Structure::StoneFloor: c={151,157,159}; break;
            case Structure::Campfire: c={247,148,65}; break;
            case Structure::Relay: c={101,202,199}; break;
            default: break;
        }
        Rect(r,sx(x),sy(y),scale,scale,c[0],c[1],c[2]);
    }
    const SDL_Rect clip{left,top,span,span};
    SDL_RenderSetClipRect(r,&clip);
    for(const auto& cache:Game::Caches()) {
        const auto p=cache.at;
        if(!in(p) || !g.Discovered(p.x,p.y)) continue;
        const bool opened=g.ObjectAt(p.x,p.y)==Structure::OpenedCache;
        Rect(r,sx(p.x)-2,sy(p.y)-2,8,8,opened?119:244,opened?124:192,opened?117:84);
    }
    const auto beacon=Game::Beacon();
    if(in(beacon) && g.Discovered(beacon.x,beacon.y))
        Rect(r,sx(beacon.x)-2,sy(beacon.y)-2,8,8,99,228,214);
    if(!s.guardian_defeated && in(s.guardian) && g.Visible(s.guardian.x,s.guardian.y))
        Rect(r,sx(s.guardian.x)-2,sy(s.guardian.y)-2,8,8,230,90,79);
    Rect(r,sx(s.player.x)-3,sy(s.player.y)-3,10,10,244,238,211);
    SDL_RenderSetClipRect(r,nullptr);
    Outline(r,left-1,top-1,span+2,span+2,111,140,121);
    SDL_RenderPresent(r);
}
const char* GuardName(GuardMode mode) {
    switch(mode) {
        case GuardMode::Idle: return "idle";
        case GuardMode::Chase: return "chase";
        case GuardMode::Windup: return "windup";
        case GuardMode::Recovery: return "recovery";
        case GuardMode::Return: return "return";
        case GuardMode::Defeated: return "defeated";
    }
    return "unknown";
}
std::string Title(const Game& g,bool paused,bool map_open) {
    const auto& s=g.State();
    std::string phase=s.phase==Phase::Won?"VICTORY - F5 to restart":s.phase==Phase::Lost?
      "DEFEAT - F5 to restart":map_open?"MAP - M or ESC to close; gold cache, grey looted, white player, cyan beacon":paused?"PAUSED - ESC to resume":
      !s.bridge?"Gather G; build B at river crossing":
      !s.guardian_defeated?"Find guardian; K attack, V evade marked strike":
      !s.beacon?"Place P/Q beside sockets; 2 rotate each east; E activate":
      !s.fire?"Build F campfire":"Stay near fire; R refuel, SPACE wait";
    if(!map_open && !paused && s.phase==Phase::Playing) {
        for(const auto& cache:Game::Caches()) {
            const auto p=cache.at;
            if(g.ObjectAt(p.x,p.y)==Structure::SupplyCache &&
               std::abs(p.x-s.player.x)+std::abs(p.y-s.player.y)<=1) {
                phase+=" | E open "+std::string(cache.name);
                break;
            }
        }
    }
    return "HEARTHLINE | "+phase+" | pos "+std::to_string(s.player.x)+","+
      std::to_string(s.player.y)+" | HP "+std::to_string(s.health)+
      " wood "+std::to_string(s.inventory.wood)+
      " stone "+std::to_string(s.inventory.stone)+
      " energy "+std::to_string(s.inventory.energy)+
      " food "+std::to_string(s.inventory.food)+
      " night "+std::to_string(s.night_turns)+"/24"+
      " fuel "+std::to_string(s.fuel)+" explored "+std::to_string(g.DiscoveredCount())+
      " build "+std::string(std::array<const char*,4>{"E","S","W","N"}[s.facing])+
      " caches "+std::to_string(g.OpenedCacheCount())+"/4 harvested "+std::to_string(g.HarvestedCount())+
      " | guard "+std::to_string(s.guardian.x)+","+std::to_string(s.guardian.y)+
      " guard HP "+std::to_string(s.guard_health)+" "+GuardName(s.guard_mode)+
      " attack cd "+std::to_string(s.attack_ticks)+" evade cd "+std::to_string(s.dodge_cooldown);
}
}


struct App {
    SDL_Window* window=nullptr;
    SDL_Renderer* renderer=nullptr;
    Game game{1};
    unsigned seed=1;
    bool running=true, paused=false, map_open=false, smoke=false, ready=false;
    std::string last_status, feedback;
    int frames=0;
    Uint32 last_frame_ms=0, combat_accumulator=0;

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
        if(SDL_RenderSetLogicalSize(renderer,kWidth,kHeight)!=0) {Close();return false;}
        last_frame_ms=SDL_GetTicks();
        return true;
    }
    void Close() {
        if(renderer){SDL_DestroyRenderer(renderer);renderer=nullptr;}
        if(window){SDL_DestroyWindow(window);window=nullptr;}
        SDL_Quit();
    }
    bool SaveProgress() {
#if defined(__EMSCRIPTEN__)
        const auto bytes=game.Save();
        return EM_ASM_INT({
            try {
                const data=HEAPU8.subarray($0,$0+$1);
                let binary="";
                for(let i=0;i<data.length;i+=8192)
                    binary+=String.fromCharCode.apply(null,data.subarray(i,i+8192));
                localStorage.setItem("hearthline.save.v1",btoa(binary));
                return 1;
            } catch(error) { return 0; }
        },bytes.data(),bytes.size())==1;
#else
        return game.SaveFile("hearthline.sav");
#endif
    }
    bool LoadProgress() {
#if defined(__EMSCRIPTEN__)
        std::vector<std::uint8_t> bytes(kMaxSaveBytes);
        const int n=EM_ASM_INT({
            try {
                const encoded=localStorage.getItem("hearthline.save.v1");
                if(!encoded || encoded.length>4*Math.ceil($1/3)) return 0;
                const binary=atob(encoded);
                if(binary.length>$1) return 0;
                for(let i=0;i<binary.length;++i) HEAPU8[$0+i]=binary.charCodeAt(i);
                return binary.length;
            } catch(error) { return 0; }
        },bytes.data(),bytes.size());
        return n>0 && game.Load(std::span<const std::uint8_t>(bytes.data(),static_cast<std::size_t>(n)));
#else
        return game.LoadFile("hearthline.sav");
#endif
    }
    void ResetCombatClock() {
        combat_accumulator=0;
        last_frame_ms=SDL_GetTicks();
    }
    void PauseForFocusLoss() {
        if(game.State().phase==Phase::Playing) {
            paused=true;
            ResetCombatClock();
        }
    }
    void Key(SDL_Keycode k, SDL_Keymod mods) {
        if(k==SDLK_j) {
            feedback=SaveProgress()?"Saved expedition":"Save failed";
            return;
        }
        if(k==SDLK_l) {
            if(LoadProgress()) {
                seed=game.Seed();
                paused=true; map_open=false;
                ResetCombatClock();
                feedback="Loaded expedition - ESC to resume";
            } else feedback="Load failed - current game unchanged";
            return;
        }
        feedback.clear();
        if(k==SDLK_ESCAPE){
            if(map_open) map_open=false;
            else paused=!paused;
            ResetCombatClock(); return;
        }
        if(k==SDLK_m && game.State().phase==Phase::Playing) {
            map_open=!map_open; ResetCombatClock(); return;
        }
        if((k==SDLK_n && (mods&KMOD_CTRL)) || k==SDLK_F5){
            game=Game(seed);paused=false;map_open=false;ResetCombatClock();return;
        }
        if(paused || map_open || game.State().phase!=Phase::Playing) return;
        if(k==SDLK_UP || k==SDLK_w) (void)game.Move(0,-1);
        else if(k==SDLK_DOWN || k==SDLK_s) (void)game.Move(0,1);
        else if(k==SDLK_LEFT || k==SDLK_a) (void)game.Move(-1,0);
        else if(k==SDLK_RIGHT || k==SDLK_d) (void)game.Move(1,0);
        else if(k==SDLK_g) (void)game.Act(Action::Gather);
        else if(k==SDLK_b) (void)game.Act(Action::BuildBridge);
        else if(k==SDLK_k) (void)game.Act(Action::Attack);
        else if(k==SDLK_v) (void)game.Act(Action::Dodge);
        else if(k==SDLK_SPACE) (void)game.Act(Action::Wait);
        else if(k==SDLK_p) (void)game.Act(Action::PlaceRelay,{105,64});
        else if(k==SDLK_q) (void)game.Act(Action::PlaceRelay,{107,64});
        else if(k==SDLK_e) (void)game.Act(Action::Interact);
        else if(k==SDLK_f) (void)game.Act(Action::BuildCampfire);
        else if(k==SDLK_r) (void)game.Act(Action::Refuel);
        else if(k==SDLK_h) (void)game.Act(Action::Heal);
        else if(k==SDLK_t) game.RotateBuildFacing();
        else if(k==SDLK_1) {
            if(!game.Act(Action::BuildWall,game.BuildTarget()))
                feedback="Wall unavailable (2 wood; free unprotected tile)";
        } else if(k==SDLK_3) {
            if(!game.Act(Action::BuildFloor,game.BuildTarget()))
                feedback="Floor unavailable (1 stone; free unprotected tile)";
        } else if(k==SDLK_2) {
            (void)game.RotateNearbyRelay();
        }
    }
    void Tick() {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) running=false;
#if !defined(__EMSCRIPTEN__)
            // Moving focus to the browser's HTML controls is not leaving the game.
            // The shell handles top-level window and tab focus in a web build.
            if(e.type==SDL_WINDOWEVENT &&
                (e.window.event==SDL_WINDOWEVENT_FOCUS_LOST || e.window.event==SDL_WINDOWEVENT_MINIMIZED))
                PauseForFocusLoss();
#endif
            if(e.type==SDL_KEYDOWN) {
                const auto k=e.key.keysym.sym;
                const bool movement=k==SDLK_UP || k==SDLK_DOWN || k==SDLK_LEFT || k==SDLK_RIGHT ||
                    k==SDLK_w || k==SDLK_a || k==SDLK_s || k==SDLK_d || k==SDLK_SPACE;
                if(!e.key.repeat || movement) Key(k,static_cast<SDL_Keymod>(e.key.keysym.mod));
            }
        }
        if(!running) return;
        const Uint32 now=SDL_GetTicks();
        const Uint32 elapsed=std::min<Uint32>(now-last_frame_ms,250);
        last_frame_ms=now;
        if(!paused && !map_open && game.State().phase==Phase::Playing) {
            combat_accumulator+=elapsed;
            while(combat_accumulator>=kCombatTickMs) {
                game.TickCombat();
                combat_accumulator-=kCombatTickMs;
            }
        } else combat_accumulator=0;
        const std::string status=Title(game,paused,map_open)+(feedback.empty()?"":" | "+feedback);
        if(status!=last_status) {
            last_status=status;
            SDL_SetWindowTitle(window,status.c_str());
#if defined(__EMSCRIPTEN__)
            EM_ASM({ if(Module.onGameStatus) Module.onGameStatus(UTF8ToString($0)); },
                   status.c_str());
#endif
        }
        if(map_open) DrawMap(renderer,game);
        else Draw(renderer,game,kWidth,kHeight,paused);
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
extern "C" EMSCRIPTEN_KEEPALIVE void hearthline_pause_for_focus_loss() {
    if(g_browser_app) g_browser_app->PauseForFocusLoss();
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
                <<"P/Q place relay | E interact | F campfire | R refuel | H heal\n"
                <<"T face | 1 wall | 3 floor | 2 rotate relay | J save | L load | SPACE wait | M map | ESC pause | F5 restart | --smoke\n";
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
