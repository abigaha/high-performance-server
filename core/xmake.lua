target("high-performance-server")
set_kind("binary")
set_targetdir(path.join(os.projectdir(), "bin"))
add_files("src/*.cpp")
add_deps("logger", "net")

-- Compilation options
add_cxxflags("-g", "-O0", "-Wall", "-Wextra", "-Wpedantic", "-Werror")

-- Sanitizers
add_cxxflags("-fsanitize=address", "-fsanitize=undefined")
add_ldflags("-fsanitize=address", "-fsanitize=undefined")

-- Precompiled headers
set_pcxxheader("pch.h")
