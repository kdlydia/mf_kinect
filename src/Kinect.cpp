#include "Kinect.hpp"

#include "MayaFlux/API/Depot.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/Journal/Archivist.hpp"
#include "MayaFlux/Kakshya/Source/CameraContainer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/IOService.hpp"

#include <libfreenect.h>

namespace MayaFlux::IO {

FreenectCameraSource::~FreenectCameraSource()
{
    release();

    if (m_owns_service) {
        Registry::BackendRegistry::instance()
            .unregister_service<Registry::Service::IOService>();
    }
}

std::vector<CameraConfig> FreenectCameraSource::enumerate_configs() const
{
    freenect_context* ctx = nullptr;
    if (freenect_init(&ctx, nullptr) < 0)
        return {};

    freenect_select_subdevices(ctx, FREENECT_DEVICE_CAMERA);
    const int count = freenect_num_devices(ctx);
    freenect_shutdown(ctx);

    std::vector<CameraConfig> configs;
    for (int i = 0; i < count; ++i) {
        const std::string index = std::to_string(i);
        for (const char* stream : { "depth", "registered", "rgb" }) {
            configs.push_back({
                .device_name = index,
                .display_name = "Kinect " + index + " " + stream,
                .input_format = stream,
            });
        }
    }
    return configs;
}

std::vector<CameraMode> FreenectCameraSource::preset_modes() const
{
    return {
        { .label = "640 x 480 @ 30 fps", .width = 640, .height = 480, .fps = 30.0 },
        { .label = "1280 x 1024 @ 10 fps (rgb only)", .width = 1280, .height = 1024, .fps = 10.0 },
    };
}

bool FreenectCameraSource::open(const CameraConfig& config)
{
    close();

    const std::string& stream = config.input_format;
    m_depth = stream.empty() || stream == "depth" || stream == "registered";
    if (!m_depth && stream != "rgb") {
        m_last_error = "unknown input_format '" + stream + "', use depth, registered or rgb";
        return false;
    }

    int index = 0;
    if (!config.device_name.empty()) {
        const auto* end = config.device_name.data() + config.device_name.size();
        if (std::from_chars(config.device_name.data(), end, index).ptr != end) {
            m_last_error = "device_name '" + config.device_name + "' is not a Kinect index";
            return false;
        }
    }

    const auto fail = [this](std::string message) {
        m_last_error = std::move(message);
        release();
        MF_ERROR(Journal::Component::IO, Journal::Context::FileIO,
            "FreenectCameraSource::open: {}", m_last_error);
        return false;
    };

    if (freenect_init(&m_ctx, nullptr) < 0)
        return fail("freenect_init failed");

    freenect_select_subdevices(m_ctx, FREENECT_DEVICE_CAMERA);

    if (freenect_open_device(m_ctx, &m_dev, index) < 0) {
        m_dev = nullptr;
        return fail("no Kinect at index " + std::to_string(index));
    }

    const bool high = !m_depth && config.target_width == 1280 && config.target_height == 1024;
    const freenect_resolution res = high ? FREENECT_RESOLUTION_HIGH : FREENECT_RESOLUTION_MEDIUM;

    const freenect_frame_mode mode = m_depth
        ? freenect_find_depth_mode(res, stream == "registered" ? FREENECT_DEPTH_REGISTERED : FREENECT_DEPTH_MM)
        : freenect_find_video_mode(res, FREENECT_VIDEO_RGB);

    if (!mode.is_valid)
        return fail("libfreenect has no such mode");

    if ((m_depth ? freenect_set_depth_mode(m_dev, mode) : freenect_set_video_mode(m_dev, mode)) < 0)
        return fail("the Kinect refused the mode");

    m_width = static_cast<uint32_t>(mode.width);
    m_height = static_cast<uint32_t>(mode.height);
    m_fps = mode.framerate;
    m_frame.assign(static_cast<size_t>(mode.bytes), 0);
    m_fresh = false;

    freenect_set_user(m_dev, this);
    if (m_depth) {
        freenect_set_depth_callback(m_dev, &FreenectCameraSource::on_frame);
        if (freenect_start_depth(m_dev) < 0)
            return fail("freenect_start_depth failed");
    } else {
        freenect_set_video_callback(m_dev, &FreenectCameraSource::on_frame);
        if (freenect_start_video(m_dev) < 0)
            return fail("freenect_start_video failed");
    }

    m_stop.store(false);
    m_events = std::thread([this] {
        while (!m_stop.load()) {
            timeval timeout { .tv_sec = 0, .tv_usec = 100000 };
            if (freenect_process_events_timeout(m_ctx, &timeout) < 0) {
                MF_ERROR(Journal::Component::IO, Journal::Context::Runtime,
                    "FreenectCameraSource: event loop stopped, the Kinect may be unplugged");
                break;
            }
        }
    });

    MF_INFO(Journal::Component::IO, Journal::Context::FileIO,
        "FreenectCameraSource: opened Kinect {} {}: {}x{} @{:.0f}fps",
        index, stream.empty() ? "depth" : stream, m_width, m_height, m_fps);

    return true;
}

void FreenectCameraSource::release()
{
    m_stop.store(true);
    if (m_events.joinable())
        m_events.join();

    if (m_dev) {
        if (m_depth)
            freenect_stop_depth(m_dev);
        else
            freenect_stop_video(m_dev);
        freenect_close_device(m_dev);
        m_dev = nullptr;
    }

    if (m_ctx) {
        freenect_shutdown(m_ctx);
        m_ctx = nullptr;
    }
}

void FreenectCameraSource::close()
{
    release();
    m_last_error.clear();
}

bool FreenectCameraSource::is_open() const
{
    return m_dev != nullptr;
}

std::shared_ptr<Kakshya::CameraContainer> FreenectCameraSource::create_container() const
{
    if (!is_open()) {
        m_last_error = "Cannot create container: Kinect not open";
        return nullptr;
    }

    auto container = std::make_shared<Kakshya::CameraContainer>(
        m_width, m_height,
        m_depth ? Kakshya::ImageFormat::DEPTH16 : Kakshya::ImageFormat::RGBA8,
        m_fps);

    if (m_depth) {
        container->get_structure().dimensions.back().with_range(
            FREENECT_DEPTH_MM_NO_VALUE, FREENECT_DEPTH_MM_MAX_VALUE, FREENECT_DEPTH_MM_NO_VALUE);
    }

    return container;
}

void FreenectCameraSource::on_frame(freenect_device* dev, void* data, uint32_t /*timestamp*/)
{
    static_cast<FreenectCameraSource*>(freenect_get_user(dev))->receive(data);
}

void FreenectCameraSource::receive(const void* data)
{
    std::lock_guard lock(m_mutex);
    std::memcpy(m_frame.data(), data, m_frame.size());
    m_fresh = true;

    if (m_frame_requested.exchange(false)) {
        if (auto container = m_container_ref.lock())
            write_frame(*container);
    }
}

bool FreenectCameraSource::pull_frame(const std::shared_ptr<Kakshya::CameraContainer>& container)
{
    if (!container)
        return false;

    std::lock_guard lock(m_mutex);
    return write_frame(*container);
}

bool FreenectCameraSource::write_frame(Kakshya::CameraContainer& container)
{
    if (!m_fresh)
        return false;

    uint8_t* dest = container.mutable_frame_ptr();
    if (!dest)
        return false;

    if (m_depth) {
        std::memcpy(dest, m_frame.data(), m_frame.size());
    } else {
        const size_t pixels = m_frame.size() / 3;
        for (size_t i = 0; i < pixels; ++i) {
            dest[i * 4 + 0] = m_frame[i * 3 + 0];
            dest[i * 4 + 1] = m_frame[i * 3 + 1];
            dest[i * 4 + 2] = m_frame[i * 3 + 2];
            dest[i * 4 + 3] = 255;
        }
    }

    m_fresh = false;
    container.mark_ready_for_processing(true);
    return true;
}

void FreenectCameraSource::set_container(const std::shared_ptr<Kakshya::CameraContainer>& container)
{
    std::lock_guard lock(m_mutex);
    m_container_ref = container;
    m_frame_requested.store(true);
}

void FreenectCameraSource::pull_frame_all()
{
    m_frame_requested.store(true);
}

void FreenectCameraSource::setup_io_service(uint64_t reader_id)
{
    m_standalone_reader_id = reader_id;

    if (!Registry::BackendRegistry::instance()
            .get_service<Registry::Service::IOService>()) {

        m_standalone_service = std::make_shared<Registry::Service::IOService>();
        m_standalone_service->request_frame = [this](uint64_t id) {
            if (id == m_standalone_reader_id)
                pull_frame_all();
        };

        Registry::BackendRegistry::instance()
            .register_service<Registry::Service::IOService>(
                [this]() -> void* { return m_standalone_service.get(); });

        m_owns_service = true;
    }
}

std::shared_ptr<Kakshya::CameraContainer> open_kinect(const CameraConfig& config)
{
    auto source = std::make_shared<FreenectCameraSource>();
    if (!source->open(config))
        return nullptr;

    auto container = source->create_container();
    if (!container)
        return nullptr;

    container->create_default_processor();
    source->set_container(container);

    const uint64_t id = get_io_manager()->register_camera_source(source);
    container->setup_io(id);
    container->mark_ready_for_processing(true);

    return container;
}

} // namespace MayaFlux::IO
