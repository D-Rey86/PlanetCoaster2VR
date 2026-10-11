#include "../../Loader/src/control_settings.h"

#include <objbase.h>


#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

namespace pc2vr::log { void Write(const char*, ...) {} }
using pc2vr::controls::Settings;
void Check(bool ok, const char* reason) { if (!ok) { std::fprintf(stderr, "%s\n", reason); std::exit(1); } }
std::string Xml(const char* sensitivity, const char* invert = "false") {
    return std::string("<Controls_remote><Camera><InvertYOrbit>true</InvertYOrbit></Camera><CameraGamepad>") +
        "<InvertXOrbit>" + invert + "</InvertXOrbit><InvertYOrbit>" + invert + "</InvertYOrbit>" +
        "<InvertXFirstPerson>" + invert + "</InvertXFirstPerson><InvertYFirstPerson>" + invert +
        "</InvertYFirstPerson></CameraGamepad><CameraSensitivity><GamepadRightStickSen>" + sensitivity +
        "</GamepadRightStickSen></CameraSensitivity></Controls_remote>";
}
bool Parse(const std::string& xml, Settings& s, float& value) {
    return pc2vr::controls::ParseXml(xml.data(), xml.size(), s, value);
}
int main() {
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM initialization");
    Settings s; float value = 0;
    Check(Parse(Xml("10.5"), s, value) && s.gain == 1.f && value == 10.5f && !s.invertYOrbit, "default/scoped settings");
    Check(Parse(Xml("5", "true"), s, value) && s.gain == static_cast<float>(0.52938533539755128) &&
          s.invertXOrbit && s.invertYOrbit && s.invertXFirstPerson && s.invertYFirstPerson, "native gain 5 and all inversions");
    Check(Parse(Xml("15"), s, value) && s.gain == static_cast<float>(1.6826926428366416), "native gain 15 rounding");
    Check(Parse(Xml("1"), s, value) && s.gain == 1.f / 3.f, "minimum gain");
    Check(Parse(Xml("20"), s, value) && s.gain == 3.f, "maximum gain");
    for (const char* bad : {"nan", "inf", "0", "21", "5oops", "", "1,5"})
        Check(!Parse(Xml(bad), s, value), "invalid sensitivity accepted");
    Check(!Parse(Xml("5", "yes"), s, value), "invalid boolean accepted");
    std::string full = Xml("15", "true");
    for (size_t n = 0; n < full.size(); ++n) {
        s.gain = 7; value = 42;
        Check(!pc2vr::controls::ParseXml(full.data(), n, s, value) && s.gain == 7 && value == 42, "truncated XML changed snapshot");
    }
    auto duplicate = full; duplicate.insert(duplicate.find("</CameraGamepad>"), "<InvertXOrbit>false</InvertXOrbit>");
    Check(!Parse(duplicate, s, value), "duplicate setting accepted");
    Check(!Parse("<!DOCTYPE Controls_remote [<!ENTITY x 'true'>]>" + full, s, value), "DTD accepted");
    Check(!Parse("<Other/>", s, value), "wrong root accepted");
    std::puts("native gains, invert scope, bounds, duplicates, DTD, and every truncated XML passed");
    CoUninitialize();
}
