#include "MayaFlux/MayaFlux.hpp"

#include <gtest/gtest.h>

#include "MayaFlux/Journal/Archivist.hpp"
#include "MayaFlux/Kakshya/Source/CameraContainer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/IOService.hpp"

#include "Kinect.hpp"

#include <libfreenect.h>

// Runs against libfakenect, which plays back a recording through the libfreenect
// API. The recording is written here: a disc in front of a wall, a corner with
// no reading, and a colour ramp.

namespace {

using namespace MayaFlux;
using IO::FreenectCameraSource;

constexpr int W = 640;
constexpr int H = 480;
constexpr int FRAMES = 30;
constexpr uint16_t DISC_RAW = 700;
constexpr uint16_t WALL_RAW = 950;

/** Millimetres for a raw reading, as libfreenect's registration.c computes them with the constants in device.json. */
uint16_t raw_to_mm(uint16_t raw)
{
    const double fixed_ref_x = ((raw - 4.0 * 200.0) / 4.0) - 0.375;
    const double metric = fixed_ref_x * 0.1042;
    return static_cast<uint16_t>(10.0 * ((metric * 120.0 / (7.5 - metric)) + 120.0));
}

bool in_disc(int x, int y, int frame)
{
    const int cx = 160 + frame * 10;
    return (x - cx) * (x - cx) + (y - 240) * (y - 240) < 60 * 60;
}

void write_recording(const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "device.json")
        << R"({"reg_info":{"pad_info":{},"zero_plane_info":{"dcmos_emitter_distance":7.5,)"
        << R"("dcmos_rcmos_distance":2.3,"reference_distance":120,"reference_pixel_size":0.1042}},"const_shift":200})";

    std::ofstream index(dir / "INDEX.txt");
    for (int f = 0; f < FRAMES; ++f) {
        const std::string stamp = std::format("{:.6f}-{}", 1000.0 + f / 30.0, f * 1000);

        std::vector<uint16_t> depth(static_cast<size_t>(W) * H);
        std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                depth[i] = (x < 10 && y < 10) ? FREENECT_DEPTH_RAW_NO_VALUE
                    : in_disc(x, y, f)        ? DISC_RAW
                                              : WALL_RAW;
                rgb[i * 3 + 0] = static_cast<uint8_t>(x % 256);
                rgb[i * 3 + 1] = static_cast<uint8_t>(y % 256);
                rgb[i * 3 + 2] = static_cast<uint8_t>(f * 8);
            }
        }

        const std::string d = "d-" + stamp + ".pgm";
        const std::string r = "r-" + stamp + ".ppm";
        std::ofstream(dir / d, std::ios::binary) << "P5 640 480 65535\n"
                                                 << std::string_view(reinterpret_cast<const char*>(depth.data()), depth.size() * 2);
        std::ofstream(dir / r, std::ios::binary) << "P6 640 480 255\n"
                                                 << std::string_view(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        index << d << "\n"
              << r << "\n";
    }
}

uint16_t depth_at(const Kakshya::CameraContainer& c, int x, int y)
{
    const auto bytes = c.get_frame_pixels(0);
    uint16_t v {};
    std::memcpy(&v, bytes.data() + (static_cast<size_t>(y) * W + x) * 2, 2);
    return v;
}

bool wait_for(const std::function<bool()>& done)
{
    for (int i = 0; i < 200; ++i) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TEST(Kinect, Depth)
{
    FreenectCameraSource source;
    EXPECT_TRUE(source.enumerate_configs().size() == 3) << "one Kinect lists depth, registered and rgb";

    EXPECT_TRUE(source.open({ .input_format = "depth" })) << "depth opens";
    EXPECT_TRUE(source.width() == 640 && source.height() == 480 && source.frame_rate() == 30.0) << "depth is 640 x 480 @ 30";

    auto c = source.create_container();
    EXPECT_TRUE(c && c->get_format() == Kakshya::ImageFormat::DEPTH16) << "depth container is DEPTH16";
    const auto& range = c->get_structure().dimensions.back().value_range;
    EXPECT_TRUE(range && range->min == 0 && range->max == 10000 && range->invalid == 0.0) << "depth range is 0 to 10000 mm, 0 for no reading";

    source.setup_io_service(7);
    c->setup_io(7);
    source.set_container(c);

    EXPECT_TRUE(wait_for([&] { return depth_at(*c, 600, 400) != 0; })) << "first frame arrives after set_container";
    EXPECT_TRUE(depth_at(*c, 600, 400) == raw_to_mm(WALL_RAW)) << std::format("wall reads {} mm", raw_to_mm(WALL_RAW));
    EXPECT_TRUE(depth_at(*c, 5, 5) == 0) << "no reading reads 0";

    int disc = 0;
    for (int x = 0; x < W; ++x)
        disc += depth_at(*c, x, 240) == raw_to_mm(DISC_RAW) ? 1 : 0;
    EXPECT_TRUE(disc >= 115 && disc <= 121) << std::format("disc reads {} mm across 119 pixels (got {})", raw_to_mm(DISC_RAW), disc);

    uint16_t marker = 1;
    std::memcpy(c->mutable_frame_ptr() + (400 * W + 600) * 2, &marker, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(depth_at(*c, 600, 400) == 1) << "no frame is written without a request";

    Registry::BackendRegistry::instance().get_service<Registry::Service::IOService>()->request_frame(7);
    EXPECT_TRUE(wait_for([&] { return depth_at(*c, 600, 400) == raw_to_mm(WALL_RAW); })) << "IOService request_frame brings the next frame";

    source.close();
    EXPECT_TRUE(!source.is_open()) << "close releases the Kinect";
}

TEST(Kinect, Rgb)
{
    FreenectCameraSource source;
    EXPECT_TRUE(source.open({ .input_format = "rgb" })) << "rgb opens";

    auto c = source.create_container();
    EXPECT_TRUE(c && c->get_format() == Kakshya::ImageFormat::RGBA8) << "rgb container is RGBA8";

    EXPECT_TRUE(wait_for([&] { return source.pull_frame(c); })) << "pull_frame writes a frame without set_container";
    const auto px = c->get_frame_pixels(0).subspan((3 * W + 5) * 4, 4);
    EXPECT_TRUE(px[0] == 5 && px[1] == 3 && px[2] % 8 == 0 && px[3] == 255) << "rgb pixel (5, 3) expands to RGBA";
    EXPECT_TRUE(!source.pull_frame(c)) << "pull_frame is false until the next frame";
}

TEST(Kinect, Errors)
{
    FreenectCameraSource source;
    EXPECT_TRUE(!source.open({ .input_format = "ir" })) << "unknown input_format fails";
    EXPECT_TRUE(source.last_error().contains("ir")) << "error names the format";
    EXPECT_TRUE(!source.open({ .device_name = "kinect" })) << "non-numeric device_name fails";
}

/** Writes the recording libfakenect plays and points it there, once per test process. */
class Recording : public ::testing::Environment {
public:
    void SetUp() override
    {
        const auto dir = std::filesystem::temp_directory_path() / "mf_kinect_recording";
        write_recording(dir);
        setenv("FAKENECT_PATH", dir.c_str(), 1);
    }
};

} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    testing::AddGlobalTestEnvironment(new Recording);
    return RUN_ALL_TESTS();
}
