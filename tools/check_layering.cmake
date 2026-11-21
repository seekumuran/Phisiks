# Layering and brand check (cmake -P, strict: any violation fails the build).
#
# 1. Public engine headers (engine/include) include only the public API ("Phisiks/..."), glm,
#    EnTT and the C++ standard library.
# 2. Apps (apps/) and the runtime test harness (tests/runtime) follow the same rule, plus their own
#    local headers ("Name.h").
# 3. Unit tests (tests/unit) may use engine-private headers (white-box), but no backend library
#    headers (OpenGL loader, GLFW, PhysX).
# 4. No project text file references the names of the code base Phisiks was originally derived
#    from. The patterns are written as character classes so this file does not match itself.

set(violations 0)

function(phisiks_report rel line)
    message(STATUS "layering: ${rel}: ${line}")
endfunction()

# Include rules -----------------------------------------------------------------------------------
function(phisiks_check_includes dirs allow_local out_count)
    set(count 0)
    foreach(dir ${dirs})
        file(GLOB_RECURSE files "${PHISIKS_ROOT}/${dir}/*.h" "${PHISIKS_ROOT}/${dir}/*.hpp" "${PHISIKS_ROOT}/${dir}/*.cpp")
        foreach(f ${files})
            file(RELATIVE_PATH rel "${PHISIKS_ROOT}" "${f}")
            file(STRINGS "${f}" lines REGEX "^[ \t]*#[ \t]*include")
            foreach(line ${lines})
                string(STRIP "${line}" line)
                set(ok FALSE)
                if(line MATCHES "#[ \t]*include[ \t]*\"Phisiks/")
                    set(ok TRUE)                                   # public API
                elseif(allow_local AND line MATCHES "#[ \t]*include[ \t]*\"[A-Za-z0-9_]+\\.h\"")
                    set(ok TRUE)                                   # the app's own header
                elseif(line MATCHES "#[ \t]*include[ \t]*<glm/")
                    set(ok TRUE)
                elseif(line MATCHES "#[ \t]*include[ \t]*<entt\\.hpp>")
                    set(ok TRUE)
                elseif(allow_local AND line MATCHES "#[ \t]*include[ \t]*<(imgui|ImGuizmo)\\.h>")
                    set(ok TRUE)                                   # studio UI (Dear ImGui / ImGuizmo, linked with the engine)
                elseif(line MATCHES "#[ \t]*include[ \t]*<[a-z_0-9]+>")
                    set(ok TRUE)                                   # C++ standard library
                endif()
                if(NOT ok)
                    phisiks_report("${rel}" "${line}")
                    math(EXPR count "${count} + 1")
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(${out_count} ${count} PARENT_SCOPE)
endfunction()

phisiks_check_includes("engine/include" FALSE public_violations)
phisiks_check_includes("apps;examples;tests/runtime" TRUE app_violations)

set(unit_violations 0)
file(GLOB unit_files "${PHISIKS_ROOT}/tests/unit/*.cpp" "${PHISIKS_ROOT}/tests/unit/*.h")
foreach(f ${unit_files})
    file(RELATIVE_PATH rel "${PHISIKS_ROOT}" "${f}")
    file(STRINGS "${f}" lines REGEX "#[ \t]*include[ \t]*[<\"](glad/|GLFW/|Px|physx/|PxPhysics)")
    foreach(line ${lines})
        phisiks_report("${rel}" "${line}")
        math(EXPR unit_violations "${unit_violations} + 1")
    endforeach()
endforeach()

# Brand check -------------------------------------------------------------------------------------
set(BRAND_REGEX "[Hh][Ee][Ll][Ll]2025|[Uu][Nn][Ll][Oo][Vv][Ee][Dd]|[Ll][Ee][Gg][Aa][Cc][Yy]|[Vv][Ee][Tt][Tt][Aa][Ii]|[Mm][Aa][Nn][Nn][Aa][Nn]|H[e]ll::|H[e]ll/")
set(brand_violations 0)
set(brand_files)
foreach(dir engine apps tests tools assets docs examples)
    file(GLOB_RECURSE found
        "${PHISIKS_ROOT}/${dir}/*.h" "${PHISIKS_ROOT}/${dir}/*.hpp" "${PHISIKS_ROOT}/${dir}/*.cpp" "${PHISIKS_ROOT}/${dir}/*.c"
        "${PHISIKS_ROOT}/${dir}/*.glsl" "${PHISIKS_ROOT}/${dir}/*.cmake" "${PHISIKS_ROOT}/${dir}/*.txt"
        "${PHISIKS_ROOT}/${dir}/*.md" "${PHISIKS_ROOT}/${dir}/*.json" "${PHISIKS_ROOT}/${dir}/*.obj")
    list(APPEND brand_files ${found})
endforeach()
foreach(name CMakeLists.txt CMakePresets.json README.md LICENSE THIRD_PARTY_NOTICES.md .gitignore)
    if(EXISTS "${PHISIKS_ROOT}/${name}")
        list(APPEND brand_files "${PHISIKS_ROOT}/${name}")
    endif()
endforeach()
foreach(f ${brand_files})
    file(RELATIVE_PATH rel "${PHISIKS_ROOT}" "${f}")
    file(STRINGS "${f}" lines REGEX "${BRAND_REGEX}")
    foreach(line ${lines})
        string(STRIP "${line}" line)
        phisiks_report("${rel}" "${line}")
        math(EXPR brand_violations "${brand_violations} + 1")
    endforeach()
endforeach()

math(EXPR violations "${public_violations} + ${app_violations} + ${unit_violations} + ${brand_violations}")
if(violations GREATER 0)
    message(FATAL_ERROR "layering: ${violations} violation(s): public headers ${public_violations}, apps/runtime tests ${app_violations}, "
                        "unit tests ${unit_violations}, old-name references ${brand_violations}")
endif()
list(LENGTH brand_files scanned)
message(STATUS "layering: clean (public API, apps, tests; ${scanned} files free of old project names)")
