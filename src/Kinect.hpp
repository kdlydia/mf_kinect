#pragma once

#include "MayaFlux/IO/Camera/CameraSource.hpp"

struct _freenect_context;
struct _freenect_device;

namespace MayaFlux::Registry::Service {
class IOService;
}

namespace MayaFlux::IO {

/**
 * @class FreenectCameraSource
 * @brief CameraSource for the first Kinect (Xbox 360 and Kinect for Windows v1), through libfreenect.
 *
 * Reads these CameraConfig fields:
 *   - device_name:  Kinect index, "0" for the first. Empty means "0".
 *   - input_format: "depth" (the default), "registered" or "rgb".
 *   - target_width and target_height: 1280 x 1024 asks for the 10 fps colour
 *     mode. Anything else opens 640 x 480 at 30 fps.
 *
 * "depth" and "registered" fill a DEPTH16 container with millimetres, 0 where
 * the sensor has no reading and at most 10000. The component dimension carries
 * that range, so normalised reads map 10000 mm to 1. "registered" is shifted to
 * line up with the colour camera. "rgb" fills an RGBA8 container.
 *
 * libfreenect delivers frames on its own event thread, started by open(). A
 * frame is written into the container only after the container asks for one.
 *
 * One source reads one stream. libfreenect claims the whole device, so a
 * second source on the same Kinect fails to open.
 */
class FreenectCameraSource : public CameraSource {
public:
    FreenectCameraSource() = default;
    ~FreenectCameraSource() override;

    FreenectCameraSource(const FreenectCameraSource&) = delete;
    FreenectCameraSource& operator=(const FreenectCameraSource&) = delete;
    FreenectCameraSource(FreenectCameraSource&&) = delete;
    FreenectCameraSource& operator=(FreenectCameraSource&&) = delete;

    [[nodiscard]] std::vector<CameraConfig> enumerate_configs() const override;
    [[nodiscard]] std::vector<CameraMode> preset_modes() const override;

    [[nodiscard]] bool open(const CameraConfig& config) override;
    void close() override;
    [[nodiscard]] bool is_open() const override;

    [[nodiscard]] std::shared_ptr<Kakshya::CameraContainer> create_container() const override;
    bool pull_frame(const std::shared_ptr<Kakshya::CameraContainer>& container) override;

    [[nodiscard]] uint32_t width() const override { return m_width; }
    [[nodiscard]] uint32_t height() const override { return m_height; }
    [[nodiscard]] double frame_rate() const override { return m_fps; }

    void set_container(const std::shared_ptr<Kakshya::CameraContainer>& container) override;
    void pull_frame_all() override;

    [[nodiscard]] const std::string& last_error() const override { return m_last_error; }

    void setup_io_service(uint64_t reader_id) override;

private:
    _freenect_context* m_ctx {};
    _freenect_device* m_dev {};
    bool m_depth { true };
    uint32_t m_width {};
    uint32_t m_height {};
    double m_fps {};
    mutable std::string m_last_error;

    std::mutex m_mutex;
    std::vector<uint8_t> m_frame;
    bool m_fresh {};
    std::weak_ptr<Kakshya::CameraContainer> m_container_ref;

    std::thread m_events;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_frame_requested { false };

    std::shared_ptr<Registry::Service::IOService> m_standalone_service;
    uint64_t m_standalone_reader_id {};
    bool m_owns_service {};

    static void on_frame(_freenect_device* dev, void* data, uint32_t timestamp);
    void receive(const void* data);
    bool write_frame(Kakshya::CameraContainer& container);
    void release();
};

/**
 * @brief Open a Kinect and register it with IOManager, as IOManager::open_camera() does for FFmpeg cameras.
 * @return The container, ready for `| Graphics`, or nullptr if the Kinect could not be opened.
 */
std::shared_ptr<Kakshya::CameraContainer> open_kinect(const CameraConfig& config = {});

} // namespace MayaFlux::IO
