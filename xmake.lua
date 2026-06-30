add_rules("mode.debug", "mode.release")
set_languages("c++20")

add_requires("gtest")

-- ===== 包含所有子模块 =====

includes("core")
includes("logger")
includes("file-system")
includes("memory-pool")
includes("net")

-- ===== 测试（自动发现 tests/ 目录下的测试文件）=====

for _, file in ipairs(os.files("tests/*.cpp")) do
    local name = path.basename(file)
    target("test_" .. name)
        set_kind("binary")
        add_packages("gtest")
        add_files(file)
        add_includedirs("net/tcp/tcp_server/include",
                        "net/tcp/tcp_client/include",
                        "net/thread-pool/include",
                        "net/http/include",
                        "net/coroutine",
                        "logger/include",
                        "memory-pool/include")
        add_deps("tcp_server", "tcp_client", "http")
        -- 为 test binary 设置 RPATH（传递性），而非 RUNPATH
        add_ldflags("-Wl,-rpath," .. path.join(os.projectdir(), "lib"), "-Wl,--disable-new-dtags", {force = true})
        add_tests("default")
end
