//! These run against whatever Steam install exists on the machine. They assert
//! shape rather than specific games, so they stay green on a build box.

use ds2os_core::{GameType, Steam};

#[test]
fn discovers_steam_when_present() {
    let Ok(steam) = Steam::discover() else {
        eprintln!("no Steam installation here; skipping");
        return;
    };

    assert!(steam.root().join("steamapps").is_dir());

    let libraries = steam.libraries();
    assert!(!libraries.is_empty(), "the install root is always a library");
    assert_eq!(libraries[0], steam.root());
}

#[test]
fn game_lookup_is_consistent() {
    let Ok(steam) = Steam::discover() else {
        return;
    };

    for game_type in [GameType::DarkSouls2, GameType::DarkSouls3] {
        let Some(install) = steam.find_game(game_type) else {
            continue;
        };
        assert_eq!(install.app_id, game_type.app_id());
        assert!(install.install_dir.is_dir());
        if let Some(prefix) = &install.prefix_path {
            assert!(prefix.join("drive_c").is_dir(), "a prefix always has drive_c");
        }
    }
}

#[test]
fn proton_builds_ship_their_launcher() {
    let Ok(steam) = Steam::discover() else {
        return;
    };

    for build in steam.proton_builds() {
        assert!(build.join("proton").is_file());
    }
}
