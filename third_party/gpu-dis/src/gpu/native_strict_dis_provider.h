// Distribution note (2026-09-09): project-owned additions and modifications
// are licensed under Apache-2.0. Upstream portions retain their original
// licenses and attribution; see NOTICE, PROVENANCE.md and licenses/.
// OpenCV DIS/reference-derived portions retain Intel (2000-2008), Willow
// Garage (2009) and other upstream copyrights and BSD/Apache terms.
// See licenses/OPENCV_DIS_BSD_HEADER.txt and OPENCV_APACHE_LICENSE.txt.

#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace xess_gpu {
class MotionProvider;
enum class StrictDisPreset { Fast, Medium };
enum class StrictDisInput { RawLuma8, Ffmpeg71RgbGrayLimited };
// Frozen CPU FAST/MEDIUM semantics, no caller-provided temporal initial flow.
// The caller owns producer waits, command submission, completion signaling,
// and source pool leases. NV12 Y and R8_UNORM crop views are supported.
std::unique_ptr<MotionProvider> make_strict_dis_provider(
    const std::string& shader_directory, StrictDisPreset preset=StrictDisPreset::Fast,
    StrictDisInput input=StrictDisInput::RawLuma8);
// Veyra/PushFPS 2026-11-25: the same provider over shaders compiled with `double` defined as `float` (the CPU-rounding
// corrections become float approximations; no FP64 shader operations are required). Not CPU-exact; compared with the
// strict provider by its caller before use.
std::unique_ptr<MotionProvider> make_float_dis_provider(
    const std::string& shader_directory, StrictDisPreset preset=StrictDisPreset::Fast,
    StrictDisInput input=StrictDisInput::RawLuma8);
uint64_t strict_dis_shader_dispatches(const MotionProvider& provider) noexcept;
// Veyra/PushFPS 2026-11-25, tests only: record at most `limit` dispatches per record() (-1 = all) and, with trace, keep
// "<shader>.dxil <x>x<y>" for every dispatch of the last record() (skipped ones included).
void strict_dis_set_test(MotionProvider& provider,int64_t limit,bool trace) noexcept;
std::vector<std::string> strict_dis_test_names(const MotionProvider& provider);
}
