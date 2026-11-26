# mf_kinect

A camera source for the first Kinect, read through libfreenect. It opens the Kinect's depth or colour stream as a `CameraContainer`, so it goes to `Graphics` like any other camera in MayaFlux.

Covers the Kinect for Xbox 360 (models 1414 and 1473) and Kinect for Windows v1. The Kinect v2 and Azure Kinect use other libraries and are not covered.

Built and tested against the MayaFlux nightly (1.0.0, main `0f71ff55`). The camera interface it plugs into has been in MayaFlux since 0.5.0, but the module hasn't been built against 0.5.0. It also needs libfreenect:

- Arch: `pacman -S libfreenect`, which also installs the udev rule that lets you open the Kinect without root
- Debian and Ubuntu: `apt install libfreenect-dev`
- macOS: `brew install libfreenect`

## Use

```cpp
#include "Kinect.hpp"

void compose()
{
    auto window = create_window({ .title = "Kinect depth", .width = 640, .height = 480 });
    auto depth = IO::open_kinect() | Graphics;
    auto picture = get_associated_buffer(depth);
    picture->setup_rendering({ .target_window = window, .fragment_shader = "kinect_depth.frag" });
    window->show();
}
```

`open_kinect()` returns nullptr if no Kinect is found, so the lines after it have nothing to work on.

Depth arrives in millimetres, which would look almost black on screen. This shader, saved as `data/shaders/kinect_depth.frag`, shows near things white, far things dark and missing readings black:

```glsl
#version 460

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D texSampler;

void main() {
    float mm = texture(texSampler, fragTexCoord).r * 65535.0;
    float near = mm > 0.0 ? 1.0 - clamp((mm - 500.0) / 4000.0, 0.0, 1.0) : 0.0;
    outColor = vec4(vec3(near), 1.0);
}
```

For colour, open `{ .input_format = "rgb" }` and leave out the shader.

## Streams

| `input_format` | Container | Contents |
|---|---|---|
| `"depth"` (default) | 640 x 480 `DEPTH16`, 30 fps | Millimetres. 0 means no reading, and the maximum is 10000. |
| `"registered"` | 640 x 480 `DEPTH16`, 30 fps | The same, shifted to line up with the colour camera. |
| `"rgb"` | 640 x 480 `RGBA8`, 30 fps | Colour. Ask for 1280 x 1024 (`.target_width`, `.target_height`) to get 10 fps at that size. |

`device_name` is the Kinect's index, `"0"` for the first one. `FreenectCameraSource::enumerate_configs()` lists every Kinect and stream, for a picker.

The depth container's last dimension carries the range 0 to 10000 mm, with 0 marked as no reading, so normalised reads give 0 to 1 over that range.

Drawing the depth container needs a MayaFlux nightly from 24 November 2026 or later. Older builds can't upload a `DEPTH16` texture.

## Not covered

- **Depth and colour together.** libfreenect claims the whole device, so a second source on the same Kinect fails to open. Reading both needs one source with two containers.
- The infrared stream, the tilt motor, the LED, the accelerometer and the microphones.

## Tests

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Run `test/build/test_mf_kinect` to see each check.

No Kinect needs to be connected. The test links against libfakenect, which comes with libfreenect and plays back a recording through the same API. The test writes its own recording first: a disc in front of a wall, a corner with no reading, and a colour ramp. It then checks:

- the millimetre values against libfreenect's conversion formula
- that a frame is written only when the container asks for one
- the RGBA expansion of the colour stream

To run a whole project from a recording instead of a Kinect, preload libfakenect. Arch's `fakenect` wrapper script doesn't swap the library in, because the package installs it as `libfakenect.so`.

```sh
FAKENECT_PATH=/path/to/recording LD_PRELOAD=/usr/lib/fakenect/libfakenect.so ./build/your_project
```

Make a recording from a real Kinect with `fakenect-record /path/to/recording`.

None of this has been run with a real Kinect yet. Two modes are untested: libfakenect can't play back the 1280 x 1024 colour mode, and the test's recording has no registration table, so `"registered"` needs a real Kinect or a real recording.

## Licence

GPLv3.
