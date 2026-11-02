fn main() {
    let source =
        std::env::var("SQUAD_MUMBLE_CRYPT_DIR").unwrap_or_else(|_| "mumble-crypt".to_owned());
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .include(&source)
        .file(format!("{source}/CryptStateOCB2.cpp"))
        .file(format!("{source}/bridge.cpp"));
    if let Some(paths) = std::env::var_os("DEP_OPENSSL_INCLUDE") {
        for path in std::env::split_paths(&paths) {
            build.include(path);
        }
    }
    build.compile("squad_mumble_crypt");
    println!("cargo:rerun-if-changed={source}");
    println!("cargo:rerun-if-env-changed=SQUAD_MUMBLE_CRYPT_DIR");
}
