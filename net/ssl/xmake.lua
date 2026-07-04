target("net_ssl")
set_kind("shared")
set_targetdir(path.join(os.projectdir(), "lib"))
add_files("src/*.cpp")
add_headerfiles("include/(**.h)")
add_includedirs("include", { public = true })
add_packages("openssl", { public = true })
add_syslinks("ssl", "crypto", { public = true })

after_build(function(target)
    local cert_dir = path.join(os.projectdir(), "build/certs")
    local cert_file = path.join(cert_dir, "cert.pem")
    if not os.isfile(cert_file) then
        os.mkdir(cert_dir)
        os.execv("openssl", {"req", "-x509", "-newkey", "rsa:2048",
                 "-keyout", path.join(cert_dir, "key.pem"),
                 "-out", cert_file,
                 "-days", "3650", "-nodes",
                 "-subj", "/CN=localhost/O=hps/C=CN"})
        os.execv("openssl", {"x509", "-in", cert_file, "-outform", "DER",
                 "-out", path.join(cert_dir, "cert.der")})
    end
end)
