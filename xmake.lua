set_xmakever("3.0.8")
set_project("mcqnet")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build", lsp = "clangd"})

set_languages("c99", "cxx20")

option("benchmarks")
    set_default(false)
    set_showmenu(true)
    set_description("Build mcqnet benchmark targets")
option_end()

target("mcqnet")
    set_kind("headeronly")
    add_headerfiles("mcqnet/include/(mcqnet/**.h)")
    add_includedirs("mcqnet/include", {public = true})

    on_load(function (target)
        if not target:is_plat("linux") then
            return
        end

        local liburing = find_package("liburing", {system = true})
        if not liburing then
            return
        end

        if liburing.includedirs then
            target:add("includedirs", liburing.includedirs, {public = true})
        end
        if liburing.sysincludedirs then
            target:add("sysincludedirs", liburing.sysincludedirs, {public = true})
        end
        if liburing.linkdirs then
            target:add("linkdirs", liburing.linkdirs, {public = true})
        end
        if liburing.links then
            target:add("links", liburing.links, {public = true})
        end
        if liburing.syslinks then
            target:add("syslinks", liburing.syslinks, {public = true})
        end
    end)

local function register_binaries(pattern, group, register_tests)
    for _, sourcefile in ipairs(os.files(pattern)) do
        local name = path.basename(sourcefile)

        target(name)
            set_kind("binary")
            set_group(group)
            set_rundir(os.projectdir())
            add_files(sourcefile)
            add_deps("mcqnet")

            if register_tests then
                add_tests("default")
            end
    end
end

if os.isfile("mcqnet/tooling/mcqnet.cc") then
    target("mcqnet_clangd")
        set_kind("binary")
        set_group("tools")
        add_files("mcqnet/tooling/mcqnet.cc")
        add_deps("mcqnet")
end

register_binaries("mcqnet/test/test_*.cpp", "tests", true)

if has_config("benchmarks") then
    register_binaries("mcqnet/benchmark/*.cpp", "benchmarks", false)
end
