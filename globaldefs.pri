# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Ensure symbols are always generated
CONFIG += force_debug_info

# Disable asserts on release builds
CONFIG(release, debug|release) {
    DEFINES += NDEBUG
}

# Enable CFG, EHCont, and CET
*-msvc {
    QMAKE_CFLAGS += -guard:cf -guard:ehcont
    QMAKE_CXXFLAGS += -guard:cf -guard:ehcont
    QMAKE_LFLAGS += -guard:cf -guard:ehcont

    contains(QT_ARCH, x86_64) {
        QMAKE_LFLAGS += -cetcompat
    }
}

# Enable ASan for Linux or macOS
#CONFIG += sanitizer sanitize_address

# Enable ASan for Windows
#QMAKE_CFLAGS += -fsanitize=address
#QMAKE_CXXFLAGS += -fsanitize=address
#QMAKE_LFLAGS += -incremental:no

# Propagate environment variable flags
QMAKE_CFLAGS   += $$(CFLAGS)
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS   += $$(LDFLAGS)

# Keep the Moonlight latency-probe renderer hooks out of upstream plvk.cpp.
# This only activates for the app project on Linux; the header itself is inert
# unless HAVE_LIBPLACEBO_VULKAN is defined.
unix:!macx:exists($$_PRO_FILE_PWD_/streaming/latencyprobe_hooks.h) {
    QMAKE_CXXFLAGS += -include $$_PRO_FILE_PWD_/streaming/latencyprobe_hooks.h
}

# The private Sunshine benchmark control client is app-local and Linux-only,
# just like the libplacebo latency probe that consumes it.
unix:!macx:exists($$_PRO_FILE_PWD_/streaming/latencybenchmarkcontrol.cpp) {
    SOURCES += $$_PRO_FILE_PWD_/streaming/latencybenchmarkcontrol.cpp
    HEADERS += $$_PRO_FILE_PWD_/streaming/latencybenchmarkcontrol.h
}

# Stream-health counters are session-lifetime diagnostics and remain independent
# of whether the automatic latency benchmark is running.
unix:!macx:exists($$_PRO_FILE_PWD_/streaming/streamhealthtelemetry.cpp) {
    SOURCES += $$_PRO_FILE_PWD_/streaming/streamhealthtelemetry.cpp
    HEADERS += $$_PRO_FILE_PWD_/streaming/streamhealthtelemetry.h
}

# Phase 1 stream-pipeline telemetry stays in a downstream-only unit and is
# activated only while the automatic latency benchmark is running.
unix:!macx:exists($$_PRO_FILE_PWD_/streaming/streampipelinetelemetry.cpp) {
    SOURCES += $$_PRO_FILE_PWD_/streaming/streampipelinetelemetry.cpp
    HEADERS += $$_PRO_FILE_PWD_/streaming/streampipelinetelemetry.h
}
