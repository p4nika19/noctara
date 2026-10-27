-- Project tasks. Built-in `xmake build`, `xmake run`, `xmake test` cover build/run/test.

task("i18n-update")
    set_category("plugin")
    set_menu {usage = "xmake i18n-update", description = "Extract translatable strings into translations/*.ts (lupdate)"}
    on_run(function ()
        import("lib.detect.find_file")
        local lupdate = find_file("lupdate", {"/usr/lib/qt6/bin", "/usr/bin"})
        local sources = {"src", "tools"}
        for _, ts in ipairs(os.files(path.join(os.projectdir(), "translations", "*.ts"))) do
            os.execv(lupdate, table.join({"-locations", "relative", "-no-obsolete", "-extensions", "cpp,hpp,qml,js"},
                sources, {"-ts", ts}), {curdir = os.projectdir()})
        end
    end)
task_end()

task("i18n-release")
    set_category("plugin")
    set_menu {usage = "xmake i18n-release", description = "Compile translations/*.ts into build/i18n/*.qm (lrelease)"}
    on_run(function ()
        import("lib.detect.find_file")
        local lrelease = find_file("lrelease", {"/usr/lib/qt6/bin", "/usr/bin"})
        local outdir = path.join(os.projectdir(), "build", "i18n")
        os.mkdir(outdir)
        for _, ts in ipairs(os.files(path.join(os.projectdir(), "translations", "*.ts"))) do
            os.execv(lrelease, {ts, "-qm", path.join(outdir, path.basename(ts) .. ".qm")})
        end
    end)
task_end()

-- CI gate: fails if sources contain strings missing from the .ts files, or if any
-- non-English translation is unfinished/obsolete/vanished.
task("i18n-check")
    set_category("plugin")
    set_menu {usage = "xmake i18n-check", description = "Verify translations are complete and in sync with sources"}
    on_run(function ()
        import("lib.detect.find_file")
        local lupdate = find_file("lupdate", {"/usr/lib/qt6/bin", "/usr/bin"})
        local tmpdir = os.tmpfile() .. ".i18n"
        os.mkdir(tmpdir)
        local failed = false
        for _, ts in ipairs(os.files(path.join(os.projectdir(), "translations", "*.ts"))) do
            local copy = path.join(tmpdir, path.filename(ts))
            os.cp(ts, copy)
            os.iorunv(lupdate, {"-locations", "relative", "-no-obsolete", "-extensions", "cpp,hpp,qml,js",
                "src", "tools", "-ts", copy}, {curdir = os.projectdir()})
            local fresh = io.readfile(copy)
            local original = io.readfile(ts)
            local function strip(s) return (s:gsub("<location[^>]*/>%s*", "")) end
            if strip(fresh) ~= strip(original) then
                print("out of date: %s (run `xmake i18n-update`)", ts)
                failed = true
            end
            local function count(text, needle)
                local n = 0
                for _ in text:gmatch(needle) do n = n + 1 end
                return n
            end
            if not path.basename(ts):endswith("_en") then
                local unfinished = count(original, "type=\"unfinished\"")
                local obsolete = count(original, "type=\"obsolete\"") + count(original, "type=\"vanished\"")
                if unfinished > 0 or obsolete > 0 then
                    print("%s: %d unfinished, %d obsolete/vanished", ts, unfinished, obsolete)
                    failed = true
                end
            end
        end
        os.tryrm(tmpdir)
        if failed then raise("i18n check failed") end
        print("i18n check passed")
    end)
task_end()

task("docs-config")
    set_category("plugin")
    set_menu {usage = "xmake docs-config [--check]", description = "Generate docs/configuration.md from the settings schema",
              options = {{nil, "check", "k", nil, "Fail if docs/configuration.md is out of date instead of writing it"}}}
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        import("core.project.project")
        config.load()
        os.execv("xmake", {"build", "noctara-schema-doc"})
        local tool = project.target("noctara-schema-doc"):targetfile()
        local generated = os.iorunv(tool, {"--markdown"})
        local docfile = path.join(os.projectdir(), "docs", "configuration.md")
        if option.get("check") then
            if not os.isfile(docfile) or io.readfile(docfile) ~= generated then
                raise("docs/configuration.md is out of date; run `xmake docs-config`")
            end
            print("docs/configuration.md is in sync with the schema")
        else
            io.writefile(docfile, generated)
            print("wrote %s", docfile)
        end
    end)
task_end()

task("format")
    set_category("plugin")
    set_menu {usage = "xmake format [--check]", description = "Run clang-format over all sources",
              options = {{nil, "check", "k", nil, "Only check formatting (CI)"}}}
    on_run(function ()
        import("core.base.option")
        local files = {}
        for _, pattern in ipairs({"src/**.cpp", "src/**.hpp", "tests/**.cpp", "tests/**.hpp", "tools/**.cpp", "bench/**.cpp"}) do
            table.join2(files, os.files(path.join(os.projectdir(), pattern)))
        end
        local args = option.get("check") and {"--dry-run", "--Werror"} or {"-i"}
        os.execv("clang-format", table.join(args, files))
    end)
task_end()

task("lint")
    set_category("plugin")
    set_menu {usage = "xmake lint", description = "Run clang-tidy (curated checks in .clang-tidy) over project sources"}
    on_run(function ()
        local files = {}
        for _, pattern in ipairs({"src/**.cpp", "tools/**.cpp"}) do
            table.join2(files, os.files(path.join(os.projectdir(), pattern)))
        end
        assert(os.isfile(path.join(os.projectdir(), "compile_commands.json")), "build once to generate compile_commands.json")
        local runner = "run-clang-tidy"
        if os.isexec("/usr/bin/run-clang-tidy") then
            os.execv(runner, table.join({"-quiet", "-p", os.projectdir()}, files))
        else
            os.execv("clang-tidy", table.join({"-p", os.projectdir(), "--quiet"}, files))
        end
    end)
task_end()

task("pgo")
    set_category("plugin")
    set_menu {usage = "xmake pgo", description = "PGO: instrumented build, scripted scenario, merge, optimised rebuild"}
    on_run(function ()
        local pgo = path.join(os.projectdir(), "build", "pgo")
        os.tryrm(pgo)
        os.execv("xmake", {"f", "-m", "release", "--pgo=generate"})
        os.execv("xmake", {"build", "noctara"})
        os.execv(path.join(os.projectdir(), "build", "noctara"), {"--pgo-scenario"},
                 {envs = {QT_QPA_PLATFORM = "offscreen", LLVM_PROFILE_FILE = path.join(pgo, "%p.profraw")}})
        os.execv("llvm-profdata", table.join({"merge", "-o", path.join(pgo, "noctara.profdata")},
                 os.files(path.join(pgo, "*.profraw"))))
        os.execv("xmake", {"f", "-m", "release", "--pgo=use"})
        os.execv("xmake", {"build", "noctara"})
    end)
task_end()

task("package")
    set_category("plugin")
    set_menu {usage = "xmake package", description = "Build a release tarball with desktop file and icons"}
    on_run(function ()
        os.execv("sh", {path.join(os.projectdir(), "tools", "package.sh")})
    end)
task_end()

-- Launches the app offscreen in every renderer/theme/language combination, plus every blur style on the GPU
-- path, and visits all pages; fails if any combination logs a warning (QML errors, missing types, binding
-- loops, shader errors, ...). The offscreen platform does not draw shader effects, so shader *rendering* is only
-- exercised with a real display: NOCTARA_SMOKE_PLATFORM=wayland (or xcb) xmake smoke.
task("smoke")
    set_category("plugin")
    set_menu {usage = "xmake smoke", description = "Headless smoke test of all pages (software + GPU, both themes and languages, all glass styles)"}
    on_run(function ()
        local binary = path.join(os.projectdir(), "build", "noctara")
        assert(os.isfile(binary), "build the app first")
        local scratch = os.tmpfile() .. ".smoke"
        local platform = os.getenv("NOCTARA_SMOKE_PLATFORM") or "offscreen"
        local runs = {}
        for _, gpu in ipairs({"off", "auto"}) do
            for _, theme in ipairs({"dark", "light"}) do
                for _, lang in ipairs({"en", "ru"}) do
                    table.insert(runs, {gpu = gpu, theme = theme, lang = lang, style = "kawase"})
                end
            end
        end
        for _, style in ipairs({"gaussian", "liquid"}) do
            for _, theme in ipairs({"dark", "light"}) do
                table.insert(runs, {gpu = "auto", theme = theme, lang = "en", style = style})
            end
        end
        local failures = 0
        for _, run in ipairs(runs) do
            local ok = try { function ()
                os.vrunv(binary, {"--smoke-test", "--gpu=" .. run.gpu, "--theme=" .. run.theme, "--lang=" .. run.lang,
                                  "--set", "appearance.glass.style=" .. run.style},
                         {envs = {QT_QPA_PLATFORM = platform, XDG_CONFIG_HOME = scratch .. "/config",
                                  XDG_CACHE_HOME = scratch .. "/cache", XDG_STATE_HOME = scratch .. "/state"}})
                return true
            end }
            print("%s gpu=%s theme=%s lang=%s glass=%s", ok and "ok  " or "FAIL", run.gpu, run.theme, run.lang, run.style)
            if not ok then failures = failures + 1 end
        end
        os.tryrm(scratch)
        if failures > 0 then raise("%d smoke combinations failed", failures) end
    end)
task_end()

-- ThreadSanitizer run (configure with `xmake f -m tsan` first). The distro Qt is not TSan-instrumented:
-- Qt-internal synchronisation is suppressed (tools/tsan.supp), and suites that create QThreadPool
-- threads are excluded because TSan's thread registry aborts on pthread_t reuse there
-- (CHECK live_.try_emplace). Full coverage needs a TSan-instrumented Qt build (docs/performance.md).
task("test-tsan")
    set_category("plugin")
    set_menu {usage = "xmake test-tsan", description = "Run unit tests under ThreadSanitizer (tsan mode)"}
    on_run(function ()
        local binary = path.join(os.projectdir(), "build", "linux", "x86_64", "tsan", "noctara-tests")
        assert(os.isfile(binary), "configure with `xmake f -m tsan` and build noctara-tests first")
        os.execv(binary, {"-tse=settings.file,ui.settings"}, {envs = {
            TSAN_OPTIONS = "halt_on_error=1 suppressions=" .. path.join(os.projectdir(), "tools", "tsan.supp")}})
    end)
task_end()
