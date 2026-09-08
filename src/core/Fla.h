#pragma once
#include <string>

// fastman92's limit adjuster, which several parts of this plugin have to stand aside from.
//
// It is the standard base layer for large-map San Andreas mods - it is how GTA Underground carries
// five cities - and it owns two things this plugin would otherwise want to own itself: the world
// sector grid and the path system. Where it is doing the job, ours must not.
namespace Fla {
    // Is the module in the process at all. Cheap, and true whether or not any particular patch of
    // its own is switched on, so it is never the whole answer on its own.
    bool Loaded();
    // Its version string, or empty. From its exported GetProjectVersion.
    std::string Version();

    // Has ANYTHING already changed how the game reads its path data - the navi link packing, the
    // CPathNode stride or the CCarPathLink stride? Named for the mod that does it in practice, but
    // the test is behavioural and does not care who is responsible: if the encoding is not the one
    // San Andreas shipped with, nothing here may touch it.
    bool PathEncodingChanged();
    // What the check actually saw, for the log.
    void DescribePathEncoding(std::string& out);
}
