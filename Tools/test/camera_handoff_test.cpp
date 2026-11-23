// Preserve the transition checks used for the accepted Guest-camera fade.
#include "../../Loader/src/camera_handoff.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool ok) {
    if (!ok) { std::fputs("FAIL camera handoff transition\n",stderr);std::exit(1); }
}
}
int main() {
    using pc2vr::gamestate::Ride;
    pc2vr::xr::CameraHandoff fade;
    auto step = [&](uint64_t t, bool locked, Ride mode, bool park, bool guest, bool hold = true) {
        return fade.Update(t, true, false, locked, mode, park, guest, hold);
    };
    Check(step(100, false, Ride::None, true, false).fade == 0);
    Check(step(200, true, Ride::None, false, false).hold);
    auto middle = step(245, true, Ride::None, false, false);
    Check(middle.hold && middle.fade == .5f);
    Check(step(290, true, Ride::Guest, false, false).fade == 1);
    Check(step(1000, true, Ride::Guest, false, false).fade == 1);
    Check(step(1400, true, Ride::Guest, false, true).fade == 1);
    Check(step(1495, true, Ride::Guest, false, true).fade == .5f);
    Check(step(1590, true, Ride::Guest, false, true).fade == 0);
    Check(step(1650, true, Ride::Guest, false, false).fade == 0);
    Check(step(1700, false, Ride::None, false, false, false).fade == 1);
    Check(step(4250, false, Ride::None, false, false).fade == 1);
    Check(step(4300, false, Ride::None, false, false).fade == 1);
    Check(step(4490, false, Ride::None, false, false).fade == 0);
    Check(step(4500, false, Ride::None, true, false).fade == 0);
    Check(step(4600, true, Ride::Cinematic, false, false).fade == 0);
    Check(fade.Update(4610, true, true, false, Ride::None, false, false, true).fade == 0);
    std::puts("PASS camera handoff timing, timeout, tracking gaps and pause reset");
}
