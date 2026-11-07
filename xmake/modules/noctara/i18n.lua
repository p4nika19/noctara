-- Helpers shared by the translation tasks.
import("lib.detect.find_file")

function lupdate()
    return find_file("lupdate", {"/usr/lib/qt6/bin", "/usr/bin"})
end

-- One lupdate scan of the sources, merged into each of `ts_files`.
function lupdate_args(ts_files)
    return table.join({"-locations", "relative", "-no-obsolete", "-extensions", "cpp,hpp,qml,js", "src", "tools", "-ts"},
                      ts_files)
end
