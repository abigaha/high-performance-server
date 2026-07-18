add_rules("mode.debug", "mode.release")
set_languages("c++20")

add_requires("gtest")
add_requires("nlohmann_json")

-- ===== 包含所有子模块 =====

includes("core")
includes("logger")
includes("file-system")
includes("memory-pool")
includes("net")
includes("db")

-- ===== 公共 include 目录列表（测试和 benchmark 共用）=====

local common_includedirs = {
    "net/tcp/tcp_server/include",
    "net/tcp/tcp_client/include",
    "net/thread-pool/include",
    "net/http/include",
    "net/coroutine",
    "net/file-transfer/include",
    "net/ssl/include",
    "net/websocket/include",
    "logger/include",
    "memory-pool/include",
    "file-system/include",
    "db/include",
}

local common_deps = {
    "tcp_server", "tcp_client", "websocket", "http", "file-system", "db",
    "file-transfer", "net_ssl",
}

-- ===== 测试（自动发现 tests/ 目录下的测试文件）=====

for _, file in ipairs(os.files("tests/*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_packages("gtest")
        add_files(file)
        add_includedirs(common_includedirs)
        add_deps(common_deps)
        -- 为 test binary 设置 RPATH（传递性），而非 RUNPATH
        add_ldflags("-Wl,-rpath," .. path.join(os.projectdir(), "lib"), "-Wl,--disable-new-dtags", {force = true})
        add_tests("default")
end

-- ===== 性能基准测试（自动发现 benchmark/ 目录下的文件）=====

if os.host() == "linux" and os.isfile("/usr/lib/x86_64-linux-gnu/libbenchmark.so") then
    for _, file in ipairs(os.files("benchmark/bench_*.cpp")) do
        local name = path.basename(file)
        target(name)
            set_kind("binary")
            set_targetdir(path.join(os.projectdir(), "bin"))
            set_rundir("$(projectdir)")
            set_group("benchmark")
            add_files(file)
            add_includedirs(common_includedirs)
            add_includedirs("/usr/include")
            add_deps(common_deps)
            add_syslinks("benchmark", "pthread")
            add_ldflags("-L/usr/lib/x86_64-linux-gnu",
                        "-Wl,--allow-shlib-undefined",
                        "-Wl,-rpath," .. path.join(os.projectdir(), "lib"),
                        "-Wl,--disable-new-dtags",
                        {force = true})
            if is_mode("debug") then
                add_cxflags("-UNDEBUG")
            end
    end

    -- ===== QPS 基准测试（自动发现 benchmark/qps_*.cpp，不依赖 libbenchmark）=====

    for _, file in ipairs(os.files("benchmark/qps_*.cpp")) do
        local name = path.basename(file) -- e.g. "qps_chunk_header"
        target(name)
            set_kind("binary")
            set_targetdir(path.join(os.projectdir(), "bin"))
            set_rundir("$(projectdir)")
            set_group("benchmark")
            add_files(file)
            add_includedirs(common_includedirs)
            add_deps(common_deps)
            add_syslinks("pthread", "rt")
            add_ldflags("-Wl,-rpath," .. path.join(os.projectdir(), "lib"),
                        "-Wl,--allow-shlib-undefined",
                        "-Wl,--disable-new-dtags", {force = true})
            if is_mode("debug") then
                add_cxflags("-UNDEBUG")
            end
    end
end
