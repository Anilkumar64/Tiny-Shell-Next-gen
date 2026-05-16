cmake_minimum_required(VERSION 3.20)

# ─────────────────────────────────────────────────────────────────────────────
# TinyShell NextGen — GUI target
#
# This file is standalone. The parent TinyShell/CMakeLists.txt should add it
# via:  add_subdirectory(gui)
#
# Requirements
#   • Qt 6.2+ (or Qt 5.15 in compatibility mode — see QT_VERSION_MAJOR below)
#   • Find Qt: set CMAKE_PREFIX_PATH or Qt6_DIR / Qt5_DIR in your environment
#
# Optional (recommended for full feature set):
#   Qt6::Charts  — metrics line charts on the Dashboard (gracefully excluded
#                  if not found; the Prometheus text view is always available)
# ─────────────────────────────────────────────────────────────────────────────

# ── Qt version detection ──────────────────────────────────────────────────────
# Prefer Qt6; fall back to Qt5 when Qt6 is absent.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
set(QT_VERSION_MAJOR ${QT_VERSION_MAJOR} CACHE STRING "Qt major version in use" FORCE)
message(STATUS "[gui] Using Qt ${QT_VERSION_MAJOR}")

# ── Required Qt modules ───────────────────────────────────────────────────────
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network          # QNetworkAccessManager → ApiClient
)

# ── Optional Qt modules ───────────────────────────────────────────────────────
find_package(Qt${QT_VERSION_MAJOR} OPTIONAL_COMPONENTS Charts)
if(Qt${QT_VERSION_MAJOR}Charts_FOUND)
    message(STATUS "[gui] Qt Charts found — enabling chart widgets")
    set(TSH_HAS_CHARTS TRUE)
else()
    message(STATUS "[gui] Qt Charts NOT found — chart widgets disabled")
    set(TSH_HAS_CHARTS FALSE)
endif()

# ── Source files ───────────────────────────────────────────────────────────────
# All widgets are header-only (Q_OBJECT macros are processed by AUTOMOC).
# Only files with .cpp counterparts need to be listed explicitly.
set(TSH_GUI_SOURCES
    main_gui.cpp
)

set(TSH_GUI_HEADERS
    TshStyle.h
    ApiClient.h
    MetricCard.h
    DashboardWidget.h
    TerminalWidget.h
    GrpcJobClient.h
    LiveJobWidget.h
    ServerControlPanel.h
    ServerControlPanelPage.h
    ClusterWidget.h
    RbacWidget.h
    TenantWidget.h
    JobWidget.h
    AuditWidget.h
    ConfigWidget.h
    MainWindow.h
)

# Server control panel headers (event-driven architecture)
set(TSH_SERVER_GUI_HEADERS
    ExecutionEventStreamWidget.h
    ClientConnectionPanel.h
    WorkerNodeDashboard.h
    AuditLogPanel.h
    SecurityEventsPanel.h
    JobSchedulerPanel.h
    ExecutionTraceView.h
)

# ── Executable ────────────────────────────────────────────────────────────────
qt_add_executable(tsh_gui
    ${TSH_GUI_SOURCES}
    ${TSH_GUI_HEADERS}       # listed so IDEs show them in the project tree
    ${TSH_SERVER_GUI_HEADERS}
)

# Qt automatic tooling (moc / uic / rcc)
set_target_properties(tsh_gui PROPERTIES
    AUTOMOC  ON
    AUTOUIC  ON
    AUTORCC  ON

    # macOS bundle / Windows app metadata
    MACOSX_BUNDLE                     TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER      "local.tinyshell.gui"
    MACOSX_BUNDLE_BUNDLE_VERSION      "3.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "3.0"
    WIN32_EXECUTABLE                  TRUE   # no console window on Windows
)

# ── Include dirs ──────────────────────────────────────────────────────────────
target_include_directories(tsh_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}           # gui/ headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../config # tsh_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common # shared types (AiContext, Ast, …)
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(tsh_gui PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    tsh_spine_proto
)

if(TSH_HAS_CHARTS)
    target_link_libraries(tsh_gui PRIVATE Qt${QT_VERSION_MAJOR}::Charts)
    target_compile_definitions(tsh_gui PRIVATE TSH_HAS_QTCHARTS)
endif()

# ── C++ standard ─────────────────────────────────────────────────────────────
target_compile_features(tsh_gui PRIVATE cxx_std_20)

# ── Compiler warnings ─────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(tsh_gui PRIVATE /W4 /permissive-)
else()
    target_compile_options(tsh_gui PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter       # Qt signal slots trigger this
    )
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Server Control Panel GUI (production-grade admin control center)
# ─────────────────────────────────────────────────────────────────────────────
qt_add_executable(tsh_server_gui
    main_server_gui.cpp
    ${TSH_GUI_HEADERS}
    ${TSH_SERVER_GUI_HEADERS}
)

set_target_properties(tsh_server_gui PROPERTIES
    AUTOMOC  ON
    AUTOUIC  ON
    AUTORCC  ON
    MACOSX_BUNDLE                     TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER      "local.tinyshell.server-gui"
    MACOSX_BUNDLE_BUNDLE_VERSION      "3.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "3.0"
    WIN32_EXECUTABLE                  TRUE
)

target_include_directories(tsh_server_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../config
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(tsh_server_gui PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    tsh_spine_proto
)

if(TSH_HAS_CHARTS)
    target_link_libraries(tsh_server_gui PRIVATE Qt${QT_VERSION_MAJOR}::Charts)
    target_compile_definitions(tsh_server_gui PRIVATE TSH_HAS_QTCHARTS)
endif()

target_compile_features(tsh_server_gui PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(tsh_server_gui PRIVATE /W4 /permissive-)
else()
    target_compile_options(tsh_server_gui PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ── Install rules ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS tsh_gui tsh_server_gui
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# On Windows, deploy Qt DLLs next to the executable automatically
if(WIN32 AND TARGET Qt${QT_VERSION_MAJOR}::qmake)
    get_target_property(_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin_dir}")
    if(WINDEPLOYQT)
        add_custom_command(TARGET tsh_gui POST_BUILD
            COMMAND "${WINDEPLOYQT}" "$<TARGET_FILE:tsh_gui>"
            COMMENT "Running windeployqt…"
        )
    endif()
endif()
