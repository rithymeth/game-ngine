#pragma once

#include "aether/script/luau_host.h"

// Luau's `Vehicles` table: the vehicles kit's calls, on the active VehicleSystem. Present only when the kit is built (it needs physics). The Vehicle
// component itself needs no script: put one on an entity with a dynamic body and a box collider, then drive it by writing its VehicleInput component
// (entity:Get("VehicleInput").throttle = 1) or with Drive, and read VehicleState (speed, rpm, gear).
//
//   Vehicles.Drive (entity, throttle, steer [, brake [, handbrake]])  -> true if the entity is a Vehicle: throttle -1..1 (negative reverses), steer -1..1
//                                                                         (positive is right), brake and handbrake 0..1; values are clamped. false for an
//                                                                         entity that is not a Vehicle or a value that is not a number
//   Vehicles.Speed (entity)                                           -> m/s along the vehicle's nose (negative reversing); 0 for a vehicle not built yet
//   Vehicles.WheelPosition (entity, index)                            -> the wheel's world position (index 0 to 3: front-left, front-right, rear-left,
//                                                                         rear-right); a zero vector if there is no such wheel
//   Vehicles.WheelSpin (entity, index)                                -> how far that wheel has turned, radians (for animating a wheel mesh)
//   Vehicles.WheelsOnGround (entity)                                  -> how many wheels touch the ground (0 to 4)
//   Vehicles.Flip (entity)                                            -> true if the vehicle was stood back on its wheels where it is
//
// A call with a missing or wrong-typed argument raises a script error; without an active system (no physics) the calls return false (or 0).

namespace aether::script {

void InstallVehiclesApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
