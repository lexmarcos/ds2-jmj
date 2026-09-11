//! Bakes the server address and public key into the binary.
//!
//! They come from a `.env` that is not in the repository, or from real
//! environment variables so a CI release can pass them as secrets. Either way
//! the player never sees a key, never imports one, and there is no server
//! field anywhere in the interface.
//!
//! The values are written into a generated source file rather than through
//! `cargo:rustc-env`, which is line based and would silently truncate a PEM
//! key at its first newline.

use std::{env, fs, path::PathBuf};

/// The injector copies the hostname over the retail one in UTF-16 units with
/// no bounds check (`DS2_ReplaceServerAddressHook.cpp:61-71`), and the retail
/// string is 45 units long.
const HOST_MAX_CHARS: usize = 45;

/// Same again for the key, in bytes, over a 426 byte literal (`:128`).
const KEY_MAX_BYTES: usize = 426;

fn main() {
    println!("cargo:rerun-if-changed=.env");
    for var in [
        "DS2OS_SERVER_HOST",
        "DS2OS_SERVER_KEY",
        "DS2OS_SERVER_KEY_FILE",
        "DS2OS_SERVER_NAME",
    ] {
        println!("cargo:rerun-if-env-changed={var}");
    }

    let file = read_env_file();
    let host = value("DS2OS_SERVER_HOST", &file);
    let name = value("DS2OS_SERVER_NAME", &file);

    // A PEM key is several lines and .env is one line per value, so pasting a
    // key in means escaping every newline by hand. Pointing at the file the
    // server wrote is the same thing without the ceremony.
    let key_file = value("DS2OS_SERVER_KEY_FILE", &file);
    let key = if key_file.is_empty() {
        value("DS2OS_SERVER_KEY", &file)
    } else {
        println!("cargo:rerun-if-changed={key_file}");
        fs::read_to_string(&key_file)
            .unwrap_or_else(|error| panic!("não consegui ler {key_file}: {error}"))
    };

    if host.is_empty() || key.is_empty() {
        panic!(
            "sem servidor embutido. Copie .env.example para .env e preencha \
             DS2OS_SERVER_HOST e uma de DS2OS_SERVER_KEY ou \
             DS2OS_SERVER_KEY_FILE, ou passe as duas como variaveis de ambiente."
        );
    }

    // The hook writes UTF-16 units, so counting bytes would let a non-ASCII
    // hostname through and corrupt whatever follows the retail string.
    if !host.is_ascii() {
        panic!("DS2OS_SERVER_HOST precisa ser ASCII; recebi {host:?}");
    }
    if host.chars().count() > HOST_MAX_CHARS {
        panic!(
            "DS2OS_SERVER_HOST tem {} caracteres; o maximo e {HOST_MAX_CHARS}, \
             porque ele e escrito por cima do hostname retail sem checagem de limite",
            host.chars().count()
        );
    }

    // Measure what will actually ship: the server compares the key byte for
    // byte, so the loader sends it normalised and that is the form that has
    // to fit.
    let key = ds2os_core::normalize_public_key(&key).unwrap_or_else(|| {
        panic!("DS2OS_SERVER_KEY nao parece uma chave publica RSA em PEM")
    });
    if key.len() > KEY_MAX_BYTES {
        panic!(
            "DS2OS_SERVER_KEY tem {} bytes depois de normalizada; o maximo e {KEY_MAX_BYTES}",
            key.len()
        );
    }

    let name = if name.is_empty() { "Jamuja".to_owned() } else { name };
    for (label, text) in [("host", &host), ("name", &name)] {
        if text.contains("\"#") {
            panic!("{label} contem a sequencia que fecha o literal gerado");
        }
    }

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("pinned.rs");
    fs::write(
        &out,
        format!(
            "pub const SERVER_NAME: &str = r#\"{name}\"#;\n\
             pub const SERVER_HOST: &str = r#\"{host}\"#;\n\
             pub const SERVER_KEY: &str = r#\"{key}\"#;\n"
        ),
    )
    .expect("write pinned.rs");
}

fn read_env_file() -> Vec<(String, String)> {
    let text = fs::read_to_string(".env").unwrap_or_default();
    text.lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .filter_map(|line| line.split_once('='))
        .map(|(key, value)| {
            let value = value.trim().trim_matches('"').replace("\\n", "\n");
            (key.trim().to_owned(), value)
        })
        .collect()
}

/// `.env` first, then the real environment, so a developer's file wins locally
/// and CI secrets work with no file at all.
fn value(name: &str, file: &[(String, String)]) -> String {
    file.iter()
        .find(|(key, _)| key == name)
        .map(|(_, value)| value.clone())
        .filter(|value| !value.is_empty())
        .or_else(|| env::var(name).ok().map(|v| v.replace("\\n", "\n")))
        .unwrap_or_default()
}
