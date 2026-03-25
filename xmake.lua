set_project("im2d")
set_version("0.1.0", {build = "%Y%m%d%H%M"})
set_languages("c++20")
add_rules("mode.debug", "mode.release")

add_requires("libsdl3")
add_requires("glad")
add_requires("imgui", { configs = { sdl3 = true, opengl3 = true } })
add_requires("freetype")
add_requires("catch2")

add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})

target("vendor_spdlog")
    set_kind("headeronly")
    set_default(false)
    add_includedirs("vendor/spdlog-1.17.0/include", {public = true})

target("im2d_logging")
    set_kind("static")
    set_default(false)
    set_warnings("all")
    add_files("src/common/*.cpp")
    add_deps("vendor_spdlog")

target("vendor_lunasvg")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs("vendor/lunasvg/include", "vendor/lunasvg/source", "vendor/lunasvg/plutovg/include", "vendor/lunasvg/plutovg/source", {public = true})
    add_files("vendor/lunasvg/source/*.cpp", "vendor/lunasvg/plutovg/source/*.c")
    add_defines("LUNASVG_BUILD", "LUNASVG_BUILD_STATIC", {public = true})
    add_syslinks("m")

target("vendor_libdxfrw")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs("vendor/libdxfrw/src", "vendor/libdxfrw/src/intern", {public = true})
    add_files("vendor/libdxfrw/src/*.cpp", "vendor/libdxfrw/src/intern/*.cpp")
    add_syslinks("m")

target("vendor_clipper2")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    add_includedirs("vendor/Clipper2/CPP/Clipper2Lib/include", {public = true})
    add_files("vendor/Clipper2/CPP/Clipper2Lib/src/*.cpp")

target("im2d_import")
    set_kind("static")
    set_default(false)
    set_warnings("all")
    add_files("src/import/*.cpp")
    add_includedirs("vendor/nanosvg/src")
    add_deps("im2d_canvas", "im2d_operations", "im2d_logging", "vendor_lunasvg", "vendor_libdxfrw")
    add_packages("imgui", "freetype")

target("im2d_canvas")
    set_kind("static")
    set_warnings("all")
    set_default(false)
    add_files("src/canvas/**.cpp")
    add_deps("im2d_logging", "vendor_clipper2")
    add_packages("imgui")

target("im2d_operations")
    set_kind("static")
    set_default(false)
    set_warnings("all")
    add_files("src/operations/*.cpp")
    add_deps("im2d_canvas", "im2d_logging")
    add_packages("imgui")

target("im2d_export")
    set_kind("static")
    set_default(false)
    set_warnings("all")
    add_files("src/export/*.cpp")
    add_deps("im2d_canvas")
    add_packages("imgui")

target("im2d_nesting_core")
    set_kind("static")
    set_default(false)
    set_warnings("all")
    add_includedirs("src", {public = true})
    add_files("src/nesting/*.cpp")
    add_deps("im2d_canvas", "im2d_logging", "vendor_clipper2")
    add_packages("imgui")

target("nesting_unit_tests")
    set_kind("binary")
    set_default(false)
    set_warnings("all")
    set_rundir("$(projectdir)")
    add_files("tests/nesting/*.cpp")
    add_deps("im2d_nesting_core")
    add_packages("catch2", "imgui")

target("export_regression")
    set_kind("binary")
    set_default(false)
    set_warnings("all")
    set_rundir("$(projectdir)")
    add_files("src/tools/export_regression.cpp")
    add_deps("im2d_canvas", "im2d_import", "im2d_export")
    add_packages("imgui", "freetype")

target("canvas_demo")
    set_kind("binary")
    set_warnings("all")
    set_default(true)
    set_rundir("$(projectdir)")
    add_files("src/demo/demo_app.cpp", "src/demo/main.cpp")
    add_deps("im2d_canvas", "im2d_operations", "im2d_logging")
    add_packages("libsdl3", "glad", "imgui")

target("import_demo")
    set_kind("binary")
    set_warnings("all")
    set_default(true)
    set_rundir("$(projectdir)")
    add_files("src/demo/demo_app.cpp", "src/demo/demo_imported_artwork_windows.cpp", "src/demo/demo_sample_browser.cpp", "src/demo/import_demo.cpp")
    add_deps("im2d_canvas", "im2d_operations", "im2d_import", "im2d_export", "im2d_logging")
    add_packages("libsdl3", "glad", "imgui")

if is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
else
    set_symbols("hidden")
    set_optimize("fastest")
end
--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--

