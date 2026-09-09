//! Prints what the loader can find on this machine. Useful when a user reports
//! that their game is not detected: ask them to run this and paste the output.

use ds2os_core::{GameType, Steam};

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

    println!("proton builds");
    let builds = steam.proton_builds();
    if builds.is_empty() {
        println!("  none");
    }
    for build in builds {
        println!("  {}", build.display());
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
                match install.prefix_path {
                    Some(prefix) => println!("  prefix  {}", prefix.display()),
                    None => println!("  prefix  none yet; run the game once through Steam"),
                }
            }
        }
    }
}
