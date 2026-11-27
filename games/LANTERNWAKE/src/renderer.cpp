#include "renderer.h"

#include "aether/assets/png_writer.h"
#include "aether/ui/draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>

namespace lanternwake {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr std::uint32_t Rgb(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r | (g << 8) | (b << 16)) | (static_cast<std::uint32_t>(a) << 24);
}
constexpr auto ink = Rgb(11, 24, 30);
constexpr auto cream = Rgb(245, 235, 206);
constexpr auto gold = Rgb(230, 188, 112);
constexpr auto quiet = Rgb(151, 171, 164);
constexpr auto teal = Rgb(100, 211, 191);
constexpr auto red = Rgb(238, 126, 116);
bool Inside(float px, float py, float x, float y, float w, float h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}
std::uint32_t Sample(const aether::assets::ImageData& image, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, static_cast<int>(image.width) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(image.height) - 1);
    const float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy, int c) { return image.pixels[(static_cast<std::size_t>(yy) * image.width + xx) * 4 + c]; };
    int channels[4];
    // Interpolate premultiplied color when importing a transparent sprite,
    // then return straight alpha for the compositor. This avoids dark halos.
    const float weights[] = {(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    const int xs[] = {x0, x1, x0, x1}, ys[] = {y0, y0, y1, y1};
    float alpha = 0;
    for (int j = 0; j < 4; ++j) alpha += at(xs[j], ys[j], 3) * weights[j];
    for (int c = 0; c < 3; ++c) {
        float value = 0;
        for (int j = 0; j < 4; ++j) value += at(xs[j], ys[j], c) * at(xs[j], ys[j], 3) * weights[j];
        channels[c] = alpha > 0 ? std::clamp(static_cast<int>(value / alpha + .5f), 0, 255) : 0;
    }
    channels[3] = std::clamp(static_cast<int>(alpha + .5f), 0, 255);
    return Rgb(channels[0], channels[1], channels[2], channels[3]);
}
} // namespace

Renderer::Renderer() : pixels_(kWidth * kHeight), background_(kWidth * kHeight) {}

bool Renderer::LoadAssets(const std::string& directory, std::string& error) {
    const auto path = [&](const char* filename) { return (std::filesystem::path(directory) / filename).string(); };
    aether::assets::ImageData forest;
    if (!aether::assets::DecodeImageFile(path("forest.png"), forest)) {
        error = "Cannot load forest.png from " + directory + ". Keep the assets folder beside Lanternwake.";
        return false;
    }
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto pixel = Sample(forest, (x + .5f) * forest.width / kWidth - .5f,
                                      (y + .5f) * forest.height / kHeight - .5f);
            const float nx = (x - kWidth * .5f) / (kWidth * .6f), ny = (y - kHeight * .5f) / (kHeight * .65f);
            const float vignette = std::clamp(1.05f - (nx * nx + ny * ny) * .22f, .50f, 1.0f);
            background_[static_cast<std::size_t>(y) * kWidth + x] =
                Rgb(static_cast<int>((pixel & 255) * vignette),
                    static_cast<int>(((pixel >> 8) & 255) * vignette),
                    static_cast<int>(((pixel >> 16) & 255) * vignette));
        }
    }
    auto sprite = [&](const char* filename, int height, Sprite& out) {
        aether::assets::ImageData image;
        if (!aether::assets::DecodeImageFile(path(filename), image)) {
            error = std::string("Cannot load required character art: ") + filename;
            return false;
        }
        int left = static_cast<int>(image.width), top = static_cast<int>(image.height), right = -1, bottom = -1;
        for (int y = 0; y < static_cast<int>(image.height); ++y) for (int x = 0; x < static_cast<int>(image.width); ++x) {
            if (image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + 3] > 8) {
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
        }
        if (right < left || bottom < top) { error = std::string("Empty sprite: ") + filename; return false; }
        out.height = height;
        out.width = std::max(1, static_cast<int>(height * static_cast<float>(right-left+1) / (bottom-top+1)));
        out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
        for (int y = 0; y < out.height; ++y) for (int x = 0; x < out.width; ++x)
            out.pixels[static_cast<std::size_t>(y) * out.width + x] = Sample(image,
                left + (x + .5f) * (right-left+1) / out.width - .5f,
                top + (y + .5f) * (bottom-top+1) / out.height - .5f);
        return true;
    };
    if (!sprite("keeper.png", 87, keeper_) || !sprite("wraith.png", 74, wraith_)) return false;
    if (!sprite("shrine.png", 107, shrine_)) return false;
    aether::ui::SdfFontSettings settings;
    settings.atlas_width = 1024;
    settings.atlas_height = 512;
    settings.base_size = 54;
    body_ = aether::ui::SdfFont::FromFile(path("body.ttf"), settings, &error);
    if (!body_) return false;
    title_ = aether::ui::SdfFont::FromFile(path("title.ttf"), settings, &error);
    if (!title_) return false;
    const std::string glyphs = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !?.,:/+-<>[]'";
    body_->Prewarm(glyphs);
    title_->Prewarm(glyphs);
    return true;
}

float Renderer::Random() {
    random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
    return static_cast<float>(random_ & 0xffff) / 65535.0f;
}

void Renderer::Blend(int x, int y, std::uint32_t color, float alpha) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    const int a = std::clamp(static_cast<int>((color >> 24) * alpha), 0, 255);
    if (a == 0) return;
    auto& pixel = pixels_[static_cast<std::size_t>(y) * kWidth + x];
    if (a == 255) { pixel = color | 0xff000000u; return; }
    const int inv = 255 - a;
    pixel = Rgb(((pixel & 255) * inv + (color & 255) * a) / 255,
                (((pixel >> 8) & 255) * inv + ((color >> 8) & 255) * a) / 255,
                (((pixel >> 16) & 255) * inv + ((color >> 16) & 255) * a) / 255);
}

void Renderer::Rect(float x, float y, float w, float h, std::uint32_t color, float alpha) {
    for (int yy = std::max(0, static_cast<int>(y)); yy < std::min(kHeight, static_cast<int>(std::ceil(y+h))); ++yy)
        for (int xx = std::max(0, static_cast<int>(x)); xx < std::min(kWidth, static_cast<int>(std::ceil(x+w))); ++xx)
            Blend(xx, yy, color, alpha);
}

void Renderer::Rounded(float x, float y, float w, float h, float radius, std::uint32_t color, float alpha) {
    for (int yy = std::max(0, static_cast<int>(y)); yy < std::min(kHeight, static_cast<int>(std::ceil(y+h))); ++yy) {
        for (int xx = std::max(0, static_cast<int>(x)); xx < std::min(kWidth, static_cast<int>(std::ceil(x+w))); ++xx) {
            const float dx = std::max(std::max(x+radius-xx, xx-(x+w-radius)), 0.0f);
            const float dy = std::max(std::max(y+radius-yy, yy-(y+h-radius)), 0.0f);
            const float edge = radius - std::sqrt(dx*dx+dy*dy);
            if (edge >= 0) Blend(xx, yy, color, alpha * std::min(1.0f, edge+.5f));
        }
    }
}

void Renderer::Disc(float x, float y, float radius, std::uint32_t color, float alpha) {
    if (radius <= 0) return;
    for (int yy = std::max(0, static_cast<int>(y-radius-1)); yy <= std::min(kHeight-1, static_cast<int>(y+radius+1)); ++yy)
        for (int xx = std::max(0, static_cast<int>(x-radius-1)); xx <= std::min(kWidth-1, static_cast<int>(x+radius+1)); ++xx) {
            const float d = radius - std::sqrt((xx-x)*(xx-x)+(yy-y)*(yy-y));
            if (d > -.5f) Blend(xx, yy, color, alpha*std::clamp(d+.5f,0.0f,1.0f));
        }
}

void Renderer::Glow(float x, float y, float radius, std::uint32_t color, float strength) {
    if (radius <= 0) return;
    const float inv = 1.0f / (radius*radius);
    for (int yy = std::max(0, static_cast<int>(y-radius)); yy < std::min(kHeight, static_cast<int>(y+radius)); ++yy)
        for (int xx = std::max(0, static_cast<int>(x-radius)); xx < std::min(kWidth, static_cast<int>(x+radius)); ++xx) {
            const float d = ((xx-x)*(xx-x)+(yy-y)*(yy-y))*inv;
            if (d < 1) {
                const float f = 1-d;
                Blend(xx, yy, color, strength*f*f*f);
            }
        }
}

void Renderer::Line(float x1,float y1,float x2,float y2,float thickness,std::uint32_t color,float alpha) {
    const float dx=x2-x1,dy=y2-y1;
    const int steps=std::max(1,static_cast<int>(std::sqrt(dx*dx+dy*dy)));
    for(int i=0;i<=steps;++i) Disc(x1+dx*i/steps,y1+dy*i/steps,thickness*.5f,color,alpha);
}

void Renderer::Ring(float x,float y,float radius,float thickness,std::uint32_t color,float alpha,float fraction) {
    if(fraction<=0) return;
    const int count=std::max(24,static_cast<int>(radius*5));
    for(int i=0;i<static_cast<int>(count*std::min(1.0f,fraction));++i) {
        const float a=i*2*pi/count-pi*.5f;
        Disc(x+std::cos(a)*radius,y+std::sin(a)*radius,thickness*.5f,color,alpha);
    }
}

void Renderer::Diamond(float x,float y,float radius,std::uint32_t color,float alpha) {
    for(int yy=static_cast<int>(y-radius); yy<=static_cast<int>(y+radius);++yy) {
        const float span=radius-std::fabs(yy-y);
        for(int xx=static_cast<int>(x-span);xx<=static_cast<int>(x+span);++xx) Blend(xx,yy,color,alpha);
    }
}

void Renderer::DrawSprite(const Sprite& sprite,float x,float y,float alpha,bool flip,float sx,float sy) {
    const int w=static_cast<int>(sprite.width*sx),h=static_cast<int>(sprite.height*sy);
    if(w<=0||h<=0) return;
    const int ox=static_cast<int>(x-w*.5f),oy=static_cast<int>(y-h);
    for(int yy=0;yy<h;++yy) for(int xx=0;xx<w;++xx) {
        int tx=std::min(sprite.width-1,static_cast<int>(xx/sx));
        if(flip)tx=sprite.width-1-tx;
        const auto pixel=sprite.pixels[static_cast<std::size_t>(std::min(sprite.height-1,static_cast<int>(yy/sy)))*sprite.width+tx];
        if((pixel>>24)>0)Blend(ox+xx,oy+yy,pixel,alpha);
    }
}

void Renderer::Text(const std::string& text,float x,float y,float size,std::uint32_t color,bool use_title,
                    aether::ui::TextAlign align,float width,float alpha) {
    if(!body_||!title_)return;
    auto& font=use_title?*title_:*body_;
    aether::ui::DrawList list;
    const aether::ui::Color c{(color&255)/255.f,((color>>8)&255)/255.f,((color>>16)&255)/255.f,alpha};
    list.AddText(font,text,size,{x,y,width,size*1.5f},c,align);
    for(const auto& q:list.quads) {
        if(q.rect.w<=0||q.rect.h<=0)continue;
        const int x0=std::max(0,static_cast<int>(std::floor(q.rect.x))),x1=std::min(kWidth,static_cast<int>(std::ceil(q.rect.Right())));
        const int y0=std::max(0,static_cast<int>(std::floor(q.rect.y))),y1=std::min(kHeight,static_cast<int>(std::ceil(q.rect.Bottom())));
        for(int yy=y0;yy<y1;++yy)for(int xx=x0;xx<x1;++xx) {
            const aether::ui::Vec2 uv{q.uv.x+((xx+.5f-q.rect.x)/q.rect.w)*q.uv.w,
                                      q.uv.y+((yy+.5f-q.rect.y)/q.rect.h)*q.uv.h};
            const float coverage=aether::ui::SdfCoverage(font.SampleDistance(uv),q);
            if(coverage>0)Blend(xx,yy,color,coverage*alpha);
        }
    }
}

void Renderer::TrackingText(const std::string& text,float x,float y,float size,float spacing,std::uint32_t color) {
    for(char c:text) {
        Text(std::string(1,c),x,y,size,color);
        x+=body_->GlyphOf(static_cast<unsigned char>(c),size).advance+spacing;
    }
}

void Renderer::Key(const std::string& label,float x,float y,float width) {
    Rounded(x,y,width,25,5,Rgb(36,54,57),.92f);
    Rect(x+5,y+24,width-10,1,quiet,.45f);
    Text(label,x,y+4,13,cream,false,aether::ui::TextAlign::Center,width);
}

void Renderer::Button(const std::string& text,float x,float y,float w,float h,const ViewOptions& options,bool primary) {
    const bool hover=Inside(options.mouse_x,options.mouse_y,x,y,w,h);
    Rounded(x,y,w,h,7,primary?(hover?Rgb(250,220,158):gold):(hover?Rgb(47,68,68):Rgb(25,42,45)),1);
    if(primary)Rect(x+13,y+2,w-26,1,cream,.55f);
    Text(text,x,y+(h-24)*.5f,24,primary?ink:cream,false,aether::ui::TextAlign::Center,w);
}

void Renderer::OnEvent(const Event& event) {
    const bool big=event.type==EventType::ShrineLit||event.type==EventType::Victory;
    const int count=big?75:event.type==EventType::Pulse?40:18;
    const auto color=event.type==EventType::Hurt?red:event.type==EventType::Dash?teal:gold;
    for(int i=0;i<count;++i) {
        const float angle=Random()*2*pi,speed=(big?35:20)+Random()*(big?155:85);
        const float life=.45f+Random()*(big?2.4f:.7f);
        particles_.push_back({event.x,event.y-18,std::cos(angle)*speed,std::sin(angle)*speed-25,life,life,1+Random()*2.2f,color});
    }
    if(event.type==EventType::Start) { toast_="Gather four embers. Hold E beside a shrine.";toast_time_=7;particles_.clear(); }
    if(event.type==EventType::Hurt) { flash_=.30f;toast_="The dark draws close. F repels the shades.";toast_time_=3; }
    if(event.type==EventType::Pickup&&event.value%4==0) {toast_="Your lantern is full. Find an unlit shrine.";toast_time_=3.5f;}
    if(event.type==EventType::ShrineLit) {
        toast_=event.value>=3?"The grove remembers. Return to the northern arch.":"A light rekindled. The forest stirs.";
        toast_time_=5;
    }
}

void Renderer::DrawWorld(const Game& game,const ViewOptions& options) {
    pixels_=background_;
    const float t=options.time;
    for(int i=0;i<42;++i) {
        const float x=120+std::fmod(i*117.73f+std::sin(t*.12f+i)*22,1040.0f);
        const float y=185+std::fmod(i*71.39f+t*(2.5f+(i%4)),515.0f);
        const float a=.25f+.25f*std::sin(t*1.2f+i*4.7f);
        Glow(x,y,12,i%3==0?gold:teal,a*.28f);
        Disc(x,y,1.0f,i%3==0?cream:teal,a);
    }
    game.World().ForEach<Position,Shrine>([&](Position& p,Shrine& shrine) {
        const float glow=shrine.lit?.25f:.035f;
        Glow(p.x,p.y-10,100,gold,glow);
        Ring(p.x,p.y+4,35,1.4f,shrine.lit?gold:quiet,shrine.lit?.75f:.35f);
        for(int i=0;i<4;++i) {
            const float a=i*pi*.5f-pi*.25f;
            Diamond(p.x+std::cos(a)*42,p.y+std::sin(a)*26,3,shrine.lit?gold:quiet,.8f);
        }
    });
    const auto gate=Game::GatePosition();
    if(game.LitShrines()==3) {
        Glow(gate.x,gate.y-15,100,gold,.25f+.05f*std::sin(t*2));
        Ring(gate.x,gate.y,34,2,gold,.85f);
        Text("THE WAY HOME",gate.x-100,gate.y-60,14,cream,false,aether::ui::TextAlign::Center,200);
    }
    game.World().ForEach<Position,Mote>([&](Position& p,Mote& mote) {
        if(mote.collected)return;
        const float y=p.y-4+std::sin(t*2.7f+mote.index*1.7f)*3.3f;
        Glow(p.x,y,30,gold,.24f);
        Ring(p.x,p.y+8,8,1,gold,.25f);
        Diamond(p.x,y,6,gold,.95f);
        Diamond(p.x,y-1,2.5f,cream);
    });
    struct Actor{Position p;const Wraith* enemy;bool shrine = false;};
    std::vector<Actor> actors;
    game.World().ForEach<Position,Wraith>([&](Position& p,Wraith& w){actors.push_back({p,&w});});
    game.World().ForEach<Position,Shrine>([&](Position& p,Shrine& s){if(s.lit)actors.push_back({p,nullptr,true});});
    actors.push_back({game.PlayerPosition(),nullptr});
    std::sort(actors.begin(),actors.end(),[](const Actor& a,const Actor& b){return a.p.y<b.p.y;});
    for(const auto& actor:actors) {
        const auto& p=actor.p;
        if(actor.shrine) {
            DrawSprite(shrine_,p.x,p.y+17);
            Glow(p.x,p.y-57,30,gold,.40f);
            Disc(p.x,p.y-57,3,cream,.95f);
            for(int i=0;i<5;++i) {
                const float phase=std::fmod(t*.4f+i*.2f,1.0f);
                Disc(p.x+std::sin(i*8.3f+t)*14,p.y-50-phase*65,1.4f,gold,1-phase);
            }
        } else if(actor.enemy) {
            const auto& w=*actor.enemy;
            const float opacity=game.LitShrines()==3?.18f:w.stunned>0?.50f:.95f;
            const auto color=w.stunned>0?gold:w.alert>.1f?red:Rgb(125,126,209);
            Glow(p.x,p.y-20,45,color,.14f*opacity);
            Disc(p.x,p.y+1,15,ink,.35f);
            DrawSprite(wraith_,p.x,p.y+std::sin(t*3+w.phase)*4,opacity);
            if(w.stunned>0) {
                for(int i=0;i<3;++i)Diamond(p.x+std::cos(t*4+i*2*pi/3)*17,p.y-67+std::sin(t*4+i*2*pi/3)*4,2,gold);
            } else if(w.alert>.1f&&game.LitShrines()<3) {
                Text("!",p.x-8,p.y-83,23,red,false,aether::ui::TextAlign::Center,16);
            }
        } else {
            const auto& player=game.Player();
            Glow(p.x,p.y-20,115,gold,.11f);
            Disc(p.x,p.y,17,ink,.45f);
            Ring(p.x,p.y,19,1,cream,.42f);
            const float opacity=player.invulnerable>0?.65f+.35f*std::sin(t*27):1;
            if(player.dash_time>0) {
                for(int i=3;i>0;--i)DrawSprite(keeper_,p.x-player.facing_x*i*16,p.y-player.facing_y*i*16,.08f*(4-i),player.facing_x<-.1f);
            }
            const float bob=std::sin(player.stride)*1.9f;
            DrawSprite(keeper_,p.x,p.y+bob,opacity,player.facing_x<-.1f);
            const float hand=player.facing_x<-.1f?-18:18;
            Glow(p.x+hand,p.y-35+bob,39,gold,.27f);
            if(player.pulse_age<.7f) {
                const float progress=player.pulse_age/.7f;
                Ring(p.x,p.y-15,10+progress*160,2.4f,gold,(1-progress)*.80f);
                Ring(p.x,p.y-15,7+progress*144,1,cream,(1-progress)*.35f);
            }
            if(game.ChannelProgress()>0)Ring(p.x,p.y-30,55,3.1f,gold,1,game.ChannelProgress());
        }
    }
    const bool animated=game.State()!=Phase::Paused;
    const float dt=animated?std::clamp(options.dt,0.0f,.1f):0;
    for(auto& particle:particles_) {
        particle.life-=dt;particle.x+=particle.vx*dt;particle.y+=particle.vy*dt;
        particle.vx*=1-dt*1.5f;particle.vy+=14*dt;
        const float a=std::max(0.0f,particle.life/particle.duration);
        if(a>0) {Glow(particle.x,particle.y,particle.radius*4,particle.color,a*.25f);Disc(particle.x,particle.y,particle.radius,particle.color,a);}
    }
    std::erase_if(particles_,[](const Particle& p){return p.life<=0;});
    flash_=std::max(0.0f,flash_-dt);
    if(flash_>0)Rect(0,0,kWidth,kHeight,red,flash_*.32f);
}

void Renderer::DrawHud(const Game& game,const ViewOptions& options) {
    using aether::ui::TextAlign;
    for(int y=0;y<126;++y)Rect(0,static_cast<float>(y),kWidth,1,ink,.80f*(1-y/126.0f));
    for(int y=696;y<kHeight;++y)Rect(0,static_cast<float>(y),kWidth,1,ink,.90f*(y-696)/104.0f);
    Diamond(38,40,10,gold);Diamond(38,40,5,ink);
    Text("LANTERNWAKE",59,22,29,cream,true);
    TrackingText("THE FORGOTTEN GROVE",61,55,9.5f,1.9f,quiet);
    Rounded(440,23,400,55,11,ink,.85f);
    Text(game.LitShrines()==3?"Return through the northern arch":"Rekindle the three shrines",456,33,17,cream,false,TextAlign::Center,368);
    for(int i=0;i<3;++i) {
        const float x=612.0f+i*28;
        Diamond(x,65,4,i<game.LitShrines()?gold:quiet,i<game.LitShrines()?1:.35f);
    }
    TrackingText("VITALITY",945,26,9.5f,1.6f,quiet);
    for(int i=0;i<5;++i)Diamond(952.0f+i*22,55,6,i<game.Player().health?gold:quiet,i<game.Player().health?1:.24f);
    // Keep hit targets and drawings identical at every aspect ratio.
    const bool mute_hover=Inside(options.mouse_x,options.mouse_y,1115,23,54,50);
    Rounded(1115,23,54,50,8,ink,mute_hover?1:.75f);
    Text(options.muted?"OFF":"ON",1115,31,14,options.muted?quiet:cream,false,TextAlign::Center,54);
    Text("M",1115,52,10,quiet,false,TextAlign::Center,54);
    const bool pause_hover=Inside(options.mouse_x,options.mouse_y,1180,23,54,50);
    Rounded(1180,23,54,50,8,ink,pause_hover?1:.75f);
    Rect(1199,36,4,17,cream);Rect(1209,36,4,17,cream);

    Rounded(33,686,234,63,9,ink,.88f);
    Glow(59,716,21,gold,.22f);Diamond(59,716,8,gold);Diamond(59,714,3,cream);
    Text(std::to_string(game.Player().carried)+" embers",81,695,23,cream);
    Text("4 for each sleeping shrine",81,724,12,quiet);
    Rounded(474,704,335,48,9,ink,.90f);
    Key("SPACE",486,714,58);
    Text("DASH",554,712,11,cream);
    Rect(554,733,62,3,Rgb(53,71,69));
    Rect(554,733,62*std::clamp(1-game.Player().dash_cooldown/1.05f,0.0f,1.0f),3,teal);
    Key("F",642,714,28);
    Text("LANTERN",680,712,11,cream);
    Rect(680,733,111,3,Rgb(53,71,69));Rect(680,733,111*game.Player().energy/100,3,gold);
    char time[32];const int secs=static_cast<int>(game.Time());std::snprintf(time,sizeof(time),"%02d:%02d",secs/60,secs%60);
    Text(time,1090,711,26,cream,true,TextAlign::Right,142);
    Text("JOURNEY TIME",1090,741,9,quiet,false,TextAlign::Right,142);
    Key("W A S D",36,764,72);Text("move",118,769,12,quiet);
    Key("E",193,764);Text("hold to offer",232,769,12,quiet);
    Text("F / click  repel     SPACE  dash     ESC  pause",786,770,12,quiet,false,TextAlign::Right,448);
    const int nearby=game.NearbyShrine();
    if(nearby>=0) {
        const auto& p=game.PlayerPosition();
        const std::string message=game.Player().carried>=4?"Hold E to kindle":"Find "+std::to_string(4-game.Player().carried)+" more embers";
        const float y=std::max(115.0f,p.y-125);
        Rounded(p.x-108,y,216,36,7,ink,.94f);
        Text(message,p.x-108,y+9,17,game.Player().carried>=4?cream:quiet,false,TextAlign::Center,216);
    } else if(game.NearGate()&&game.LitShrines()==3) {
        Rounded(531,217,218,38,7,ink,.95f);
        Text("E  Return to the light",531,226,17,cream,false,TextAlign::Center,218);
    }
    if(game.State()==Phase::Playing)toast_time_=std::max(0.0f,toast_time_-options.dt);
    if(toast_time_>0&&!toast_.empty()) {
        const float a=std::min(1.0f,toast_time_);
        Rounded(350,99,580,38,8,ink,a*.85f);
        Text(toast_,350,109,16,cream,false,TextAlign::Center,580,a);
    }
}

void Renderer::DrawMenu(const Game& game,const ViewOptions& options) {
    using aether::ui::TextAlign;
    if(game.State()==Phase::Title) {
        for(int x=0;x<kWidth;++x)Rect(static_cast<float>(x),0,1,kHeight,ink,.96f*std::pow(1-x/static_cast<float>(kWidth),1.2f));
        TrackingText("AN AETHER ORIGINAL",88,76,11,3.8f,quiet);
        Line(90,132,148,132,1,gold,.8f);Diamond(162,132,3,gold);
        TrackingText("A SMALL JOURNEY INTO THE DARK",88,222,10.5f,2.4f,gold);
        Text("LANTERNWAKE",80,263,90,cream,true);
        Text("Some things only need a little light.",90,367,28,cream,true);
        Text("Gather the lost embers. Wake the ancient shrines.",90,421,19,quiet);
        Text("Find your way home before the shadows find you.",90,449,19,quiet);
        Button("Enter the grove",90,517,276,62,options);
        Text("ENTER",384,539,12,quiet);
        Text("A complete little adventure, made with Aether.",90,614,14,quiet);
        Key("W A S D",90,665,74);Text("Move",175,672,13,quiet);
        Key("SPACE",249,665,59);Text("Dash",319,672,13,quiet);
        Key("F",389,665);Text("Light pulse",429,672,13,quiet);
        Key("E",540,665);Text("Offer embers",579,672,13,quiet);
        Text("HEADPHONES RECOMMENDED",90,751,10,quiet);
        Text("01  /  THE FORGOTTEN GROVE",907,751,11,quiet,false,TextAlign::Right,292);
        return;
    }
    Rect(0,0,kWidth,kHeight,ink,.76f);
    Rounded(365,158,550,489,15,ink,.97f);
    Rect(400,159,480,1,gold,.35f);
    const bool won=game.State()==Phase::Won;
    const bool lost=game.State()==Phase::Lost;
    Glow(640,223,55,won?gold:lost?red:teal,.16f);
    Diamond(640,221,13,won?gold:lost?red:quiet);Diamond(640,221,6,ink);
    const std::string headline=won?"The grove remembers.":lost?"Even lanterns fade.":"A quiet moment.";
    Text(headline,390,267,49,cream,true,TextAlign::Center,500);
    if(won||lost) {
        Text(won?"Three lights awake. One keeper returns.":"The forest can wait. Your light will return.",390,335,19,quiet,false,TextAlign::Center,500);
        const int secs=static_cast<int>(game.Time());char time[30];std::snprintf(time,sizeof(time),"%d:%02d",secs/60,secs%60);
        Text("SHRINES",425,390,10,quiet,false,TextAlign::Center,100);
        Text("JOURNEY",590,390,10,quiet,false,TextAlign::Center,100);
        Text("SCORE",755,390,10,quiet,false,TextAlign::Center,100);
        Text(std::to_string(game.LitShrines())+" / 3",425,412,32,gold,true,TextAlign::Center,100);
        Text(time,590,412,32,cream,true,TextAlign::Center,100);
        Text(std::to_string(game.Score()),755,412,32,cream,true,TextAlign::Center,100);
        Button(won?"Walk the grove again":"Rekindle your lantern",440,489,400,61,options);
        Text("R  to begin a new journey",440,572,13,quiet,false,TextAlign::Center,400);
    } else {
        Text("Your journey will be here when you return.",390,336,19,quiet,false,TextAlign::Center,500);
        Button("Continue the journey",440,398,400,59,options);
        Button("Begin again",440,471,400,52,options,false);
        Text("ESC  to continue     M  to toggle sound",400,566,13,quiet,false,TextAlign::Center,480);
        Text("WASD move  /  SPACE dash  /  F pulse  /  hold E offer",385,604,12,quiet,false,TextAlign::Center,510);
    }
}

UiAction Renderer::HitTest(Phase phase,float x,float y) const {
    if(phase==Phase::Title)return Inside(x,y,90,517,276,62)?UiAction::Start:UiAction::None;
    if(phase==Phase::Playing) {
        if(Inside(x,y,1115,23,54,50))return UiAction::ToggleMute;
        if(Inside(x,y,1180,23,54,50))return UiAction::Resume;
        return UiAction::None;
    }
    if(phase==Phase::Paused) {
        if(Inside(x,y,440,398,400,59))return UiAction::Resume;
        if(Inside(x,y,440,471,400,52))return UiAction::Restart;
    }
    if((phase==Phase::Won||phase==Phase::Lost)&&Inside(x,y,440,489,400,61))return UiAction::Restart;
    return UiAction::None;
}

void Renderer::Render(const Game& game,const ViewOptions& options) {
    DrawWorld(game,options);
    if(game.State()!=Phase::Title)DrawHud(game,options);
    if(game.State()!=Phase::Playing)DrawMenu(game,options);
}

bool Renderer::SaveScreenshot(const std::string& path,std::string& error) const {
    const auto bytes=std::span(reinterpret_cast<const aether::u8*>(pixels_.data()),pixels_.size()*sizeof(std::uint32_t));
    const auto png=aether::assets::EncodePngRgba8(kWidth,kHeight,bytes);
    std::ofstream file(path,std::ios::binary);
    if(!file){error="Cannot write screenshot: "+path;return false;}
    file.write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));
    if(!file){error="Screenshot write failed: "+path;return false;}
    return true;
}

} // namespace lanternwake
