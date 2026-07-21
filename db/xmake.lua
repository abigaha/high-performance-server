target("db")
  set_kind("shared")
  set_targetdir(path.join(os.projectdir(), "lib"))
  add_files("src/*.cpp")
  add_headerfiles("include/*.h", "include/*.hpp")
  add_includedirs("include", { public = true })
  -- 系统 Boost（boost::mysql header-only 模式）
  add_defines("BOOST_MYSQL_HEADER_ONLY")
  add_syslinks("boost_system", "ssl", "crypto", "pthread")
