//! Exclusive control, cancellation and bounded waits. Locks are released by the OS on exit.
use std::{fs::File, path::Path, sync::atomic::{AtomicBool, Ordering}, time::{Duration, Instant}};

static CANCELLED: AtomicBool = AtomicBool::new(false);
extern "C" fn cancel(_: libc::c_int) { CANCELLED.store(true, Ordering::Relaxed); }

pub fn install() {
    unsafe {
        libc::signal(libc::SIGINT, cancel as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, cancel as *const () as libc::sighandler_t);
    }
}

pub fn check() -> Result<(), String> {
    if CANCELLED.load(Ordering::Relaxed) { Err("cancelled: operação cancelada".into()) } else { Ok(()) }
}

pub fn sleep(duration: Duration) -> Result<(), String> {
    let end = Instant::now() + duration;
    loop {
        check()?;
        let left = end.saturating_duration_since(Instant::now());
        if left.is_zero() { return Ok(()); }
        std::thread::sleep(left.min(Duration::from_millis(50)));
    }
}

pub struct Lock(File);
impl Lock {
    pub fn acquire(path: &Path) -> Result<Self, String> {
        let file = File::options().read(true).write(true).create(true).truncate(false)
            .open(path).map_err(|e| format!("lock {}: {e}", path.display()))?;
        file.try_lock().map_err(|e| format!("busy: outra operação controla {}: {e}", path.display()))?;
        Ok(Self(file))
    }
}
impl Drop for Lock { fn drop(&mut self) { let _ = self.0.unlock(); } }

#[derive(Clone, Copy)]
pub struct Deadline(Instant);
impl Deadline {
    pub fn after(duration: Duration) -> Self { Self(Instant::now() + duration) }
    pub fn remaining(self) -> Result<Duration, String> {
        check()?;
        let left = self.0.saturating_duration_since(Instant::now());
        if left.is_zero() { Err("timeout: prazo da operação esgotado".into()) } else { Ok(left) }
    }
    pub fn sleep(self, duration: Duration) -> Result<(), String> {
        sleep(duration.min(self.remaining()?))?;
        self.remaining().map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lock_excludes_other_openers_and_releases() {
        let path = std::env::temp_dir().join(format!("ds2-lock-{}", std::process::id()));
        let first = Lock::acquire(&path).unwrap();
        assert!(Lock::acquire(&path).is_err());
        drop(first);
        assert!(Lock::acquire(&path).is_ok());
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn expired_deadline_cannot_be_extended_by_an_inner_wait() {
        assert!(Deadline::after(Duration::ZERO).remaining().is_err());
    }
}
