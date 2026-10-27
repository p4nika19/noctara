set_project("noctara")
set_version("0.1.0", {build = "%Y%m%d"})
set_xmakever("3.0.0")

set_languages("c++23")
set_toolchains("clang")
set_policy("build.warning", true)

add_moduledirs("xmake/modules")
includes("xmake/rules/*.lua", "xmake/tasks/*.lua", "xmake/options.lua")

add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.tsan")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

-- xmake otherwise picks up a co-installed Qt 5; Qt 6 is required.
if not get_config("qt") then
    set_config("qt", "/usr/lib/qt6")
end

add_requires("doctest 2.4.x")

-- Toolchain: mold for every link step.
add_ldflags("-fuse-ld=mold", {force = true})
add_shflags("-fuse-ld=mold", {force = true})

add_defines("QT_NO_KEYWORDS", "QT_NO_CAST_FROM_ASCII", "QT_NO_CAST_TO_ASCII",
            "QT_NO_URL_CAST_FROM_STRING", "QT_USE_QSTRINGBUILDER")
add_defines("NOCTARA_VERSION=\"$(version)\"")
add_includedirs("src")

if is_mode("release", "releasedbg") then
    set_optimize("fastest")
    add_cxflags("-march=" .. (get_config("march") or "x86-64-v3"), "-fno-plt", "-fno-semantic-interposition",
                "-fvisibility=hidden", "-fvisibility-inlines-hidden",
                "-fstack-protector-strong", "-fcf-protection=full",
                "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=3", "-fPIE", {force = true})
    if is_mode("release") then
        add_cxflags("-fomit-frame-pointer", {force = true})
        set_symbols("hidden")
    else
        set_symbols("debug")
        add_cxflags("-fno-omit-frame-pointer", {force = true})
    end
    add_ldflags("-pie", "-Wl,--gc-sections", "-Wl,-O2", "-Wl,--as-needed",
                "-Wl,-z,relro,-z,now", {force = true})
    add_cxflags("-ffunction-sections", "-fdata-sections", {force = true})
    if has_config("lto") then
        add_cxflags("-flto=thin", {force = true})
        add_ldflags("-flto=thin", {force = true})
    end
    if get_config("pgo") == "generate" then
        add_cxflags("-fprofile-generate=build/pgo", {force = true})
        add_ldflags("-fprofile-generate=build/pgo", {force = true})
    elseif get_config("pgo") == "use" then
        add_cxflags("-fprofile-use=build/pgo/noctara.profdata", "-Wno-profile-instr-unprofiled", {force = true})
    end
elseif is_mode("debug") then
    set_symbols("debug")
    add_cxflags("-Og", "-g3", "-fno-omit-frame-pointer", {force = true})
    if has_config("sanitizers") then
        add_cxflags("-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", {force = true})
        add_ldflags("-fsanitize=address,undefined", {force = true})
    end
elseif is_mode("tsan") then
    set_symbols("debug")
    add_cxflags("-O1", "-g", "-fno-omit-frame-pointer", {force = true})
end

-- Strict diagnostics applied to project sources only (not moc/rcc output).
-- -Wuseless-cast is GCC-only; clang has no equivalent and rejects the flag.
STRICT_WARNINGS = {
    "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion", "-Wsign-conversion",
    "-Wold-style-cast", "-Wnon-virtual-dtor", "-Woverloaded-virtual", "-Wcast-align",
    "-Wnull-dereference", "-Wdouble-promotion", "-Wformat=2", "-Wimplicit-fallthrough",
    "-Wunused", "-Wmisleading-indentation", "-Wdeprecated", "-Wextra-semi",
    "-Wzero-as-null-pointer-constant", "-Wthread-safety",
}
if has_config("werror") or is_mode("release") then
    table.insert(STRICT_WARNINGS, "-Werror")
end

-- Declares a static library for one module under src/<name>.
-- Only headers that declare Q_OBJECT/Q_GADGET/Q_NAMESPACE are passed to moc.
function noctara_module(name, opts)
    target("noctara_" .. name)
        set_kind("static")
        add_rules("qt.static", "noctara.qtfix")
        add_frameworks(table.unpack(opts.qt or {"QtCore"}))
        add_deps(table.unpack(opts.deps or {}))
        add_packages(table.unpack(opts.packages or {}))
        add_files("src/" .. name .. "/**.cpp", {cxxflags = STRICT_WARNINGS})
        on_load(function (target)
            for _, header in ipairs(os.files(path.join(os.projectdir(), "src", name, "**.hpp"))) do
                local content = io.readfile(header)
                if content:find("Q_OBJECT") or content:find("Q_GADGET") or content:find("Q_NAMESPACE") then
                    target:add("files", header)
                end
            end
        end)
        if opts.extra then opts.extra() end
    target_end()
end

noctara_module("core", {qt = {"QtCore"}})
noctara_module("settings", {qt = {"QtCore", "QtConcurrent"}, deps = {"noctara_core"}})
noctara_module("i18n", {qt = {"QtCore"}, deps = {"noctara_core", "noctara_settings"}})
noctara_module("theme", {qt = {"QtCore", "QtGui"}, deps = {"noctara_core", "noctara_settings"}})
noctara_module("gfx", {qt = {"QtCore", "QtGui", "QtQuick"}, deps = {"noctara_core", "noctara_settings"}})
noctara_module("ui", {qt = {"QtCore", "QtGui", "QtQml", "QtQuick", "QtSvg"},
    deps = {"noctara_core", "noctara_settings", "noctara_i18n", "noctara_theme", "noctara_gfx"}})

target("noctara")
    set_kind("binary")
    add_rules("qt.quickapp", "noctara.qtfix", "noctara.qsb", "noctara.lrelease", "noctara.qrcgen")
    add_frameworks("QtCore", "QtGui", "QtQml", "QtQuick", "QtSvg")
    add_deps("noctara_ui")
    add_files("src/app/*.cpp", {cxxflags = STRICT_WARNINGS})
    add_files("resources/shaders/*.vert", "resources/shaders/*.frag")
    add_files("translations/*.ts")
    set_values("noctara.qrc", {
        {name = "qml",   prefix = "/qml",   root = "src/qml",          patterns = {"**.qml", "**.js"}},
        {name = "fonts", prefix = "/fonts", root = "resources/fonts",  patterns = {"*.ttf", "licenses/*.txt"}},
        {name = "icons", prefix = "/icons", root = "resources/icons",  patterns = {"*.svg"}},
        {name = "data",  prefix = "/data",  root = "resources/data",   patterns = {"*.json", "*.toml"}},
    })
    after_build(function (target)
        -- Keep a stable path for tasks and docs regardless of mode.
        os.cp(target:targetfile(), path.join(os.projectdir(), "build", "noctara"))
    end)

-- Prints the settings schema as Markdown; used by the docs-config task and CI sync check.
target("noctara-schema-doc")
    set_kind("binary")
    set_default(false)
    add_rules("qt.console", "noctara.qtfix")
    add_frameworks("QtCore")
    add_deps("noctara_settings")
    add_files("tools/schema_doc.cpp", {cxxflags = STRICT_WARNINGS})

target("noctara-tests")
    set_kind("binary")
    set_default(false)
    add_rules("qt.quickapp", "noctara.qtfix", "noctara.lrelease")
    add_frameworks("QtCore", "QtGui", "QtQml", "QtQuick", "QtSvg", "QtTest")
    add_deps("noctara_ui")
    add_packages("doctest")
    add_files("tests/**.cpp", {cxxflags = STRICT_WARNINGS})
    add_defines("NOCTARA_SOURCE_DIR=\"$(projectdir)\"")
    add_files("translations/*.ts")
    add_tests("all", {runargs = {}})
