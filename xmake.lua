includes("lib/commonlibsf")

set_project("OSF Identity")
set_version("0.1.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("OSF Identity")
    add_rules("commonlibsf.plugin", {
        name = "osf-identity",
        author = "ozooma10",
        description = "Safe package-driven runtime NPC appearance distribution",
        email = "ozooma10@protonmail.com"
    })

    add_files(
        "src/main.cpp",
        "src/NpcAppearance/Config.cpp",
        "src/NpcAppearance/Json.cpp",
        "src/NpcAppearance/Preset.cpp",
        "src/NpcAppearance/Resolver.cpp",
        "src/NpcAppearance/Runtime.cpp",
        "src/Util/NativeMainThreadQueue.cpp"
    )
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")

    after_build(function(target)
        local modsroot = os.getenv("XSE_SF_MODS_PATH")
        local gameroot = os.getenv("XSE_SF_GAME_PATH")
        local plugindir = nil

        if modsroot and #modsroot > 0 then
            plugindir = path.join(modsroot, target:name(), "SFSE", "Plugins")
        elseif gameroot and #gameroot > 0 then
            plugindir = path.join(gameroot, "Data", "SFSE", "Plugins")
        else
            cprint("${yellow}[osf-identity] no deploy root configured; skipping post-build copy")
            return
        end

        os.mkdir(plugindir)
        local outputs = {
            { src = target:targetfile(), label = "plugin" },
            { src = target:symbolfile(), label = "symbols" }
        }
        for _, entry in ipairs(outputs) do
            if entry.src and os.isfile(entry.src) then
                local dst = path.join(plugindir, path.filename(entry.src))
                os.trycp(entry.src, dst)
                cprint("${green}[osf-identity] copied %s:${clear} %s", entry.label, dst)
            end
        end

        local configdir = path.join(plugindir, "OSFIdentity")
        os.mkdir(configdir)
        for _, schema in ipairs({ "package.schema.json", "preset-metadata.schema.json" }) do
            os.trycp(
                path.join(os.projectdir(), "fixtures", "osf-identity", schema),
                path.join(configdir, schema)
            )
        end
    end)

target("npc-appearance-config-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_files(
        "tools/tests/npc_appearance_config_tests.cpp",
        "src/NpcAppearance/Config.cpp",
        "src/NpcAppearance/Json.cpp"
    )
    add_includedirs("src")

target("npc-appearance-preset-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_files(
        "tools/tests/npc_appearance_preset_tests.cpp",
        "src/NpcAppearance/Preset.cpp",
        "src/NpcAppearance/Json.cpp"
    )
    add_includedirs("src")
