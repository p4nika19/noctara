-- GLSL (Vulkan-style, #version 440) -> .qsb, validated at build time and embedded at :/shaders/<file>.qsb.
-- Any qsb diagnostic output fails the build (see tools/qsb-strict.sh).
rule("noctara.qsb")
    add_deps("qt.env")
    set_extensions(".vert", ".frag", ".comp")
    on_config(function (target)
        import("noctara.embed")
        target:data_set("noctara.qsb", embed.find_qt_tool(target, "qsb"))
        target:data_set("noctara.rcc", embed.find_qt_tool(target, "rcc"))
    end)
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("noctara.embed")
        local outdir = path.join(target:autogendir(), "rules", "noctara", "shaders")
        local qsbfile = path.join(outdir, path.filename(sourcefile) .. ".qsb")
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.qsb %s", sourcefile)
        batchcmds:mkdir(outdir)
        -- GLSL 100es/120/150 + 330 for desktop GL, SPIR-V for Vulkan; HLSL/MSL kept for portability.
        batchcmds:vrunv("sh", {path.join(os.projectdir(), "tools", "qsb-strict.sh"), target:data("noctara.qsb"),
            "--glsl", "100 es,120,150,330", "--hlsl", "50", "--msl", "12",
            "-o", path(qsbfile), path(sourcefile)})
        local objectfile = embed.embed_generated(target, batchcmds, qsbfile, "/shaders", path.filename(qsbfile))
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(objectfile))
        batchcmds:set_depcache(target:dependfile(objectfile))
    end)
rule_end()

-- Qt Linguist sources (.ts) -> .qm via lrelease, embedded at :/i18n/<name>.qm.
rule("noctara.lrelease")
    add_deps("qt.env")
    set_extensions(".ts")
    on_config(function (target)
        import("noctara.embed")
        target:data_set("noctara.lrelease", embed.find_qt_tool(target, "lrelease"))
        target:data_set("noctara.rcc", embed.find_qt_tool(target, "rcc"))
    end)
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("noctara.embed")
        local outdir = path.join(target:autogendir(), "rules", "noctara", "i18n")
        local qmfile = path.join(outdir, path.basename(sourcefile) .. ".qm")
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.lrelease %s", sourcefile)
        batchcmds:mkdir(outdir)
        batchcmds:vrunv(target:data("noctara.lrelease"), {"-silent", path(sourcefile), "-qm", path(qmfile)})
        local objectfile = embed.embed_generated(target, batchcmds, qmfile, "/i18n", path.filename(qmfile))
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(objectfile))
        batchcmds:set_depcache(target:dependfile(objectfile))
    end)
rule_end()

-- Generates .qrc files from glob groups declared with set_values("noctara.qrc", {...}).
-- Regenerated on every load; the file is rewritten only when the list changes.
rule("noctara.qrcgen")
    on_load(function (target)
        import("noctara.embed")
        local gendir = path.join(os.projectdir(), "build", ".gens", target:name(), "qrc")
        os.mkdir(gendir)
        for _, group in ipairs(target:values("noctara.qrc") or {}) do
            local root = path.join(os.projectdir(), group.root)
            local files = {}
            for _, pattern in ipairs(group.patterns) do
                for _, file in ipairs(os.files(path.join(root, pattern))) do
                    table.insert(files, file)
                end
            end
            table.sort(files)
            local lines = {"<RCC>", string.format("  <qresource prefix=\"%s\">", group.prefix)}
            for _, file in ipairs(files) do
                local alias = path.relative(file, root):gsub("\\", "/")
                table.insert(lines, string.format("    <file alias=\"%s\">%s</file>",
                    embed.escape_xml(alias), embed.escape_xml(file)))
            end
            table.insert(lines, "  </qresource>")
            table.insert(lines, "</RCC>")
            local qrcfile = path.join(gendir, group.name .. ".qrc")
            embed.write_if_changed(qrcfile, table.concat(lines, "\n") .. "\n")
            target:add("files", qrcfile)
        end
    end)
rule_end()

-- xmake's Qt rule links every static libQt*.a found in the Qt lib dir (a heuristic for static Qt builds).
-- With a shared distro Qt in /usr/lib that drags in stale Qt5 archives and LTO-only bundled libs.
rule("noctara.qtfix")
    before_link(function (target)
        local kept = {}
        for _, link in ipairs(table.wrap(target:get("syslinks"))) do
            if not (link:match("^Qt5") or link:match("^Qt6Bundled")) then
                table.insert(kept, link)
            end
        end
        target:set("syslinks", kept)
    end)
rule_end()
