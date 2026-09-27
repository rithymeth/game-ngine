#include "aether/audio/cue_player.h"

#include <algorithm>
#include <cmath>

namespace aether::audio {

// --- SoundBank ----------------------------------------------------------------------------------

void SoundBank::Add(const std::string& name, SoundWave sound) {
    Entry e;
    e.duration = sound.Duration();
    e.wave = std::make_shared<const SoundWave>(std::move(sound));
    entries_[name] = std::move(e);
}

bool SoundBank::AddStreamed(const std::string& name, std::shared_ptr<const std::vector<u8>> bytes, std::string* error) {
    const auto probe = OpenStream(bytes, error);
    if (probe == nullptr) return false;
    Entry e;
    e.duration = static_cast<f64>(probe->Frames()) / probe->SampleRate();
    e.bytes = std::move(bytes);
    entries_[name] = std::move(e);
    return true;
}

bool SoundBank::Contains(const std::string& name) const { return entries_.count(name) != 0; }

f64 SoundBank::Duration(const std::string& name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? -1.0 : it->second.duration;
}

const SoundWave* SoundBank::Find(const std::string& name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.wave.get();
}

bool SoundBank::IsStreamed(const std::string& name) const {
    const auto it = entries_.find(name);
    return it != entries_.end() && it->second.bytes != nullptr;
}

std::unique_ptr<AudioStream> SoundBank::Open(const std::string& name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() || it->second.bytes == nullptr ? nullptr : OpenStream(it->second.bytes);
}

// --- CuePlayer ----------------------------------------------------------------------------------

CueState& CuePlayer::StateOf(const SoundCue& cue) { return states_.try_emplace(&cue, seed_ + states_.size() * 0x9E3779B97F4A7C15ull).first->second; }

SoundDuration CuePlayer::Durations() const {
    return [this](const std::string& name) { return bank_.Duration(name); };
}

CuePlayer::Instance* CuePlayer::Find(CueHandle h) {
    for (Instance& i : instances_) {
        if (i.handle == h) return &i;
    }
    return nullptr;
}
const CuePlayer::Instance* CuePlayer::Find(CueHandle h) const {
    for (const Instance& i : instances_) {
        if (i.handle == h) return &i;
    }
    return nullptr;
}

void CuePlayer::Schedule(Instance& in, const CuePlan& plan) {
    const SoundCue& cue = *in.cue;
    const f64 rate = mixer_.SampleRate();
    BusId bus = mixer_.FindBus(cue.bus);
    if (bus == Mixer::kInvalidBus) bus = kMasterBus;
    for (const CueItem& item : plan.items) {
        // Every sound is placed from the cue's start frame, so nothing drifts however long it runs.
        const u64 at = in.start_frame + static_cast<u64>(std::llround(std::max(item.offset, 0.0) * rate));
        const u64 now = mixer_.FramesRendered();
        PlayParams p;
        p.bus = bus;
        p.volume_db = cue.volume_db + in.params.volume_db + item.volume_db;
        p.pitch = cue.pitch * in.params.pitch * item.pitch;
        p.loop = item.loop;
        p.delay = at > now ? static_cast<f64>(at - now) / rate : 0.0;
        p.fade_in = item.offset <= 0.0 ? in.params.fade_in : 0.0f;
        p.priority = cue.priority;
        p.virtual_mode = cue.virtual_mode;
        p.spatial = cue.spatial && !in.params.force_2d;
        p.position = in.params.position;
        p.velocity = in.params.velocity;
        p.spatial_blend = cue.spatial_blend;
        p.attenuation = cue.attenuation;
        p.doppler = cue.doppler;
        p.occlusion = cue.occlusion;
        VoiceId id = 0;
        if (const SoundWave* wave = bank_.Find(item.sound)) {
            id = mixer_.Play(wave, p);
        } else {
            id = mixer_.PlayStream(bank_.Open(item.sound), p);
        }
        if (id != 0) in.voices.emplace_back(id, item.volume_db);
    }
    in.tails.insert(in.tails.end(), plan.tails.begin(), plan.tails.end());
}

CueHandle CuePlayer::Play(const SoundCue& cue, const CuePlayParams& params) {
    Instance in;
    in.handle = next_handle_++;
    if (next_handle_ == 0) next_handle_ = 1;
    in.cue = &cue;
    in.params = params;
    in.start_frame = mixer_.FramesRendered();
    Schedule(in, EvaluateCue(cue, StateOf(cue), Durations()));
    if (in.voices.empty() && in.tails.empty()) return 0; // nothing to play
    const CueHandle h = in.handle;
    instances_.push_back(std::move(in));
    Update();
    return h;
}

void CuePlayer::Update() {
    const f64 rate = mixer_.SampleRate();
    const u64 horizon = mixer_.FramesRendered() + static_cast<u64>(std::max(lookahead, 0.0f) * rate);
    for (usize k = 0; k < instances_.size();) {
        Instance& in = instances_[k];
        // Endless loops: schedule every repeat that starts before the horizon.
        for (usize t = 0; t < in.tails.size();) {
            bool keep = true;
            for (int guard = 0; guard < 256; ++guard) {
                CueTail& tail = in.tails[t];
                if (in.start_frame + static_cast<u64>(std::llround(tail.offset * rate)) > horizon) break;
                const CueTail now = tail;
                const CuePlan plan = EvaluateCueNode(*in.cue, now.node, StateOf(*in.cue), Durations(), now.offset, now.volume_db, now.pitch);
                Schedule(in, plan); // may add nested tails (and move the vector)
                if (std::isinf(plan.duration) || plan.duration <= 1e-6) {
                    keep = false;
                    break;
                }
                in.tails[t].offset = now.offset + plan.duration;
            }
            if (keep) {
                ++t;
            } else {
                in.tails.erase(in.tails.begin() + static_cast<std::ptrdiff_t>(t));
            }
        }
        std::erase_if(in.voices, [&](const auto& v) { return !mixer_.IsPlaying(v.first); });
        if (in.voices.empty() && in.tails.empty()) {
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
}

bool CuePlayer::Stop(CueHandle h, f32 fade_out) {
    Instance* in = Find(h);
    if (in == nullptr) return false;
    for (const auto& [id, db] : in->voices) mixer_.Stop(id, fade_out);
    in->tails.clear();
    if (fade_out <= 0.0f) instances_.erase(instances_.begin() + (in - instances_.data()));
    return true;
}

void CuePlayer::StopAll() {
    for (const Instance& in : instances_) {
        for (const auto& [id, db] : in.voices) mixer_.Stop(id);
    }
    instances_.clear();
}

bool CuePlayer::IsPlaying(CueHandle h) const {
    const Instance* in = Find(h);
    if (in == nullptr) return false;
    if (!in->tails.empty()) return true;
    return std::any_of(in->voices.begin(), in->voices.end(), [&](const auto& v) { return mixer_.IsPlaying(v.first); });
}

bool CuePlayer::SetPosition(CueHandle h, const Vec3& position, const Vec3& velocity) {
    Instance* in = Find(h);
    if (in == nullptr) return false;
    in->params.position = position;
    in->params.velocity = velocity;
    for (const auto& [id, db] : in->voices) mixer_.SetPosition(id, position, velocity);
    return true;
}

bool CuePlayer::SetVolume(CueHandle h, f32 volume_db) {
    Instance* in = Find(h);
    if (in == nullptr) return false;
    in->params.volume_db = volume_db;
    for (const auto& [id, db] : in->voices) mixer_.SetVolume(id, in->cue->volume_db + volume_db + db);
    return true;
}

std::vector<VoiceId> CuePlayer::Voices(CueHandle h) const {
    std::vector<VoiceId> out;
    if (const Instance* in = Find(h)) {
        for (const auto& [id, db] : in->voices) out.push_back(id);
    }
    return out;
}

} // namespace aether::audio
