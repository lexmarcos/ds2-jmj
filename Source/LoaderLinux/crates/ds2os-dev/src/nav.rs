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
    /// Which sample this is. Two reads with the same tick are the same
    /// reading, however much time passed between them.
    pub tick: u64,
    /// The multiplayer role, the same number the server calls `archetype`.
    pub archetype: u32,
}

fn state_path(install_dir: &Path) -> PathBuf {
    install_dir.join(STATE)
}

/// Reads the published pose. `None` while no world is loaded, which is also
/// what the injector writes when the chain does not resolve.
pub fn read(install_dir: &Path) -> Option<Pose> {
    let text = std::fs::read_to_string(state_path(install_dir)).ok()?;
    let fields: Vec<&str> = text.split_whitespace().collect();
    let mut parts = fields.iter();
    let mut next = || parts.next().and_then(|v| v.parse::<f32>().ok());
    let pose = Pose {
        x: next()?,
        y: next()?,
        z: next()?,
        facing_x: next()?,
        facing_z: next()?,
        // The pointer sits between the facing and the tick and is of no use
        // here - it is the same in both instances.
        tick: fields
            .get(6)
            .and_then(|v| v.parse::<u64>().ok())
            .unwrap_or(0),
        archetype: fields
            .get(7)
            .and_then(|v| v.parse::<u32>().ok())
            .unwrap_or(0),
    };

    // While an area loads, the chain resolves but everything in it is still
    // zero. The facing is a normalised direction and is never (0, 0) on a
    // character that exists, so it is the honest liveness test - and a walk
    // that started from a zeroed pose would drive off in a straight line.
    if pose.facing_x == 0.0 && pose.facing_z == 0.0 {
        return None;
    }
    Some(pose)
}

/// How a walk ended.
#[derive(Debug, Clone)]
pub enum Outcome {
    Arrived { steps: u32, distance: f32 },
    Stuck { steps: u32, distance: f32 },
    Fell { steps: u32, drop: f32 },
    /// The character was moved by something other than the walk: a death and
    /// respawn, a Homeward Bone, an area load. Whatever it was, the walk is no
    /// longer about the same journey.
    Teleported { steps: u32, jumped: f32 },
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
            radius: 2.0,
            burst: Duration::from_millis(1200),
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

    let first = fresh(install_dir, 0, Duration::from_secs(5))
        .ok_or("o jogo não está publicando posição")?;
    let mut previous = first;
    let mut sent: Option<f32> = None;
    let mut grown: u64 = 0;
    // Which of the eight compass directions to try next when nothing moves.
    let mut probe: u32 = 0;

    loop {
        // Never measure against a reading that has not been taken since the
        // last burst: a repeated sample looks exactly like a character that
        // did not move, and that mistake reported a walk as stuck while it was
        // standing on its target.
        let pose = match fresh(install_dir, previous.tick, Duration::from_secs(3)) {
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

        if let Some(asked) = sent {
            let moved_x = pose.x - previous.x;
            let moved_z = pose.z - previous.z;
            let moved = (moved_x * moved_x + moved_z * moved_z).sqrt();

            // One burst cannot cover this much ground, so something else moved
            // the character - a death and respawn, most often. Saying so beats
            // walking on from wherever they landed: two characters were killed
            // by this walk before it could tell, and the runs after them were
            // measuring nothing.
            if moved > 8.0 {
                return Ok(Outcome::Teleported { steps, jumped: moved });
            }

            if moved < 0.20 {
                // The character turns before it walks, and a burst that ends
                // during the turn covers no ground - measured, a 700 ms one
                // after a right angle moved nothing at all. So a burst that
                // achieved little buys a longer one rather than a verdict; it
                // is also the only way the estimate below gets any signal.
                stalled += 1;
                grown = (grown * 2).min(2400);
                probe += 1;
                if stalled >= 9 {
                    return Ok(Outcome::Stuck { steps, distance });
                }
            } else {
                grown = 0;
                probe = 0;
                stalled = 0;
                // The rotation that takes what was asked for to what happened.
                // It has to be the angle actually sent, not the one aimed at
                // the target, because a sidestep sends something else.
                let got = moved_z.atan2(moved_x);
                let sample = wrap(got - asked);
                yaw = if have_yaw { wrap(yaw + 0.6 * wrap(sample - yaw)) } else { sample };
                have_yaw = true;
            }
        }

        // Desired world direction, rotated back into stick space.
        let mut stick = wrap(dz.atan2(dx) - yaw);

        // Nothing moved, so the direction is blocked and the estimate has
        // nothing to learn from. Sweep the eight compass directions instead of
        // guessing: one of them is open, and the burst that finally moves is
        // also the sample that calibrates the camera. Alternating ninety
        // degrees either side was not enough - wedged against the bonfire,
        // both sides are wall.
        if stalled > 0 {
            stick = wrap((probe as f32) * std::f32::consts::FRAC_PI_4);
        }

        // Shorter steps close in, or the walk paces back and forth over the
        // target. Not much shorter, though: the character turns before it
        // moves, and a burst that ends during the turn covers no ground at all
        // - a 700 ms one, measured, moved nothing after a 90 degree turn.
        let base = if distance < 3.0 { 500 } else { plan.burst.as_millis() as u64 };
        let burst = Duration::from_millis(base.max(grown));

        // The pad's y is up-negative, so the stick vector reaches the world as
        // (x, -y). That is a reflection, and a reflection cannot be absorbed by
        // the rotation the estimate above is made of - getting it wrong walked
        // the character steadily away from the target while the estimate
        // chased its own tail.
        //
        // Measured rather than guessed: stick right produced world angle
        // -164.8 degrees and stick forward -82.2, and forward is +90 from
        // right only under this reading.
        screen::focus(&window)?;
        pad::send(
            1,
            &format!(
                "stick l {:.3} {:.3} {}",
                stick.cos().clamp(-1.0, 1.0),
                (-stick.sin()).clamp(-1.0, 1.0),
                burst.as_millis()
            ),
        )
        .map_err(|e| format!("conta {account}: {e}"))?;

        // The stick has to sit at centre for a moment before the game takes a
        // new direction: without this the first burst of a walk moves and
        // every one after it does nothing, which reads as a wall on all eight
        // sides. It was in the first version of this loop and got lost in a
        // rewrite, and cost an afternoon of blaming the terrain.
        std::thread::sleep(Duration::from_millis(200));

        sent = Some(stick);
        previous = pose;
        steps += 1;
    }
}

/// Reads a sample newer than `after`, or gives up. Everything the walk decides
/// hangs on this: a stale reading is not a slow one, it is a wrong one.
fn fresh(install_dir: &Path, after: u64, timeout: Duration) -> Option<Pose> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(pose) = read(install_dir) {
            if pose.tick > after {
                return Some(pose);
            }
        }
        if Instant::now() >= deadline {
            return None;
        }
        std::thread::sleep(Duration::from_millis(40));
    }
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
