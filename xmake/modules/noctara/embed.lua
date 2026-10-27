-- Helpers shared by the resource-producing rules.
import("lib.detect.find_file")

function find_qt_tool(target, name)
    local qt = assert(target:data("qt"), "Qt not found!")
    local dirs = {}
    for _, dir in ipairs({qt.bindir_host, qt.bindir, qt.libexecdir_host, qt.libexecdir}) do
        if dir then table.insert(dirs, dir) end
    end
    local tool = find_file(name, dirs)
    assert(tool and os.isexec(tool), name .. " not found in the Qt installation")
    return tool
end

-- Writes `content` to `file` only if it differs, so unchanged inputs do not trigger rebuilds.
function write_if_changed(file, content)
    if os.isfile(file) and io.readfile(file) == content then
        return false
    end
    io.writefile(file, content)
    return true
end

function escape_xml(s)
    return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub("\"", "&quot;"))
end

-- Emits batch commands that compile `generated` into the target under `:/<prefix>/<alias>`.
function embed_generated(target, batchcmds, generated, prefix, alias)
    local gendir = path.join(target:autogendir(), "rules", "noctara", "embed")
    local id = path.basename(alias):gsub("[^%w]", "_") .. "_" .. hash.strhash32(generated)
    local qrcfile = path.join(gendir, id .. ".qrc")
    local cppfile = path.join(gendir, id .. ".cpp")
    os.mkdir(gendir)
    write_if_changed(qrcfile, string.format(
        "<RCC><qresource prefix=\"%s\"><file alias=\"%s\">%s</file></qresource></RCC>\n",
        escape_xml(prefix), escape_xml(alias), escape_xml(path.absolute(generated))))

    local objectfile = target:objectfile(cppfile)
    table.insert(target:objectfiles(), objectfile)
    batchcmds:vrunv(target:data("noctara.rcc"), {"-name", id, path(qrcfile), "-o", path(cppfile)})
    batchcmds:compile(cppfile, objectfile)
    return objectfile
end

-- Project headers (quoted includes resolvable under `root`) reachable from `file`, transitively.
function project_includes(file, root, seen, out)
    seen = seen or {}
    out = out or {}
    local content = io.readfile(file) or ""
    for include in content:gmatch('#%s*include%s+"([^"]+)"') do
        local candidate = path.join(root, include)
        if not os.isfile(candidate) then
            candidate = path.join(path.directory(file), include)
        end
        if os.isfile(candidate) and not seen[candidate] then
            seen[candidate] = true
            table.insert(out, candidate)
            project_includes(candidate, root, seen, out)
        end
    end
    return out
end
