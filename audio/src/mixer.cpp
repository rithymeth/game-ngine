#include "aether/audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace aether::audio {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kSmoothSeconds = 0.005f; // volume changes glide over about this long

// Catmull-Rom (cubic Hermite) through four samples, at `t` between y1 and y2.
f32 Hermite(f32 y0, f32 y1, f32 y2, f32 y3, f32 t) {
    const f32 c1 = 0.5f * (y2 - y0);
    const f32 c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const f32 c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

} // namespace

Mixer::Mixer(u32 sample_rate) : sample_rate_(std::max(sample_rate, 1u)) {
    Bus master;
    master.name = "Master";
    buses_.push_back(std::move(master));
    game_buses_.resize(1);
    voices_.reserve(256); // the audio thread rarely has to grow it
}

Mixer::~Mixer() {
    // Nothing renders any more: apply what's queued (it may hand over effects and streams), then free it.
    if (commands_) {
        ApplyCommands();
        CollectGarbage();
    }
}

// --- Buses ------------------------------------------------------------------------------------

BusId Mixer::AddBus(const std::string& name, BusId parent) {
    if (threaded_) return kInvalidBus;
    if (parent >= buses_.size()) parent = kMasterBus;
    Bus b;
    b.name = name;
    b.parent = parent;
    buses_.push_back(std::move(b));
    game_buses_.emplace_back();
    return static_cast<BusId>(buses_.size() - 1);
}

void Mixer::AddDefaultBuses() {
    for (const char* name : {"Music", "SFX", "UI", "Voice"}) {
        if (FindBus(name) == kInvalidBus) AddBus(name);
    }
}

BusId Mixer::FindBus(const std::string& name) const {
    for (usize i = 0; i < buses_.size(); ++i) {
        if (buses_[i].name == name) return static_cast<BusId>(i);
    }
    return kInvalidBus;
}

bool Mixer::DoSetBusVolume(BusId bus, f32 db) {
    buses_[bus].volume_db = db;
    buses_[bus].gain = DbToGain(db);
    return true;
}

bool Mixer::SetBusVolume(BusId bus, f32 db) {
    if (bus >= buses_.size()) return false;
    game_buses_[bus].volume_db = db;
    if (!threaded_) return DoSetBusVolume(bus, db);
    Submit([bus, db](Mixer& m) { m.DoSetBusVolume(bus, db); });
    return true;
}

f32 Mixer::BusVolume(BusId bus) const { return bus < game_buses_.size() ? game_buses_[bus].volume_db : 0.0f; }

bool Mixer::SetBusMuted(BusId bus, bool muted) {
    if (bus >= buses_.size()) return false;
    game_buses_[bus].muted = muted;
    if (!threaded_) {
        buses_[bus].muted = muted;
        return true;
    }
    Submit([bus, muted](Mixer& m) { m.buses_[bus].muted = muted; });
    return true;
}

bool Mixer::BusMuted(BusId bus) const { return bus < game_buses_.size() && game_buses_[bus].muted; }

AudioEffect* Mixer::AddEffect(BusId bus, std::unique_ptr<AudioEffect> effect) {
    if (bus >= buses_.size() || !effect) return nullptr;
    AudioEffect* raw = effect.release();
    game_buses_[bus].effects.push_back(raw);
    if (!threaded_) {
        buses_[bus].effects.emplace_back(raw);
    } else {
        Submit([bus, raw](Mixer& m) { m.buses_[bus].effects.emplace_back(raw); }); // owned from when it's applied
    }
    return raw;
}

BusMeter Mixer::Meter(BusId bus) const {
    if (!threaded_) return bus < buses_.size() ? buses_[bus].meter : BusMeter{};
    const Snapshot& s = View();
    return bus < s.meters.size() ? s.meters[bus] : BusMeter{};
}

// --- Voices -----------------------------------------------------------------------------------

Mixer::Voice* Mixer::Find(VoiceId id) {
    for (Voice& v : voices_) {
        if (v.id == id) return &v;
    }
    return nullptr;
}
const Mixer::Voice* Mixer::Find(VoiceId id) const {
    for (const Voice& v : voices_) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

VoiceId Mixer::NewId() {
    const VoiceId id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    return id;
}

VoiceId Mixer::Play(const SoundWave* sound, const PlayParams& p) {
    if (sound == nullptr || sound->Frames() == 0 || sound->sample_rate == 0 || p.bus >= buses_.size()) return 0;
    const VoiceId id = NewId();
    if (!threaded_) {
        Voice v;
        v.sound = sound;
        Start(std::move(v), p, id, frames_rendered_);
        return id;
    }
    const u64 issued = View().frames;
    const u64 seq = Submit([sound, p, id, issued](Mixer& m) {
        Voice v;
        v.sound = sound;
        m.Start(std::move(v), p, id, issued);
    });
    pending_plays_.emplace_back(seq, id);
    if (p.spatial && p.occlusion) game_occluded_[id] = p.position;
    return id;
}

VoiceId Mixer::PlayStream(std::unique_ptr<AudioStream> stream, const PlayParams& p) {
    if (stream == nullptr || stream->Frames() == 0 || stream->SampleRate() == 0 || stream->Channels() < 1 || stream->Channels() > 2 ||
        p.bus >= buses_.size()) {
        return 0;
    }
    const VoiceId id = NewId();
    auto start = [p, id](Mixer& m, AudioStream* raw, u64 issued) {
        Voice v;
        v.stream.reset(raw);
        v.window_start = std::numeric_limits<i64>::min() / 2; // nothing decoded yet: the first Fill seeks
        m.Start(std::move(v), p, id, issued);
    };
    if (!threaded_) {
        start(*this, stream.release(), frames_rendered_);
        return id;
    }
    AudioStream* raw = stream.release(); // owned by the voice once the command is applied
    const u64 issued = View().frames;
    const u64 seq = Submit([start, raw, issued](Mixer& m) { start(m, raw, issued); });
    pending_plays_.emplace_back(seq, id);
    if (p.spatial && p.occlusion) game_occluded_[id] = p.position;
    return id;
}

void Mixer::Start(Voice v, const PlayParams& p, VoiceId id, u64 issued_at) {
    v.id = id;
    v.bus = p.bus;
    v.position = std::clamp(static_cast<f64>(p.start_time) * Rate(v), 0.0, static_cast<f64>(Frames(v)));
    v.pitch = std::max(p.pitch, 0.0f);
    v.gain = v.current_gain = DbToGain(p.volume_db);
    v.pan = std::clamp(p.pan, -1.0f, 1.0f);
    v.loop = p.loop;
    // The delay counts from when the game asked (time spent in the queue comes
    // off it), or from its sync group's start.
    const u64 delay = static_cast<u64>(std::llround(std::max(p.delay, 0.0) * sample_rate_));
    if (p.sync_group != 0) {
        const u64 base = sync_bases_.try_emplace(p.sync_group, frames_rendered_).first->second;
        v.delay_frames = base + delay > frames_rendered_ ? base + delay - frames_rendered_ : 0;
    } else {
        const u64 queued = frames_rendered_ > issued_at ? frames_rendered_ - issued_at : 0;
        v.delay_frames = delay > queued ? delay - queued : 0;
    }
    v.priority = p.priority;
    v.virtual_mode = p.virtual_mode;
    v.spatial = p.spatial;
    v.occlusion = p.occlusion;
    v.position3d = p.position;
    v.velocity = p.velocity;
    v.spatial_blend = std::clamp(p.spatial_blend, 0.0f, 1.0f);
    v.doppler = std::max(p.doppler, 0.0f);
    v.attenuation = p.attenuation;
    if (p.fade_in > 0.0f) {
        v.fade = 0.0f;
        v.fade_step = 1.0f / (p.fade_in * static_cast<f32>(sample_rate_));
    }
    voices_.push_back(std::move(v));
}

bool Mixer::DoStop(VoiceId id, f32 fade_out) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    if (fade_out <= 0.0f) {
        voices_.erase(voices_.begin() + (v - voices_.data()));
        return true;
    }
    v->stopping = true;
    v->fade_step = -std::max(v->fade, 1e-6f) / (fade_out * static_cast<f32>(sample_rate_));
    return true;
}

bool Mixer::Stop(VoiceId id, f32 fade_out) {
    if (!threaded_) return DoStop(id, fade_out);
    if (!IsPlaying(id)) return false;
    const u64 seq = Submit([id, fade_out](Mixer& m) { m.DoStop(id, fade_out); });
    if (fade_out <= 0.0f) pending_stops_.emplace_back(seq, id);
    return true;
}

void Mixer::StopAll() {
    if (!threaded_) {
        voices_.clear();
        return;
    }
    stop_all_seq_ = Submit([](Mixer& m) { m.voices_.clear(); });
    pending_plays_.clear();
    game_occluded_.clear();
}

// Applies `edit` to the voice now, or queues it (threaded). False for voices that aren't playing.
#define AETHER_VOICE_EDIT(edit)                                                      \
    if (!threaded_) {                                                                \
        Voice* v = Find(id);                                                         \
        if (v == nullptr) return false;                                              \
        edit;                                                                        \
        return true;                                                                 \
    }                                                                                \
    if (!IsPlaying(id)) return false;                                                \
    Submit([=](Mixer& m) {                                                           \
        if (Voice* v = m.Find(id)) { edit; }                                         \
    });                                                                              \
    return true

bool Mixer::SetVolume(VoiceId id, f32 db) { AETHER_VOICE_EDIT(v->gain = DbToGain(db)); }
bool Mixer::SetPitch(VoiceId id, f32 pitch) { AETHER_VOICE_EDIT(v->pitch = std::max(pitch, 0.0f)); }
bool Mixer::SetPan(VoiceId id, f32 pan) { AETHER_VOICE_EDIT(v->pan = std::clamp(pan, -1.0f, 1.0f)); }
bool Mixer::SetOcclusion(VoiceId id, f32 amount) { AETHER_VOICE_EDIT(v->occlusion_target = std::clamp(amount, 0.0f, 1.0f)); }
bool Mixer::SetPosition(VoiceId id, const Vec3& position, const Vec3& velocity) {
    if (threaded_) {
        if (auto it = game_occluded_.find(id); it != game_occluded_.end()) it->second = position;
    }
    AETHER_VOICE_EDIT((v->position3d = position, v->velocity = velocity));
}

#undef AETHER_VOICE_EDIT

void Mixer::SetListener(const Listener& listener) {
    game_listener_ = listener;
    if (!threaded_) {
        listener_ = listener;
        return;
    }
    Submit([listener](Mixer& m) { m.listener_ = listener; });
}

void Mixer::SetMaxVoices(u32 count) {
    game_max_voices_ = std::max(count, 1u);
    if (!threaded_) {
        max_voices_ = game_max_voices_;
        return;
    }
    Submit([n = game_max_voices_](Mixer& m) { m.max_voices_ = n; });
}

void Mixer::UpdateOcclusion() {
    if (!occlusion_query) return;
    if (!threaded_) {
        for (Voice& v : voices_) {
            if (v.spatial && v.occlusion) v.occlusion_target = std::clamp(occlusion_query(listener_.position, v.position3d), 0.0f, 1.0f);
        }
        return;
    }
    // Threaded: ask on the game thread, from where the game last put each voice.
    for (auto it = game_occluded_.begin(); it != game_occluded_.end();) {
        if (!IsPlaying(it->first)) {
            it = game_occluded_.erase(it);
            continue;
        }
        SetOcclusion(it->first, occlusion_query(game_listener_.position, it->second));
        ++it;
    }
}

bool Mixer::GetVoiceInfo(VoiceId id, VoiceInfo& out) const {
    if (threaded_) {
        if (PendingStop(id)) return false;
        const Snapshot::VoiceState* v = Seen(id);
        if (v == nullptr) return false;
        out = v->info;
        return true;
    }
    const Voice* v = Find(id);
    if (v == nullptr) return false;
    out = v->info;
    out.is_virtual = v->is_virtual;
    return true;
}

usize Mixer::RealVoiceCount() const {
    if (threaded_) return View().real;
    return static_cast<usize>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.is_virtual; }));
}

usize Mixer::VirtualVoiceCount() const {
    const usize all = VoiceCount(), real = RealVoiceCount();
    return all > real ? all - real : 0;
}

usize Mixer::VoiceCount() const {
    if (!threaded_) return voices_.size();
    const Snapshot& s = View();
    usize n = pending_plays_.size();
    if (stop_all_seq_ <= s.applied) {
        for (const auto& v : s.voices) n += PendingStop(v.id) ? 0 : 1;
    }
    for (const auto& [seq, id] : pending_stops_) {
        for (const auto& [play_seq, play_id] : pending_plays_) n -= play_id == id ? 1 : 0;
    }
    return n;
}

bool Mixer::IsPlaying(VoiceId id) const {
    if (!threaded_) return Find(id) != nullptr;
    if (PendingStop(id)) return false;
    for (const auto& [seq, pending] : pending_plays_) {
        if (pending == id) return true;
    }
    return Seen(id) != nullptr;
}

f32 Mixer::PlaybackTime(VoiceId id) const {
    if (threaded_) {
        if (PendingStop(id)) return -1.0f;
        if (const Snapshot::VoiceState* v = Seen(id)) return v->time;
        return IsPlaying(id) ? 0.0f : -1.0f; // queued, not started yet
    }
    const Voice* v = Find(id);
    if (v == nullptr) return -1.0f;
    const f64 n = static_cast<f64>(Frames(*v));
    const f64 at = v->stream && v->loop ? std::fmod(v->position, n) : v->position; // streams count past loops
    return static_cast<f32>(at / Rate(*v));
}

std::vector<Mixer::VoiceSummary> Mixer::Voices() const {
    std::vector<VoiceSummary> out;
    if (threaded_) {
        const Snapshot& s = View();
        if (stop_all_seq_ > s.applied) return out;
        for (const auto& v : s.voices) {
            if (!PendingStop(v.id)) out.push_back({v.id, v.info, v.time});
        }
        return out;
    }
    for (const Voice& v : voices_) {
        VoiceSummary sum{v.id, v.info, PlaybackTime(v.id)};
        sum.info.is_virtual = v.is_virtual;
        out.push_back(sum);
    }
    std::sort(out.begin(), out.end(), [](const VoiceSummary& a, const VoiceSummary& b) { return a.id < b.id; });
    return out;
}

u64 Mixer::FramesRendered() const { return threaded_ ? View().frames : frames_rendered_; }

// --- Threading ----------------------------------------------------------------------------------

u32 Mixer::BeginSyncGroup() {
    const u32 group = next_sync_group_++;
    if (next_sync_group_ == 0) next_sync_group_ = 1;
    Post([group](Mixer& m) { m.sync_bases_[group] = m.frames_rendered_; });
    return group;
}

void Mixer::EndSyncGroup(u32 group) {
    if (group != 0) Post([group](Mixer& m) { m.sync_bases_.erase(group); });
}

void Mixer::Post(std::function<void(Mixer&)> edit) {
    if (!threaded_) {
        edit(*this);
        return;
    }
    Submit(std::move(edit));
}

u64 Mixer::Submit(std::function<void(Mixer&)> apply) {
    Command* c = new Command{next_seq_++, std::move(apply)};
    CollectGarbage();
    while (!commands_->Push(c)) {
        // Full: the audio thread is behind. Wait for it (it frees a slot each command).
        std::this_thread::yield();
        CollectGarbage();
    }
    return c->seq;
}

void Mixer::ApplyCommands() {
    Command* c = nullptr;
    while (commands_->Pop(c)) {
        c->apply(*this);
        applied_ = c->seq;
        if (!garbage_->Push(c)) delete c; // the game thread hasn't collected: free it here rather than wait
    }
}

void Mixer::CollectGarbage() const {
    Command* c = nullptr;
    while (garbage_->Pop(c)) delete c;
}

void Mixer::Publish() {
    Snapshot& s = slots_[back_];
    s.applied = applied_;
    s.frames = frames_rendered_;
    s.voices.clear();
    s.real = 0;
    for (const Voice& v : voices_) {
        Snapshot::VoiceState state;
        state.id = v.id;
        state.info = v.info;
        state.info.is_virtual = v.is_virtual;
        const f64 n = static_cast<f64>(Frames(v));
        state.time = static_cast<f32>((v.stream && v.loop ? std::fmod(v.position, n) : v.position) / Rate(v));
        s.voices.push_back(state);
        s.real += v.is_virtual ? 0 : 1;
    }
    std::sort(s.voices.begin(), s.voices.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    s.meters.resize(buses_.size());
    for (usize i = 0; i < buses_.size(); ++i) s.meters[i] = buses_[i].meter;
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & 3u;
}

const Mixer::Snapshot& Mixer::View() const {
    if (middle_.load(std::memory_order_acquire) & kFresh) front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3u;
    CollectGarbage();
    // Changes the audio thread has applied aren't pending any more.
    const u64 applied = slots_[front_].applied;
    while (!pending_plays_.empty() && pending_plays_.front().first <= applied) pending_plays_.pop_front();
    while (!pending_stops_.empty() && pending_stops_.front().first <= applied) pending_stops_.pop_front();
    return slots_[front_];
}

const Mixer::Snapshot::VoiceState* Mixer::Seen(VoiceId id) const {
    const Snapshot& s = View();
    if (stop_all_seq_ > s.applied) return nullptr; // everything it saw is being stopped
    const auto it = std::lower_bound(s.voices.begin(), s.voices.end(), id, [](const auto& v, VoiceId x) { return v.id < x; });
    return it != s.voices.end() && it->id == id ? &*it : nullptr;
}

bool Mixer::PendingStop(VoiceId id) const {
    View();
    for (const auto& [seq, stopped] : pending_stops_) {
        if (stopped == id) return true;
    }
    return false;
}

void Mixer::SetThreaded(bool threaded) {
    if (threaded == threaded_) return;
    if (threaded) {
        if (!commands_) {
            commands_ = std::make_unique<SpscQueue<Command*>>(8192);
            garbage_ = std::make_unique<SpscQueue<Command*>>(8192);
        }
        game_listener_ = listener_;
        for (usize i = 0; i < buses_.size(); ++i) {
            game_buses_[i].volume_db = buses_[i].volume_db;
            game_buses_[i].muted = buses_[i].muted;
        }
        game_max_voices_ = max_voices_;
        pending_plays_.clear();
        pending_stops_.clear();
        stop_all_seq_ = 0;
        game_occluded_.clear();
        for (const Voice& v : voices_) {
            if (v.spatial && v.occlusion) game_occluded_[v.id] = v.position3d;
        }
        applied_ = next_seq_ - 1;
        Publish(); // a first view, before the audio thread starts
        threaded_ = true;
        View();
    } else {
        // Nothing renders now: catch up here.
        ApplyCommands();
        CollectGarbage();
        threaded_ = false;
    }
}

void Mixer::Spatialize(Voice& v, u32 frames, const std::vector<f32>& bus_gains) {
    const f32 rate = static_cast<f32>(sample_rate_);
    VoiceInfo& info = v.info;
    // Occlusion follows its target smoothly (or jumps there on the first block).
    const f32 follow = !v.started || occlusion_smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-static_cast<f32>(frames) / (occlusion_smoothing * rate));
    info.occlusion += (v.occlusion_target - info.occlusion) * follow;
    const f32 occlusion_gain = DbToGain(occlusion_volume_db * info.occlusion);
    const f32 occlusion_cutoff = kOpenCutoff * std::pow(std::clamp(occlusion_lowpass_hz, 10.0f, kOpenCutoff) / kOpenCutoff, info.occlusion);
    if (v.spatial) {
        const f32 blend = v.spatial_blend;
        info.distance = (v.position3d - listener_.position).Length();
        info.attenuation = 1.0f + (Attenuate(v.attenuation, info.distance) - 1.0f) * blend;
        info.pan = v.pan + (PanFromListener(listener_, v.position3d) - v.pan) * blend;
        const f32 doppler = v.doppler > 0.0f ? DopplerRatio(listener_, v.position3d, v.velocity, v.doppler, speed_of_sound) : 1.0f;
        info.pitch = v.pitch * (1.0f + (doppler - 1.0f) * blend);
        const f32 air = kOpenCutoff * std::pow(AirAbsorptionCutoff(v.attenuation, info.distance) / kOpenCutoff, blend);
        info.lowpass_hz = std::min(air, occlusion_cutoff);
    } else {
        info.distance = 0.0f;
        info.attenuation = 1.0f;
        info.pan = v.pan;
        info.pitch = v.pitch;
        info.lowpass_hz = occlusion_cutoff;
    }
    v.spatial_gain = info.attenuation * occlusion_gain;
    info.audibility = v.gain * v.spatial_gain * bus_gains[v.bus];
    if (!v.started) v.current_gain = v.gain * v.spatial_gain; // no glide from full volume on the first block
}

void Mixer::AssignChannels() {
    const f32 threshold = DbToGain(virtual_threshold_db);
    std::vector<Voice*> order;
    order.reserve(voices_.size());
    for (Voice& v : voices_) {
        if (v.info.audibility < threshold) {
            v.is_virtual = true;
        } else {
            order.push_back(&v);
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Voice* a, const Voice* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->info.audibility > b->info.audibility; // ties keep the older voice (stable)
    });
    for (usize i = 0; i < order.size(); ++i) order[i]->is_virtual = i >= max_voices_;
}

bool Mixer::AdvanceVirtual(Voice& v, u32 frames) {
    const u64 wait = std::min<u64>(v.delay_frames, frames);
    v.delay_frames -= wait;
    frames -= static_cast<u32>(wait);
    const f64 n = static_cast<f64>(Frames(v));
    if (v.virtual_mode == VirtualMode::Continue) {
        v.position += static_cast<f64>(v.info.pitch) * Rate(v) / sample_rate_ * frames;
        if (v.position >= n) {
            if (!v.loop) return false;
            v.position = std::fmod(v.position, n); // a stream seeks when it's heard again
        }
    }
    v.fade = std::clamp(v.fade + v.fade_step * static_cast<f32>(frames), 0.0f, 1.0f);
    if (v.fade_step > 0.0f && v.fade >= 1.0f) v.fade_step = 0.0f;
    return !(v.stopping && v.fade <= 0.0f);
}

void Mixer::Fill(Voice& v, i64 from, i64 to) {
    AudioStream& s = *v.stream;
    const u32 ch = s.Channels();
    const i64 n = static_cast<i64>(s.Frames());
    i64 end = v.window_start + static_cast<i64>(v.window.size() / ch);
    if (from < v.window_start || from > end) {
        // Somewhere new (the start, back from virtual, a restart): seek.
        v.window.clear();
        v.window_start = end = from;
        if (from < 0) {
            v.window.assign(static_cast<usize>(-from) * ch, 0.0f); // before the start is silence
            end = 0;
        }
        s.Seek(static_cast<u64>(v.loop ? end % n : std::min(end, n)));
    } else if (from > v.window_start) {
        v.window.erase(v.window.begin(), v.window.begin() + static_cast<std::ptrdiff_t>((from - v.window_start) * ch));
        v.window_start = from;
    }
    constexpr i64 kChunk = 4096; // decode ahead in chunks, not per block
    int dry = 0;                 // reads that returned nothing, to stop on a broken stream
    while (end < to) {
        const i64 want = std::max(to - end, kChunk);
        const usize old = v.window.size();
        v.window.resize(old + static_cast<usize>(want) * ch);
        const u32 got = s.Read(v.window.data() + old, static_cast<u32>(want));
        v.window.resize(old + static_cast<usize>(got) * ch);
        end += got;
        if (static_cast<i64>(got) < want) {
            dry = got == 0 ? dry + 1 : 0;
            if (v.loop && dry < 2) {
                s.Seek(0);
                continue;
            }
            v.window.resize(v.window.size() + static_cast<usize>(std::max<i64>(to - end, 0)) * ch, 0.0f); // past the end is silence
            break;
        }
    }
}

bool Mixer::MixVoice(Voice& v, f32* out, u32 frames, f32 ramp_from, f32 ramp_to) {
    // A delayed start: silence until it's due.
    const u32 wait = static_cast<u32>(std::min<u64>(v.delay_frames, frames));
    v.delay_frames -= wait;
    const i64 n = static_cast<i64>(Frames(v));
    const u32 ch = Channels(v);
    const f64 step = static_cast<f64>(v.info.pitch) * Rate(v) / sample_rate_;
    const bool streamed = v.stream != nullptr;
    if (streamed && wait < frames) {
        const i64 from = static_cast<i64>(std::floor(v.position)) - 1;
        Fill(v, from, static_cast<i64>(std::floor(v.position + step * (frames - wait))) + 3);
    }
    const i64 window_frames = streamed ? static_cast<i64>(v.window.size() / ch) : 0;
    const f32 smooth = 1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<f32>(sample_rate_)));
    const f32 target = v.gain * v.spatial_gain;
    // One-pole low-pass for distance and occlusion; fully open passes the input through.
    const f32 lp = v.info.lowpass_hz >= kOpenCutoff ? 1.0f : 1.0f - std::exp(-2.0f * kPi * v.info.lowpass_hz / static_cast<f32>(sample_rate_));
    // Pan: equal power for mono, balance for stereo.
    f32 pan_l, pan_r;
    const f32 pan = std::clamp(v.info.pan, -1.0f, 1.0f);
    if (ch == 1) {
        const f32 angle = (pan + 1.0f) * kPi / 4.0f;
        pan_l = std::cos(angle), pan_r = std::sin(angle);
    } else {
        pan_l = pan > 0.0f ? 1.0f - pan : 1.0f;
        pan_r = pan < 0.0f ? 1.0f + pan : 1.0f;
    }
    auto at = [&](i64 frame, u32 c) -> f32 {
        if (streamed) {
            const i64 k = frame - v.window_start;
            return k >= 0 && k < window_frames ? v.window[static_cast<usize>(k) * ch + c] : 0.0f;
        }
        if (v.loop) frame = ((frame % n) + n) % n;
        else if (frame < 0 || frame >= n) return 0.0f;
        return v.sound->samples[static_cast<usize>(frame) * ch + c];
    };
    for (u32 i = wait; i < frames; ++i) {
        if (v.position >= static_cast<f64>(n) && !(streamed && v.loop)) {
            if (!v.loop) return false;
            v.position = std::fmod(v.position, static_cast<f64>(n));
        }
        const i64 base = static_cast<i64>(v.position);
        const f32 t = static_cast<f32>(v.position - static_cast<f64>(base));
        v.current_gain += (target - v.current_gain) * smooth;
        v.fade = std::clamp(v.fade + v.fade_step, 0.0f, 1.0f);
        if (v.fade_step > 0.0f && v.fade >= 1.0f) v.fade_step = 0.0f;
        const f32 ramp = ramp_from + (ramp_to - ramp_from) * static_cast<f32>(i + 1) / static_cast<f32>(frames);
        const f32 g = v.current_gain * v.fade * ramp;
        f32 left = Hermite(at(base - 1, 0), at(base, 0), at(base + 1, 0), at(base + 2, 0), t);
        f32 right = ch == 1 ? left : Hermite(at(base - 1, 1), at(base, 1), at(base + 1, 1), at(base + 2, 1), t);
        v.lowpass[0] += lp * (left - v.lowpass[0]);
        v.lowpass[1] += lp * (right - v.lowpass[1]);
        left = v.lowpass[0], right = v.lowpass[1];
        out[2 * i] += left * g * pan_l;
        out[2 * i + 1] += right * g * pan_r;
        v.position += step;
        if (v.loop && !streamed && v.position >= static_cast<f64>(n)) v.position = std::fmod(v.position, static_cast<f64>(n)); // times stay inside the sound
        if (v.stopping && v.fade <= 0.0f) return false;
    }
    return true;
}

void Mixer::Render(f32* out, u32 frames) {
    if (threaded_) ApplyCommands();
    for (Bus& b : buses_) b.buffer.assign(static_cast<usize>(frames) * 2, 0.0f);
    // Each bus's gain all the way to Master, for audibility. Parents come first.
    std::vector<f32> bus_gains(buses_.size(), 1.0f);
    for (usize i = 0; i < buses_.size(); ++i) {
        const f32 own = buses_[i].muted ? 0.0f : buses_[i].gain;
        bus_gains[i] = i == kMasterBus ? own : own * bus_gains[buses_[i].parent];
    }
    for (Voice& v : voices_) Spatialize(v, frames, bus_gains);
    AssignChannels();
    for (usize i = 0; i < voices_.size();) {
        Voice& v = voices_[i];
        if (!v.started) v.was_virtual = v.is_virtual; // a new voice starts where it's put, without a ramp
        v.started = true;
        bool keep = true;
        if (!v.is_virtual) {
            if (v.was_virtual) {
                // Back from virtual: fade in over this block.
                if (v.virtual_mode == VirtualMode::Restart) v.position = 0.0;
                v.current_gain = v.gain * v.spatial_gain;
                v.lowpass[0] = v.lowpass[1] = 0.0f;
                keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 0.0f, 1.0f);
            } else {
                keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 1.0f, 1.0f);
            }
        } else if (!v.was_virtual) {
            // Just lost its channel: fade out over this block.
            keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 1.0f, 0.0f) && v.virtual_mode != VirtualMode::Stop;
        } else {
            keep = v.virtual_mode != VirtualMode::Stop && AdvanceVirtual(v, frames);
        }
        v.was_virtual = v.is_virtual;
        if (keep) {
            ++i;
        } else {
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    frames_rendered_ += frames;
    // Children have higher ids than their parents: walk backwards so each bus is done before its parent.
    for (usize idx = buses_.size(); idx-- > 0;) {
        Bus& b = buses_[idx];
        f32* data = b.buffer.data();
        for (auto& e : b.effects) {
            if (!e->bypass) e->Process(data, frames, sample_rate_);
        }
        const f32 target = b.muted ? 0.0f : b.gain;
        BusMeter meter;
        f64 sum[2] = {0, 0};
        for (u32 i = 0; i < frames; ++i) {
            const f32 g = b.current_gain + (target - b.current_gain) * static_cast<f32>(i + 1) / static_cast<f32>(frames);
            for (int c = 0; c < 2; ++c) {
                f32& x = data[2 * i + c];
                x *= g;
                meter.peak[c] = std::max(meter.peak[c], std::fabs(x));
                sum[c] += static_cast<f64>(x) * x;
            }
        }
        b.current_gain = target;
        for (int c = 0; c < 2; ++c) meter.rms[c] = frames > 0 ? static_cast<f32>(std::sqrt(sum[c] / frames)) : 0.0f;
        b.meter = meter;
        if (idx != kMasterBus) {
            f32* parent = buses_[b.parent].buffer.data();
            for (usize i = 0; i < static_cast<usize>(frames) * 2; ++i) parent[i] += data[i];
        }
    }
    std::copy(buses_[kMasterBus].buffer.begin(), buses_[kMasterBus].buffer.end(), out);
    if (threaded_) Publish();
}

} // namespace aether::audio
