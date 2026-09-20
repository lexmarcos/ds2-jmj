//! Embeds the commit the harness was built from, and whether its sources had
//! uncommitted edits, so a run can say which code produced it.
//! `target/debug/ds2os-dev` outlives checkouts and edits, and an old binary
//! reads exactly like a new one.
//!
//! Once a build script prints `rerun-if-changed`, cargo reruns it only for the
//! paths it names. The sources are named too, or the edit flag would describe
//! the first build after a commit rather than this one.

use std::path::Path;
use std::process::Command;

fn git(dir: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new("git").arg("-C").arg(dir).args(args).output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    (output.status.success() && !text.is_empty()).then_some(text)
}

fn main() {
    let dir = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR"));
    // A build without git (a tarball, a CI checkout without history) still builds.
    let commit = git(&dir, &["rev-parse", "HEAD"]).unwrap_or_else(|| "unknown".into());
    let sources = [".", "../ds2os-core"];
    let mut status = vec!["status", "--porcelain", "--"];
    status.extend(sources);
    let dirty = commit != "unknown" && git(&dir, &status).is_some();
    println!("cargo:rustc-env=DS2OS_HARNESS_COMMIT={commit}");
    println!("cargo:rustc-env=DS2OS_HARNESS_DIRTY={dirty}");
    for path in ["build.rs", "Cargo.toml", "src", "tests", "../ds2os-core/Cargo.toml", "../ds2os-core/src"] {
        println!("cargo:rerun-if-changed={path}");
    }
    // HEAD moves on checkout and the branch's ref on commit. A ref can live in
    // packed-refs after a gc and come back as a new loose file on the next
    // commit, so the whole refs/heads directory is watched, not one file.
    for name in ["HEAD", "packed-refs", "refs/heads"] {
        if let Some(path) = git(&dir, &["rev-parse", "--git-path", name]).map(|p| dir.join(p)) {
            if path.exists() { println!("cargo:rerun-if-changed={}", path.display()); }
        }
    }
}
