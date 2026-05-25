# ydev — cross-platform device abstraction (camera/audio/sensors/location).
#
# Builds a small static library `libydev.a`:
#   - core (this dir): vfd ring + self-pipe primitive, perm cache, strerror,
#                      NV12->I420 helper. Platform-agnostic.
#   - backend (../../platform/{apple,android,linux}): per-host capture and
#                      permission glue. The right backend is picked by
#                      host_os; only that one is compiled.
#
# Public headers live in include/yos/ydev/. Anyone (including the camera
# server) can link libydev without pulling in wasm3 or the yos runtime.

# Apple backend sources are Objective-C (.m). meson lazily enables the
# language only when a target has .m files — we ask for it here on darwin.
if host_os == 'darwin'
  add_languages('objc', native: false)
endif

ydev_core_sources = files(
  'core.c',
  'perm.c',
  'error.c',
  'convert.c',
)

# Per-host backend source list. Apple sources are .m (Objective-C); meson
# compiles them with the ObjC frontend automatically. ARC is enabled via
# -fobjc-arc; the .m bodies still use CFRetain/CFRelease for the CF types
# we actually own (CVPixelBufferRef).
ydev_backend_sources = []
ydev_backend_args    = []
ydev_backend_deps    = []
ydev_backend_link    = []

if host_os == 'darwin'
  ydev_backend_sources += files(
    '../../platform/apple/camera_av.m',
    '../../platform/apple/perm_apple.m',
    '../../platform/apple/audio_apple.m',
    '../../platform/apple/sensor_apple.m',
    '../../platform/apple/location_apple.m',
  )
  ydev_backend_args += ['-fobjc-arc']
  # CoreLocation is Foundation-adjacent on iOS but a separate framework
  # everywhere. CoreMotion is iOS-only; perm_apple.m gates it with
  # TARGET_OS_IOS so we still link the framework only where it exists.
  ydev_backend_link += [
    '-framework', 'Foundation',
    '-framework', 'AVFoundation',
    '-framework', 'CoreMedia',
    '-framework', 'CoreVideo',
    '-framework', 'CoreFoundation',
  ]
  if not is_ios_app and not is_tvos_app
    ydev_backend_link += ['-framework', 'CoreLocation']
  else
    ydev_backend_link += ['-framework', 'CoreLocation']
  endif
  if is_ios_app
    ydev_backend_link += ['-framework', 'CoreMotion']
  endif
elif host_os == 'linux'
  ydev_backend_sources += files(
    '../../platform/linux/camera_linux.c',
    '../../platform/linux/audio_linux.c',
    '../../platform/linux/sensor_linux.c',
    '../../platform/linux/location_linux.c',
    '../../platform/linux/perm_linux.c',
  )
  alsa_dep = dependency('alsa', required: true)
  ydev_backend_deps += [alsa_dep]
elif host_os == 'android'
  ydev_backend_sources += files(
    '../../platform/android/camera_android.c',
    '../../platform/android/audio_android.c',
    '../../platform/android/sensor_android.c',
    '../../platform/android/location_android.c',
    '../../platform/android/perm_android.c',
  )
  # NDK frameworks: link the libs the backends call into.
  ydev_backend_link += [
    '-lcamera2ndk',  # ACameraManager + ACameraDevice + AImageReader callers
    '-lmediandk',    # AImageReader, AImage
    '-laaudio',      # AAudio
    '-landroid',     # ASensorManager + ALooper
    '-llog',         # __android_log_print
  ]
else
  error('ydev: unsupported host OS: ' + host_os)
endif

ydev_inc = include_directories(
  '../../..',          # so '#include <yos/ydev/...>' resolves via ../include
  '../../../../include',
  '.',
)

ydev_lib = static_library(
  'ydev',
  ydev_core_sources,
  ydev_backend_sources,
  include_directories : ydev_inc,
  dependencies        : ydev_backend_deps,
  c_args   : ['-D_GNU_SOURCE'] + ydev_backend_args,
  objc_args: ['-D_GNU_SOURCE'] + ydev_backend_args,
  install  : false,
)

ydev_dep = declare_dependency(
  link_with           : ydev_lib,
  include_directories : ydev_inc,
  dependencies        : ydev_backend_deps,
  link_args           : ydev_backend_link,
)
