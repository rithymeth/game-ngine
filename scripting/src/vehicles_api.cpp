#include "aether/script/vehicles_api.h"

#if AETHER_KIT_VEHICLES
#include "aether/vehicles/vehicle_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_VEHICLES

void InstallVehiclesApi(LuauHost& host) {
    using aether::vehicles::VehicleState;
    using aether::vehicles::VehicleSystem;
    using aether::vehicles::WheelInfo;
    host.RegisterNative("Vehicles", "Drive", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1) || !c.IsNumber(2)) {
            c.Fail("Vehicles.Drive(entity, throttle, steer [, brake [, handbrake]]) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        const f32 brake = c.IsNumber(3) ? static_cast<f32>(c.Number(3)) : 0.0f;
        const f32 hand = c.IsNumber(4) ? static_cast<f32>(c.Number(4)) : 0.0f;
        c.Return(system != nullptr && system->Drive(c.EntityArg(0), static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)), brake, hand));
    });
    host.RegisterNative("Vehicles", "Speed", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Vehicles.Speed(entity) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        const VehicleState* state = system != nullptr ? system->StateOf(c.EntityArg(0)) : nullptr;
        c.Return(static_cast<f64>(state != nullptr ? state->speed : 0.0f));
    });
    host.RegisterNative("Vehicles", "WheelsOnGround", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Vehicles.WheelsOnGround(entity) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        const VehicleState* state = system != nullptr ? system->StateOf(c.EntityArg(0)) : nullptr;
        c.Return(static_cast<f64>(state != nullptr ? state->wheels_on_ground : 0u));
    });
    host.RegisterNative("Vehicles", "WheelPosition", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Vehicles.WheelPosition(entity, index) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        WheelInfo info;
        const f64 index = c.Number(1);
        const bool ok = system != nullptr && index >= 0.0 && system->Wheel(c.EntityArg(0), static_cast<u32>(index), info);
        c.ReturnVector(ok ? info.position : Vec3(0, 0, 0));
    });
    host.RegisterNative("Vehicles", "WheelSpin", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Vehicles.WheelSpin(entity, index) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        WheelInfo info;
        const f64 index = c.Number(1);
        const bool ok = system != nullptr && index >= 0.0 && system->Wheel(c.EntityArg(0), static_cast<u32>(index), info);
        c.Return(static_cast<f64>(ok ? info.spin_angle : 0.0f));
    });
    host.RegisterNative("Vehicles", "Flip", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Vehicles.Flip(entity) expected");
            return;
        }
        VehicleSystem* system = VehicleSystem::Active();
        c.Return(system != nullptr && system->Flip(c.EntityArg(0)));
    });
}

#else

void InstallVehiclesApi(LuauHost&) {}

#endif

} // namespace aether::script
