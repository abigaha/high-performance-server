target("db")
  set_kind("shared")
  set_targetdir(path.join(os.projectdir(), "lib"))
  add_files("src/*.cpp")
  add_headerfiles("include/*.h", "include/*.hpp")
  add_includedirs("include", { public = true })
  -- 系统 Boost（包括 boost::mysql）
  add_syslinks("boost_system", "pthread")
