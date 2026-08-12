includes("lib/commonlibsf")

set_project("OSF Identity")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Structural JSON parsing and string decoding for the manifest/preset readers.
add_requires("glaze v7.0.2")

target("OSF Identity")
    add_rules("commonlibsf.plugin", {
        name = "OSF Identity",
        author = "ozooma10",
        description = "Runtime NPC appearance distribution",
        email = "ozooma10@protonmail.com"
    })

    add_files(
        "src/main.cpp",
        "src/Util/*.cpp",
        "src/Config/*.cpp",
        "src/Runtime/*.cpp"
    )
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")


    add_packages("glaze")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")

    add_installfiles(
        "fixtures/osf-identity/package.schema.json",
        "fixtures/osf-identity/preset-metadata.schema.json",
        { prefixdir = "SFSE/Plugins/OSFIdentity" }
    )

    add_installfiles(
        "fixtures/osf-identity/Packs/(**)",
        { prefixdir = "SFSE/Plugins/OSFIdentity/Packs" }
    )

target("npc-appearance-config-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_files(
        "tools/tests/npc_appearance_config_tests.cpp",
        "src/Config/AssignmentSelection.cpp",
        "src/Config/ConfigDetail.cpp",
        "src/Config/PackDiscovery.cpp",
        "src/Config/RuntimeFormID.cpp"
    )
    add_includedirs("src")

target("npc-appearance-preset-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_files(
        "tools/tests/npc_appearance_preset_tests.cpp",
        "src/Config/Preset.cpp"
    )
    add_includedirs("src")
    add_packages("glaze")
