target("high-performance-server")
set_kind("binary")
set_targetdir(path.join(os.projectdir(), "bin"))
add_files("src/*.cpp")
add_deps("logger", "http", "db", "file-system")
add_packages("nlohmann_json")

if is_mode("debug") then
  add_cxxflags("-g", "-O0", "-Wall", "-Wextra", "-Wpedantic", "-Werror")
  add_cxxflags("-fsanitize=address", "-fsanitize=undefined")
  add_ldflags("-fsanitize=address", "-fsanitize=undefined")
else
  add_cxxflags("-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Wpedantic", "-Werror")
end

add_ldflags("-Wl,-rpath,$ORIGIN/lib", "-Wl,--disable-new-dtags", {force = true})

add_includedirs("include")
set_pcxxheader("pch.h")
