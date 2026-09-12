//! Walking the game's own menus, so a restart ends in the world.
//!
//! Every test here starts by getting a character standing in Majula, and that
//! used to be a human pressing buttons for a minute. The steps are fixed; what
//! is not fixed is how long each one takes, and a script of blind sleeps fails
//! the day the machine is busy — in the worst way, by pressing A at a screen
//! that was not ready and leaving the game somewhere nobody planned.
//!
//! So nothing here waits for a duration. It waits for the **server** to say
//! what happened. Three lines carry the whole state machine:
//!
//! | line | meaning |
//! | --- | --- |
//! | `has logged in as player` | the client left the title screen |
//! | `Renaming connection to '<n>:<name>'` | that character is in the world |
//! | `does not appear to be valid` | the client is holding a dead token |
//!
//! The character's name in the second line is also the only statement that
//! ties a running instance to a save, which is what makes "did the right
//! character load" answerable at all.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use crate::env::Environment;
use crate::game;
use crate::pad;
use crate::paths;
use crate::probe::{self, Where};
use crate::screen::{self, GameWindow};

/// How long to keep pressing before giving up, for each leg of the walk.
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(180);

/// Between presses. Long enough that the game has changed screen, short enough
/// that a missed press costs little.
const PRESS_EVERY: Duration = Duration::from_millis(1800);

/// How often to ask the game where it is. The injector answers twice a second;
/// asking faster only writes files.
const PROBE_EVERY: Duration = Duration::from_secs(3);

/// How long the world can go without the server naming the character before the
/// client counts as playing offline. Loading Majula takes about ten seconds.
const OFFLINE_AFTER: Duration = Duration::from_secs(45);

#[derive(Debug, Clone)]
pub struct Arrival {
    pub character: String,
    pub seconds: f64,
}

/// The server's log, read from wherever it was when we started looking.
///
/// The server draws its columns with bytes that are not valid UTF-8, so this
/// reads bytes and converts leniently. It matters more than it sounds: `grep`
/// decides the file is binary and prints its match to stderr instead of
/// stdout, so a shell oracle built on it looks silent while the line it wants
/// is right there.
struct ServerLog {
    path: PathBuf,
    mark: u64,
}

impl ServerLog {
    fn from_now(path: PathBuf) -> Self {
        let mark = std::fs::metadata(&path).map(|meta| meta.len()).unwrap_or(0);
        Self { path, mark }
    }

    /// Starts looking from here on. Used after handling something, so the
    /// line that was just dealt with does not keep matching.
    fn remark(&mut self) {
        self.mark = std::fs::metadata(&self.path).map(|meta| meta.len()).unwrap_or(0);
    }

    fn since_mark(&self) -> String {
        let Ok(bytes) = std::fs::read(&self.path) else {
            return String::new();
        };
        // A restart truncates the file; then everything in it is new.
        let start = if bytes.len() as u64 >= self.mark {
            self.mark as usize
        } else {
            0
        };
        String::from_utf8_lossy(&bytes[start..]).into_owned()
    }
}

/// The character named in a `Renaming connection to '1:Samuel'` line.
///
/// The server renames a connection twice: first to the steam id, then to
/// `<player>:<character>` once the save is loaded. Only the second carries a
/// colon inside the quotes, and only the second means "in the world".
fn character_loaded(text: &str) -> Option<String> {
    text.lines()
        .filter_map(|line| line.split("Renaming connection to '").nth(1))
        .filter_map(|rest| rest.split('\'').next())
        .filter_map(|name| name.split_once(':'))
        .map(|(_player, character)| character.to_owned())
        .next_back()
}

fn logged_in(text: &str) -> bool {
    text.contains("has logged in as player")
}

fn stale_token(text: &str) -> bool {
    text.contains("does not appear to be valid")
}

/// Whether the log ends with somebody in the world rather than disconnected.
fn anyone_in_world(text: &str) -> bool {
    let mut inside = false;
    for line in text.lines() {
        if line.contains("Client disconnected") {
            inside = false;
        } else if line
            .split("Renaming connection to '")
            .nth(1)
            .and_then(|rest| rest.split('\'').next())
            .map(|name| name.contains(':'))
            .unwrap_or(false)
        {
            inside = true;
        }
    }
    inside
}

/// The window belonging to one account.
///
/// By the owning process, not by position: the two instances put their windows
/// on screen in whatever order they finish booting, and "the second window" has
/// meant the wrong account more than once. Position stays as a fallback for a
/// window that does not publish its pid.
fn window_for(environment: &Environment, account: u8) -> Result<GameWindow, String> {
    let windows = screen::windows()?;

    if let Ok(prefix) = game::compat_data(environment, account) {
        let pids = game::instance_pids(&prefix);
        if let Some(window) = windows
            .iter()
            .find(|window| window.pid.is_some_and(|pid| pids.contains(&pid)))
        {
            return Ok(window.clone());
        }
        // Its processes are there but no window yet: it is still booting. Say
        // so and wait, rather than falling through to "the first window", which
        // during a two instance start is the *other* account — and then the
        // harness drives the wrong game and loads the wrong character.
        if !pids.is_empty() {
            return Err(format!("a conta {account} ainda não abriu a janela"));
        }
    }

    // Nothing of ours is running for that account: fall back to position, which
    // is what `game focus` has always meant.
    windows
        .get(usize::from(account).saturating_sub(1))
        .cloned()
        .ok_or_else(|| {
            format!(
                "a conta {account} não tem janela ({} aberta(s)); o jogo está rodando?",
                windows.len()
            )
        })
}

/// Waits for an instance's window to exist.
///
/// A launch returns as soon as Proton is running; the window shows up half a
/// minute later. Waiting here is what lets `up` start both instances at once
/// and drive them as they arrive, instead of booting one, driving it, and only
/// then booting the other.
fn wait_for_window(
    environment: &Environment,
    account: u8,
    timeout: Duration,
) -> Result<GameWindow, String> {
    let deadline = Instant::now() + timeout;
    loop {
        match window_for(environment, account) {
            Ok(window) => return Ok(window),
            Err(error) if Instant::now() >= deadline => return Err(error),
            Err(_) => std::thread::sleep(Duration::from_millis(500)),
        }
    }
}

/// Where one account's game is, asked of the game itself.
pub fn locate(environment: &Environment, account: u8) -> Where {
    match environment
        .installs
        .iter()
        .find(|install| install.account == account)
    {
        Some(install) => probe::locate(&install.game_dir, Duration::from_secs(4)),
        None => Where::Unknown,
    }
}

fn press(window: &GameWindow, command: &str) -> Result<(), String> {
    // Focus first, every time. The game ignores the pad while it is not the
    // active window, and anything at all can take the focus between presses:
    // a browser, another terminal, the second instance coming up.
    screen::focus(window)?;
    pad::send(1, command).map(|_| ())
}

/// Takes an instance from wherever it is on the title screen into the world.
///
/// The walk is: START at the title, then A through whatever the menu puts in
/// the way — the announcement, `Continue`, the save list. Pressing A is safe on
/// every one of those and does nothing on the loading screens between them,
/// which is why this can press without knowing exactly which screen is up. It
/// stops the moment the server says a character loaded.
pub fn enter(
    environment: &Environment,
    instance: u8,
    expect: Option<&str>,
    timeout: Duration,
) -> Result<Arrival, String> {
    if !pad::running(1) {
        return Err("o pad não está rodando; `ds2os-dev pad start`".into());
    }
    let window = wait_for_window(environment, instance, Duration::from_secs(150))?;
    let mut log = ServerLog::from_now(paths::server_log());
    let started = Instant::now();
    let mut last_press = started - PRESS_EVERY;
    let mut recoveries = 0;
    let mut presses = 0u32;
    let mut left_title_at: Option<Instant> = None;
    let mut last_probe = Instant::now() - PROBE_EVERY;

    loop {
        let text = log.since_mark();

        if last_probe.elapsed() >= PROBE_EVERY {
            let position = locate(environment, instance);
            last_probe = Instant::now();
            match position {
                Where::World if left_title_at.is_none() => left_title_at = Some(Instant::now()),
                Where::Title => left_title_at = None,
                _ => {}
            }
        }

        if let Some(character) = character_loaded(&text) {
            let arrival = Arrival {
                character,
                seconds: started.elapsed().as_secs_f64(),
            };
            if let Some(expected) = expect {
                if !arrival.character.eq_ignore_ascii_case(expected) {
                    return Err(format!(
                        "carregou {} e não {expected}. O save que o jogo abre é o último \
                         jogado daquela conta, então confira a lista antes",
                        arrival.character
                    ));
                }
            }
            return Ok(arrival);
        }

        // Loading, or already standing in the world. Pressing A here would be
        // pressing A at a character in a field, so it stops: from this point on
        // the only thing left to wait for is the server naming the character.
        // If that never comes, the client got in without the server — the game
        // happily plays offline — and the way back is the title screen, where
        // it asks for a new login.
        if let Some(since) = left_title_at {
            if since.elapsed() > OFFLINE_AFTER {
                if recoveries >= 2 {
                    return Err(
                        "o jogo entrou no mundo sem o servidor duas vezes seguidas; \
                         veja o log do servidor"
                            .into(),
                    );
                }
                recoveries += 1;
                leave_now(environment, instance, Duration::from_secs(90), false)?;
                log.remark();
                left_title_at = None;
                last_probe = Instant::now() - PROBE_EVERY;
                continue;
            }
            std::thread::sleep(Duration::from_millis(300));
            continue;
        }

        if stale_token(&text) {
            // The client logged in before the server restarted and is holding a
            // token nothing recognises. It only asks for a new one on its way
            // *into* the title screen, so it has to be sent back there: B backs
            // out of the menu, the game re-initialises online mode, and the next
            // login is a fresh one. Twice is enough; a third means something
            // else is wrong and guessing louder will not fix it.
            if recoveries >= 2 {
                return Err(
                    "o servidor segue recusando o token deste cliente. Reinicie o servidor \
                     ANTES de sair para o título, ou feche e abra o jogo"
                        .into(),
                );
            }
            recoveries += 1;
            press(&window, "press b")?;
            std::thread::sleep(Duration::from_millis(800));
            press(&window, "press b")?;
            log.remark();
            last_press = Instant::now();
            std::thread::sleep(Duration::from_secs(2));
            continue;
        }

        if started.elapsed() > timeout {
            return Err(format!(
                "a instância {instance} não chegou ao mundo em {}s; \
                 veja `ds2os-dev game shot` e o log do servidor",
                timeout.as_secs()
            ));
        }

        if last_press.elapsed() >= PRESS_EVERY {
            // A for everything once the login line says the title screen is
            // behind us. Before that, alternate: the title wants START, and a
            // dialog in the way — "the connection to the game server was lost"
            // is the usual one — wants A on its OK button.
            let button = if logged_in(&text) {
                "press a"
            } else if presses % 2 == 0 {
                "press start"
            } else {
                "press a"
            };
            press(&window, button)?;
            presses += 1;
            last_press = Instant::now();
        }

        std::thread::sleep(Duration::from_millis(300));
    }
}

/// Saves and quits to the title screen, leaving the process running.
///
/// This is the cheap half of a server restart: the game keeps its window, its
/// prefix and its loaded data, and the client logs in again on its way back to
/// the title. Relaunching the process costs about a minute; this costs about
/// twenty seconds.
///
/// The walk is the system tab of the pause menu: START, RB five times to the
/// gear, down twice to `Quit Game`, A, then left to YES and A.
pub fn leave(
    environment: &Environment,
    instance: u8,
    timeout: Duration,
) -> Result<f64, String> {
    match locate(environment, instance) {
        Where::Title => {
            return Err(format!(
                "a instância {instance} já está no título; sair do menu só faz sentido \
                 a partir do mundo"
            ))
        }
        Where::World => {}
        // No answer from the injector: fall back to what the server saw, which
        // is right whenever the client is actually connected.
        Where::Unknown if !in_world() => {
            return Err(format!(
                "não consegui perguntar à instância {instance} onde ela está, e o servidor \
                 não tem ninguém no mundo"
            ))
        }
        Where::Unknown => {}
    }
    leave_now(environment, instance, timeout, false)
}

/// Whether the server's log ends with somebody in the world.
///
/// Worth reading **before** restarting the server: the log is recreated on
/// every start, so afterwards it can only say "nobody", however full the world
/// was a second earlier.
pub fn in_world() -> bool {
    let whole = std::fs::read(paths::server_log())
        .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
        .unwrap_or_default();
    anyone_in_world(&whole)
}

/// The same walk, for a caller that already knows the game is in the world.
pub fn leave_now(
    environment: &Environment,
    instance: u8,
    timeout: Duration,
    dismiss_first: bool,
) -> Result<f64, String> {
    if !pad::running(1) {
        return Err("o pad não está rodando; `ds2os-dev pad start`".into());
    }
    let window = window_for(environment, instance)?;
    let started = Instant::now();

    // The pause menu takes a moment to open; the rest is quick. These are the
    // only fixed waits in the module, and they are short enough that a missed
    // step shows up as a timeout below rather than as a wrong button.
    let walk: [(&str, u64); 11] = [
        ("press start", 900),
        ("press rb", 250),
        ("press rb", 250),
        ("press rb", 250),
        ("press rb", 250),
        ("press rb", 400),
        ("dpad down", 250),
        ("dpad down", 350),
        ("press a", 700),
        ("dpad left", 350),
        ("press a", 0),
    ];

    // Twice at most. A dialog in the way eats the whole walk — "lost connection
    // to game server, switching to offline mode" is the one that turns up here,
    // because taking the server down is how a reload starts. Its OK button
    // wants A, so the second attempt clears it first. A is not pressed before
    // the first attempt: with no dialog up that is the interact button, and
    // pressing it in the world is how a harness ends up talking to a merchant.
    for attempt in 0..2 {
        if attempt > 0 || dismiss_first {
            press(&window, "press a")?;
            std::thread::sleep(Duration::from_millis(900));
        }
        for (command, gap) in walk {
            press(&window, command)?;
            if gap > 0 {
                std::thread::sleep(Duration::from_millis(gap));
            }
        }
        let waited = Instant::now();
        while waited.elapsed() < Duration::from_secs(20) {
            if locate(environment, instance) == Where::Title {
                return Ok(started.elapsed().as_secs_f64());
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    }

    // Wait on the game, not on the server. The obvious signal is the server
    // logging the client disconnecting — but after a server restart there is
    // nothing to disconnect from, no such line is ever written, and the wait
    // runs to its timeout while the game is sitting at the title screen it was
    // asked to reach. The title flag says so whether or not a server exists.
    loop {
        if locate(environment, instance) == Where::Title {
            return Ok(started.elapsed().as_secs_f64());
        }
        if started.elapsed() > timeout {
            return Err(format!(
                "a instância {instance} não voltou ao título em {}s",
                timeout.as_secs()
            ));
        }
        std::thread::sleep(Duration::from_millis(500));
    }
}

/// Which accounts have a game window right now.
pub fn open_instances(environment: &Environment) -> Vec<u8> {
    [1u8, 2]
        .into_iter()
        .filter(|account| window_for(environment, *account).is_ok())
        .collect()
}

/// Reads a character name for an instance out of the harness config.
pub fn expected_character(settings: &crate::settings::HarnessConfig, instance: u8) -> Option<String> {
    settings.characters.get(&instance).cloned()
}
