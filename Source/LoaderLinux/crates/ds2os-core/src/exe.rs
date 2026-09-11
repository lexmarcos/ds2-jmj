//! Identifying the game executable.
//!
//! Every offset this project patches is hardcoded against one build. That is
//! not a detail a loader can shrug at: `DS2_ReplaceServerAddressHook` searches
//! for the retail hostname inside a `while (true)` loop, so against a
//! different executable the injector thread spins forever. The game still
//! starts, the injector still logs "injected", and the player ends up on
//! FromSoftware's servers believing they are on the private one.
//!
//! So a mismatch has to stop the launch, and the only way to be sure is to
//! look at the bytes.

use std::path::Path;

use sha2::{Digest, Sha256};

/// Size and content hash of a game executable.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fingerprint {
    pub size: u64,
    pub sha256: [u8; 32],
}

impl Fingerprint {
    pub fn hex(&self) -> String {
        self.sha256.iter().map(|b| format!("{b:02x}")).collect()
    }
}

/// Dark Souls II: Scholar of the First Sin, version 1.03, Calibrations 2.02.
///
/// Measured on two independent Steam downloads on two accounts; both agree,
/// including Steam's own buildid 9527516.
pub const DS2_SOTFS_1_03: Fingerprint = Fingerprint {
    size: 28_200_992,
    sha256: [
        0x00, 0x45, 0x93, 0x1b, 0x89, 0x14, 0x50, 0x45, 0x31, 0xb7, 0x86, 0x4a, 0x94, 0x88, 0xd3,
        0x96, 0xdc, 0x50, 0xcb, 0xaf, 0x52, 0x49, 0x64, 0x01, 0x6e, 0x1d, 0x69, 0xc3, 0xd1, 0x17,
        0x31, 0x31,
    ],
};

/// Reads size and hash. Hashing 28 MB costs tens of milliseconds, so callers
/// on a UI thread should push this onto a worker.
pub fn fingerprint(path: &Path) -> std::io::Result<Fingerprint> {
    use std::io::Read;

    let file = std::fs::File::open(path)?;
    let size = file.metadata()?.len();

    let mut reader = std::io::BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }

    let mut sha256 = [0u8; 32];
    sha256.copy_from_slice(&hasher.finalize());
    Ok(Fingerprint { size, sha256 })
}

/// Size alone, for a first pass that avoids hashing an obviously wrong file.
pub fn size_of(path: &Path) -> std::io::Result<u64> {
    Ok(std::fs::metadata(path)?.len())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trips_the_known_build() {
        assert_eq!(
            DS2_SOTFS_1_03.hex(),
            "0045931b8914504531b7864a9488d396dc50cbaf524964016e1d69c3d1173131"
        );
    }

    #[test]
    fn fingerprints_a_file_on_disk() {
        let dir = std::env::temp_dir().join("ds2os-core-exe-test");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("sample.bin");
        std::fs::write(&path, b"hello").unwrap();

        let taken = fingerprint(&path).unwrap();
        assert_eq!(taken.size, 5);
        // sha256("hello")
        assert_eq!(
            taken.hex(),
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
        );

        std::fs::remove_file(&path).ok();
    }
}
