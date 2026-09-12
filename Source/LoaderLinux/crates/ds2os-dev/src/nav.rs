//! Walking a character to a place, without anyone watching the screen.
//!
//! The injector publishes the local player's position and facing into
//! `DS2_Nav.txt` a few times a second (`DS2_NavHook`). That is half of what a
//! walk needs; the other half is knowing which way to push the stick, and the
//! game will not say. The left stick is **camera relative**, and the camera
//! turns as the character does, so there is no fixed mapping to learn once.
//!
//! So it is measured every step instead. A burst of stick input produces a
//! world displacement; the angle between what was asked for and what happened
//! is the camera's yaw, and the next burst is rotated by it. One sample is
//! enough because the mapping is a rotation - camera relative control neither
//! scales nor mirrors - and a stale estimate corrects itself on the next step.
//!
//! Two failures are reported rather than fought: falling (the height drops far
//! more than a step should) and being stuck (several bursts in a row that move
//! almost nothing, which is a wall). Both end the walk with what happened, so
//! a test that depended on it fails honestly instead of timing out.

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use crate::drive;
use crate::env::Environment;
use crate::pad;
use crate::screen;

const STATE: &str = "DS2_Nav.txt";

/// Where a character is and which way it is looking.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pose {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub facing_x: f32,
    pub facing_z: f32,
}

fn state_path(install_dir: &Path) -> PathBuf {
    install_dir.join(STATE)
}

/// Reads the published pose. `None` while no world is loaded, which is also
/// what the injector writes when the chain does not resolve.
pub fn read(install_dir: &Path) -> Option<Pose> {
    let text = std::fs::read_to_string(state_path(install_dir)).ok()?;
    let mut parts = text.split_whitespace();
    let mut next = || parts.next().and_then(|v| v.parse::<f32>().ok());
    Some(Pose {
        x: next()?,
        y: next()?,
        z: next()?,
        facing_x: next()?,
        facing_z: next()?,
    })
}

/// How a walk ended.
#[derive(Debug, Clone)]
pub enum Outcome {
    Arrived { steps: u32, distance: f32 },
    Stuck { steps: u32, distance: f32 },
    Fell { steps: u32, drop: f32 },
    LostPlayer { steps: u32 },
    TimedOut { steps: u32, distance: f32 },
}

/// What the walk is allowed to do.
#[derive(Debug, Clone, Copy)]
pub struct Plan {
    /// Close enough to stop, in metres.
    pub radius: f32,
    /// How long one burst of stick input lasts.
    pub burst: Duration,
    /// Give up after this long.
    pub timeout: Duration,
    /// A drop larger than this means the character fell off something.
    pub max_drop: f32,
}

impl Default for Plan {
    fn default() -> Self {
        Self {
            radius: 1.5,
            burst: Duration::from_millis(450),
            timeout: Duration::from_secs(90),
            max_drop: 6.0,
        }
    }
}

/// Walks the character of `instance` until it is within `plan.radius` of
/// `target`, or until something says it cannot.
pub fn walk_to(
    environment: &Environment,
    install_dir: &Path,
    account: u8,
    target: (f32, f32),
    plan: Plan,
    mut on_step: impl FnMut(u32, &Pose, f32),
) -> Result<Outcome, String> {
    // There is one virtual pad for both games, and the game ignores it while
    // another window is active, so focus is part of every burst rather than a
    // thing done once at the start.
    let window = drive::window_for(environment, account)?;
    let start = Instant::now();
    let mut steps = 0u32;
    let mut stalled = 0u32;

    // The camera's yaw, learned from the last burst. Starting at zero only
    // means the first burst is a guess; the second one is not.
    let mut yaw: f32 = 0.0;
    let mut have_yaw = false;

    let first = read(install_dir).ok_or("o jogo não está publicando posição")?;
    let mut previous = first;

    loop {
        let pose = match read(install_dir) {
            Some(pose) => pose,
            None => return Ok(Outcome::LostPlayer { steps }),
        };

        let dx = target.0 - pose.x;
        let dz = target.1 - pose.z;
        let distance = (dx * dx + dz * dz).sqrt();

        on_step(steps, &pose, distance);

        if distance <= plan.radius {
            return Ok(Outcome::Arrived { steps, distance });
        }
        if first.y - pose.y > plan.max_drop {
            return Ok(Outcome::Fell { steps, drop: first.y - pose.y });
        }
        if Instant::now().duration_since(start) > plan.timeout {
            return Ok(Outcome::TimedOut { steps, distance });
        }

        // What the last burst actually produced, against what it asked for.
        if steps > 0 {
            let moved_x = pose.x - previous.x;
            let moved_z = pose.z - previous.z;
            let moved = (moved_x * moved_x + moved_z * moved_z).sqrt();
            if moved < 0.15 {
                stalled += 1;
                if stalled >= 4 {
                    return Ok(Outcome::Stuck { steps, distance });
                }
            } else {
                stalled = 0;
                // The rotation that takes the stick direction to the world
                // direction. Smoothed, because a single burst that clipped a
                // wall would otherwise throw the next one off.
                let wanted = last_requested_angle(&previous, target);
                let got = moved_z.atan2(moved_x);
                let sample = wrap(got - wanted);
                yaw = if have_yaw { wrap(yaw + 0.6 * wrap(sample - yaw)) } else { sample };
                have_yaw = true;
            }
        }

        // Desired world direction, rotated back into stick space.
        let world = dz.atan2(dx);
        let stick = world - yaw;

        // A stalled walk tries a sidestep before giving up: most walls here are
        // cleared by a metre of strafe, and the alternative is a dead test.
        let stick = if stalled > 0 {
            stick + std::f32::consts::FRAC_PI_2 * if stalled % 2 == 0 { 1.0 } else { -1.0 }
        } else {
            stick
        };

        // The pad takes x right and y up-negative, which is why the z component
        // is negated on the way out.
        let sx = stick.cos();
        let sy = stick.sin();
        screen::focus(&window)?;
        pad::send(
            1,
            &format!(
                "stick l {:.3} {:.3} {}",
                sx.clamp(-1.0, 1.0),
                sy.clamp(-1.0, 1.0),
                plan.burst.as_millis()
            ),
        )
        .map_err(|e| format!("conta {account}: {e}"))?;

        previous = pose;
        steps += 1;
        std::thread::sleep(Duration::from_millis(120));
    }
}

/// The world angle the previous burst was aimed at, recomputed from where the
/// character stood then. Keeping it out of the loop state means the estimate
/// never drifts from a value nobody can check.
fn last_requested_angle(from: &Pose, target: (f32, f32)) -> f32 {
    (target.1 - from.z).atan2(target.0 - from.x)
}

fn wrap(angle: f32) -> f32 {
    let mut a = angle;
    while a > std::f32::consts::PI {
        a -= 2.0 * std::f32::consts::PI;
    }
    while a < -std::f32::consts::PI {
        a += 2.0 * std::f32::consts::PI;
    }
    a
}
