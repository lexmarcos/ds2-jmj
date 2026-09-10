//! Which Proton build to run the game with.
//!
//! Getting this wrong is not cosmetic. Proton refuses, or rebuilds, a prefix
//! created by a newer build than the one running, so picking the wrong one can
//! cost the player their saved settings.
//!
//! The obvious source, Steam's `CompatToolMapping`, is often simply absent:
//! it only records what the player has *overridden*, so a game running on the
//! default has no entry at all. The reliable source is the prefix itself.
//! Steam writes `compatdata/<appid>/config_info` when it builds one, and its
//! second line points inside the exact Proton that did the building.

use std::path::{Path, PathBuf};

use crate::steam::{GameInstall, Steam};
use crate::vdf;

/// A Proton installation on disk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProtonBuild {
    /// Directory holding the `proton` launcher script.
    pub dir: PathBuf,
    /// Build date from the `version` file, as a unix timestamp. Zero when the
    /// file is missing, which sorts it last.
    pub stamp: u64,
    /// Second field of the `version` file, e.g. `experimental-11.0-20260903c-x86_64`.
    pub label: Option<String>,
}

impl ProtonBuild {
    /// The launcher script Proton is driven through.
    pub fn launcher(&self) -> PathBuf {
        self.dir.join("proton")
    }

    fn at(dir: PathBuf) -> Option<Self> {
        if !dir.join("proton").is_file() {
            return None;
        }
        let (stamp, label) = read_version(&dir);
        Some(Self { dir, stamp, label })
    }
}

/// `version` holds a unix timestamp and a build name on one line.
fn read_version(dir: &Path) -> (u64, Option<String>) {
    let Ok(text) = std::fs::read_to_string(dir.join("version")) else {
        return (0, None);
    };
    let mut parts = text.split_whitespace();
    let stamp = parts.next().and_then(|s| s.parse().ok()).unwrap_or(0);
    let label = parts.next().map(str::to_owned);
    (stamp, label)
}

/// The build that created this game's prefix, read from `config_info`.
///
/// Line 2 is a path inside the Proton that built the prefix — on this machine
/// `.../Proton - Experimental/files/share/fonts/`. Trimming the known suffix
/// gives the build directory.
pub fn from_prefix(install: &GameInstall) -> Option<ProtonBuild> {
    let compatdata = install.compatdata_dir()?;
    let text = std::fs::read_to_string(compatdata.join("config_info")).ok()?;

    let line = text.lines().nth(1)?.trim();
    let dir = line.strip_suffix("/files/share/fonts/").or_else(|| {
        // Older layouts point at the lib directory instead.
        line.strip_suffix("/files/lib/")
    })?;

    ProtonBuild::at(PathBuf::from(dir))
}

/// The build the player pinned for this game in Steam, if they pinned one.
///
/// Absent for a game left on the default, which is the common case — Steam
/// only records an override here.
pub fn from_mapping(steam: &Steam, app_id: u32) -> Option<ProtonBuild> {
    let text = std::fs::read_to_string(steam.root().join("config/config.vdf")).ok()?;
    let parsed = vdf::parse(&text).ok()?;

    let mapping = parsed.path([
        "InstallConfigStore",
        "Software",
        "Valve",
        "Steam",
        "CompatToolMapping",
    ])?;

    // The game's own entry, else the "all other titles" default under key "0".
    let name = mapping
        .get(&app_id.to_string())
        .and_then(|entry| entry.get("name"))
        .and_then(vdf::Value::as_str)
        .filter(|name| !name.is_empty())
        .or_else(|| {
            mapping
                .get("0")
                .and_then(|entry| entry.get("name"))
                .and_then(vdf::Value::as_str)
                .filter(|name| !name.is_empty())
        })?;

    resolve_name(steam, name)
}

/// Turns a compat tool name from `config.vdf` into a directory.
fn resolve_name(steam: &Steam, name: &str) -> Option<ProtonBuild> {
    // Third party tools declare themselves and their install path.
    let tools = steam.root().join("compatibilitytools.d");
    if let Ok(entries) = std::fs::read_dir(&tools) {
        for entry in entries.flatten() {
            let manifest = entry.path().join("compatibilitytool.vdf");
            let Ok(text) = std::fs::read_to_string(&manifest) else {
                continue;
            };
            let Ok(parsed) = vdf::parse(&text) else {
                continue;
            };
            let Some(tools) = parsed.path(["compatibilitytools", "compat_tools"]) else {
                continue;
            };
            for (tool_name, body) in tools.entries() {
                if tool_name != name {
                    continue;
                }
                let path = body
                    .get("install_path")
                    .and_then(vdf::Value::as_str)
                    .map(|p| {
                        if p == "." {
                            entry.path()
                        } else {
                            entry.path().join(p)
                        }
                    })
                    .unwrap_or_else(|| entry.path());
                if let Some(build) = ProtonBuild::at(path) {
                    return Some(build);
                }
            }
        }
    }

    // Official builds are named like `proton_experimental` or `proton_9`, and
    // live under a directory whose name reads very differently. Match loosely
    // on the distinguishing word rather than trying to keep a table in step
    // with Valve's naming.
    let needle = name.trim_start_matches("proton").trim_matches(['_', '-']).to_lowercase();
    let mut candidates = installed(steam);
    candidates.retain(|build| {
        let dir = build.dir.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
        needle.is_empty() || dir.contains(&needle)
    });
    candidates.into_iter().next()
}

/// Every Proton on disk, newest build first.
pub fn installed(steam: &Steam) -> Vec<ProtonBuild> {
    let mut builds: Vec<ProtonBuild> = steam
        .proton_builds()
        .into_iter()
        .filter_map(ProtonBuild::at)
        .collect();

    // By build date. Sorting by path, as this used to, ranks "Proton 9.0"
    // above "Proton - Experimental" purely because '-' sorts before '9'.
    builds.sort_by(|a, b| b.stamp.cmp(&a.stamp).then_with(|| a.dir.cmp(&b.dir)));
    builds.dedup_by(|a, b| a.dir == b.dir);
    builds
}

/// The build to launch this game with.
///
/// The prefix is asked first because it is the only source that names the
/// build the player's saves were made under.
pub fn for_game(steam: &Steam, install: &GameInstall) -> Option<ProtonBuild> {
    from_prefix(install)
        .or_else(|| from_mapping(steam, install.app_id))
        .or_else(|| installed(steam).into_iter().next())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join("ds2os-core-proton-test").join(name);
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn fake_build(dir: &Path, stamp: &str, label: &str) {
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join("proton"), "#!/usr/bin/env python3\n").unwrap();
        std::fs::write(dir.join("version"), format!("{stamp} {label}\n")).unwrap();
    }

    #[test]
    fn reads_the_build_out_of_a_prefix() {
        let root = scratch("prefix");
        let proton = root.join("steamapps/common/Proton - Experimental");
        fake_build(&proton, "1788804323", "experimental-11.0-20260903c-x86_64");

        let compatdata = root.join("steamapps/compatdata/335300");
        std::fs::create_dir_all(compatdata.join("pfx")).unwrap();
        std::fs::write(
            compatdata.join("config_info"),
            format!("11.0-100\n{}/files/share/fonts/\n", proton.display()),
        )
        .unwrap();

        let install = GameInstall {
            app_id: 335300,
            game_type: crate::steam::GameType::DarkSouls2,
            install_dir: root.join("steamapps/common/game"),
            library: root.clone(),
            prefix_path: Some(compatdata.join("pfx")),
        };

        let found = from_prefix(&install).expect("prefix names its proton");
        assert_eq!(found.dir, proton);
        assert_eq!(found.stamp, 1_788_804_323);
        assert_eq!(found.label.as_deref(), Some("experimental-11.0-20260903c-x86_64"));
    }

    #[test]
    fn newest_wins_over_alphabetical() {
        let root = scratch("newest");
        let common = root.join("steamapps/common");
        // '-' sorts before '9', so sorting by path would pick "Proton 9.0".
        fake_build(&common.join("Proton - Experimental"), "1788804323", "experimental");
        fake_build(&common.join("Proton 9.0"), "1700000000", "9.0-4");
        std::fs::create_dir_all(root.join("steamapps")).unwrap();

        let steam = Steam::from_root(&root);
        let order = installed(&steam);
        assert_eq!(order.first().unwrap().dir, common.join("Proton - Experimental"));
    }

    #[test]
    fn a_directory_without_the_launcher_is_not_a_build() {
        let root = scratch("empty");
        std::fs::create_dir_all(root.join("steamapps/common/Proton Broken")).unwrap();
        let steam = Steam::from_root(&root);
        assert!(installed(&steam).is_empty());
    }
}
