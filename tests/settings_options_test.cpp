#include "../src/common.h"
#include <fstream>
#include <iterator>
#include <cstdio>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL: %s\n", name); }
}
int wmain() {
    const auto dir = AppDataDir();
    // This executable is built in its own fixture directory. Never touch installed data.
    if (dir.find(L"\\tests\\settings-options\\data") == std::wstring::npos) return 2;
    const auto path = dir + L"\\settings.ini";
    const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::ifstream originalFile(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(originalFile)), {});
    if (existed && !originalFile) return 2;
    originalFile.close();
    struct Restore {
        std::wstring path; bool existed; std::string original;
        ~Restore() {
            if (existed) { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << original; }
            else DeleteFileW(path.c_str());
        }
    } restore{path, existed, original};
    auto write = [&](const std::string& text) { std::ofstream f(path, std::ios::trunc); f << text; };
    const std::string legacy = "edge=1\niconSize=54\nmagnification=2.1\nglassStyle=2\nshowDelay=0.15\nhideDelay=0.7\nautostart=1\ncpuTemp=0\npinsVersion=2\n";
    write(legacy);
    Settings s; s.Load();
    Check(s.showDesktopButton && s.clockSize == 0 && s.iconSpacing == 1, "legacy settings get new defaults");
    auto legacyUnchanged = [](const Settings& v) {
        return v.edge == Edge::Left && v.iconSize == 54 && v.magnification == 2.1f && v.glassStyle == 2 &&
               v.showDelay == 0.15f && v.hideDelay == 0.7f && v.autostart && !v.cpuTemp && v.pinsVersion == 2;
    };
    Check(legacyUnchanged(s), "legacy preferences survive load");
    for (bool desktop : {false, true}) for (int clock = 0; clock < 2; ++clock) for (int spacing = 0; spacing < 3; ++spacing) {
        s.showDesktopButton = desktop; s.clockSize = clock; s.iconSpacing = spacing; s.Save();
        Settings loaded; loaded.Load();
        Check(loaded.showDesktopButton == desktop && loaded.clockSize == clock && loaded.iconSpacing == spacing,
              "all new option combinations survive save and fresh load");
        Check(legacyUnchanged(loaded), "saving new options preserves legacy preferences");
    }
    write(legacy + "showDesktopButton=invalid\nclockSize=invalid\niconSpacing=invalid\n");
    Settings invalid; invalid.Load();
    Check(invalid.showDesktopButton && invalid.clockSize == 0 && invalid.iconSpacing == 1, "malformed new values use defaults");
    Check(legacyUnchanged(invalid), "malformed new values do not discard legacy preferences");
    write(legacy + "clockSize=99\niconSpacing=-5\n");
    Settings bounded; bounded.Load();
    Check(bounded.clockSize == 1 && bounded.iconSpacing == 0, "numeric option values are bounded");
    write(legacy + "dockThickness=999\nedgeGap=-3\nedgeOffset=-250\n");
    Settings shaped; shaped.Load();
    Check(shaped.dockThickness == 150 && shaped.edgeGap == 0 && shaped.edgeOffset == -100, "shape values are bounded");
    Settings fresh; fresh.Load();
    shaped.dockThickness = 120; shaped.edgeGap = 18; shaped.edgeOffset = 35; shaped.Save();
    Settings reread; reread.Load();
    Check(reread.dockThickness == 120 && reread.edgeGap == 18 && reread.edgeOffset == 35 && legacyUnchanged(reread), "shape values survive save and load");
    write(legacy);
    Settings legacyShape; legacyShape.Load();
    Check(legacyShape.dockThickness == 100 && legacyShape.edgeGap == 6 && legacyShape.edgeOffset == 0, "older files get the default shape");
    write(legacy);
    Settings hidden; hidden.Load();
    Check(hidden.autoHide, "older files keep auto-hide on");
    hidden.autoHide = false; hidden.Save();
    Settings shown; shown.Load();
    Check(!shown.autoHide && legacyUnchanged(shown), "auto-hide off survives save and load");
    write(legacy + "iconGap=99\n");
    Settings gapped; gapped.Load();
    Check(gapped.iconGap == 60 && gapped.iconSpacing == 1, "icon gap is bounded and leaves the preset alone");
    write(legacy + "magnification=2.5\n");
    Settings magnified; magnified.Load();
    Check(magnified.magnification == 2.1f, "magnification above the menu maximum is clamped");
    write(legacy + "clockSize=1\n");
    Settings partial; partial.Load();
    Check(partial.clockSize == 1 && partial.showDesktopButton && partial.iconSpacing == 1, "partially upgraded files retain defaults for missing keys");
    printf("RESULT settings checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
