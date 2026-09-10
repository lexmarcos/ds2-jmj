//! Prints what the loader can find on this machine. Useful when a user reports
//! that their game is not detected: ask them to run this and paste the output.

use ds2os_core::{exe, process, proton, GameType, Steam};

fn main() {
    let steam = match Steam::discover() {
        Ok(steam) => steam,
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    };

    println!("steam root   {}", steam.root().display());

    println!("libraries");
    for library in steam.libraries() {
        println!("  {}", library.display());
    }

    println!("proton builds, newest first");
    let builds = proton::installed(&steam);
    if builds.is_empty() {
        println!("  none");
    }
    for build in &builds {
        println!(
            "  {}  ({})",
            build.dir.display(),
            build.label.as_deref().unwrap_or("sem version")
        );
    }

    print!("steam running  ");
    match process::steam_running() {
        Some(true) => println!("yes"),
        Some(false) => println!("no"),
        None => println!("cannot tell on this platform"),
    }

    for game_type in [GameType::DarkSouls2, GameType::DarkSouls3] {
        print!("{game_type:?}  ");
        match steam.find_game(game_type) {
            None => println!("not installed"),
            Some(install) => {
                println!("{}", install.install_dir.display());
                match install.executable() {
                    Some(exe) => println!("  exe     {}", exe.display()),
                    None => println!("  exe     not found under the install directory"),
                }
                match &install.prefix_path {
                    Some(prefix) => println!("  prefix  {}", prefix.display()),
                    None => println!("  prefix  none yet; run the game once through Steam"),
                }

                match proton::for_game(&steam, &install) {
                    Some(build) => println!(
                        "  proton  {}  ({})",
                        build.dir.display(),
                        build.label.as_deref().unwrap_or("sem version")
                    ),
                    None => println!("  proton  none found"),
                }

                if let Some(path) = install.executable() {
                    match exe::fingerprint(&path) {
                        Ok(taken) if taken == exe::DS2_SOTFS_1_03 => {
                            println!("  build   1.03, a esperada")
                        }
                        Ok(taken) => println!(
                            "  build   DIFERENTE: {} bytes, sha256 {}",
                            taken.size,
                            taken.hex()
                        ),
                        Err(error) => println!("  build   nao consegui ler: {error}"),
                    }
                }
            }
        }
    }
}
