//! ds2os-dev - debug harness for a local Dark Souls II server.
//!
//! Brings up the local DS3OS server, configures the game to talk to it, and
//! runs the second instance so PvP can be tested with a single copy of the
//! game. It deliberately has nothing to do with the public master server: this
//! is for one machine, one server, two clients.
//!
//! Instance 1 belongs to Steam. The harness configures it and notices when it
//! appears, but Steam is what starts it.

mod api;
mod drive;
mod env;
mod game;
mod logs;
mod nav;
mod pad;
mod paths;
mod probe;
mod proc;
mod screen;
mod server;
mod settings;
mod watch;

use std::path::PathBuf;

use clap::{Parser, Subcommand, ValueEnum};
use serde::Serialize;

use env::Environment;
use probe::Where;

#[derive(Parser)]
#[command(
    name = "ds2os-dev",
    about = "Sobe o servidor local, configura o jogo e roda as duas instâncias",
    version
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Checks the environment and reports everything that is missing
    Doctor {
        #[arg(long)]
        json: bool,
    },
    /// Server, game and second instance, in one go
    Up {
        /// Value written to the phantom session timer
        #[arg(long, default_value_t = 4000.0)]
        timer_seconds: f64,
        /// Leave the phantom timer patch off
        #[arg(long)]
        no_timer: bool,
        /// Turn on the exploratory probe that locates the area id in memory
        #[arg(long)]
        probe_area: bool,
        /// Start the games but leave them at the title screen
        #[arg(long)]
        no_enter: bool,
        /// Leave the barrier that pens a phantom into the host's area
        #[arg(long)]
        keep_fog: bool,
        /// Leave the closed areas closed, the way retail has them
        #[arg(long)]
        no_force_zone: bool,
        /// Summon the pair's red sign again from the host, after a duel
        #[arg(long)]
        auto_rematch: bool,
        /// Death in another world goes to the last bonfire, not home
        #[arg(long)]
        seamless: bool,
    },
    /// Stops the server and the second instance
    Down,
    /// Restarts the server and puts every open instance back in the world
    ///
    /// The game is not closed: it saves, drops to the title screen and comes
    /// back. Use it for a server change; a new injector still needs a real
    /// relaunch, because the DLL is only read when the process starts.
    Reload,
    /// Who the server has connected, and everything it knows about them
    Players,
    /// Watches for anything that would silently ruin a test, above all a death
    Watch {
        /// How long to watch, in seconds
        #[arg(long, default_value_t = 120)]
        seconds: u64,
    },
    /// Where a character is standing, as the game itself sees it
    Where {
        /// 1, 2, or both
        #[arg(long, default_value = "both")]
        instance: String,
    },
    /// Walks a character to a place, without anyone watching the screen
    ///
    /// The target is either a coordinate pair (`--to x,z`) or another
    /// instance's current position (`--to-instance 2`), which is what "go and
    /// stand on their summon sign" means in practice.
    Goto {
        /// Which character walks
        #[arg(long)]
        instance: u8,
        /// World coordinates, as `x,z`
        #[arg(long, conflicts_with = "to_instance")]
        to: Option<String>,
        /// Walk to where this instance is standing right now
        #[arg(long)]
        to_instance: Option<u8>,
        /// Close enough, in metres
        #[arg(long, default_value_t = 2.0)]
        radius: f32,
        /// Give up after this many seconds
        #[arg(long, default_value_t = 90)]
        seconds: u64,
    },
    /// One screen of what is running
    Status {
        #[arg(long)]
        json: bool,
    },
    /// The local server on its own
    Server {
        #[command(subcommand)]
        action: ServerAction,
    },
    /// The game directory and the instances
    Game {
        #[command(subcommand)]
        action: GameAction,
    },
    /// The virtual gamepad the harness drives the game with
    Pad {
        #[command(subcommand)]
        action: PadAction,
    },
    /// The second Steam client, which gives instance 2 its own account
    Steam2 {
        #[command(subcommand)]
        action: Steam2Action,
    },
    /// Reads any of the logs, sanitised and greppable
    Logs {
        #[arg(value_enum, default_value_t = LogName::Server)]
        which: LogName,
        /// How many matching lines to show
        #[arg(short = 'n', long, default_value_t = 40)]
        lines: usize,
        /// Only lines containing this, case insensitive
        #[arg(short = 'g', long)]
        grep: Option<String>,
        /// Keep printing as the log grows
        #[arg(short = 'f', long)]
        follow: bool,
    },
}

#[derive(Subcommand)]
enum PadAction {
    /// Creates the device and serves commands until stopped
    Start {
        #[arg(long, default_value_t = 1)]
        index: u8,
        /// Stay in the foreground instead of detaching
        #[arg(long)]
        foreground: bool,
    },
    /// Stops the daemon and removes the device
    Stop {
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Presses a button: a b x y lb rb back start guide l3 r3
    Press {
        button: String,
        /// How long to hold it, in milliseconds
        #[arg(long, default_value_t = 90)]
        ms: u64,
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Presses a direction on the d-pad: up down left right
    Dpad {
        direction: String,
        #[arg(long, default_value_t = 90)]
        ms: u64,
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Holds a trigger: lt or rt
    Trigger {
        side: String,
        #[arg(long, default_value_t = 90)]
        ms: u64,
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Holds a stick away from centre, then releases it
    #[command(allow_negative_numbers = true)]
    Stick {
        /// l or r
        side: String,
        /// -1.0 to 1.0
        x: f32,
        /// -1.0 to 1.0, negative is up
        y: f32,
        #[arg(long, default_value_t = 400)]
        ms: u64,
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Runs several inputs in one go, so a menu walk costs one command
    ///
    /// Steps are separated by ";" and are the same verbs used above, plus
    /// "wait <ms>". Example:
    ///   pad seq "press start; wait 900; dpad right; wait 250; press a"
    #[command(allow_negative_numbers = true)]
    Seq {
        script: String,
        /// Focus this instance's window first
        #[arg(long)]
        focus: Option<usize>,
        /// Capture both windows when the sequence ends
        #[arg(long)]
        shot: bool,
        /// Pause inserted between steps that do not say otherwise
        #[arg(long, default_value_t = 250)]
        gap: u64,
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
    /// Says whether the device is up
    Status {
        #[arg(long, default_value_t = 1)]
        index: u8,
    },
}

#[derive(Subcommand)]
enum Steam2Action {
    /// Creates the home for a second Steam client and explains the next steps
    Init {
        /// Where the second client keeps its files
        #[arg(long, default_value = "~/steam2")]
        home: String,
    },
    /// Starts the second Steam client so the other account can log in
    Run,
    /// Shows what is configured and whether that client is usable
    Show,
}

#[derive(Subcommand)]
enum ServerAction {
    /// Starts the server, fixing its config first
    Up,
    /// Stops the server
    Down,
    /// Stops and starts it
    Restart,
    /// Shows the server's state
    Status,
}

#[derive(Subcommand)]
enum GameAction {
    /// Writes Injector.config, the wrapper and copies the injector binaries
    Prepare {
        #[arg(long, default_value_t = 4000.0)]
        timer_seconds: f64,
        #[arg(long)]
        no_timer: bool,
        /// Turn on the exploratory probe that locates the area id in memory
        #[arg(long)]
        probe_area: bool,
        /// With the probe on, report which instructions read that address
        #[arg(long)]
        watch_reads: bool,
        /// Skip the scan and watch this address, e.g. 0x7ffff03a6f90
        #[arg(long)]
        area_address: Option<String>,
        /// Report the multiplay zone the player is in and what it permits
        #[arg(long)]
        probe_zone: bool,
        /// Allow summoning anywhere by always reporting a real multiplay zone
        #[arg(long)]
        force_zone: bool,
        /// Build the fog walls as if the player owned the world, which is the
        /// experiment for a phantom's area barrier
        #[arg(long)]
        remove_fog: bool,
        /// Summon the pair's red sign again from the host, after a duel
        #[arg(long)]
        auto_rematch: bool,
        /// Death in another world goes to the last bonfire, not home
        #[arg(long)]
        seamless: bool,
    },
    /// Starts the game, in its own Proton prefix, without Steam
    Launch {
        /// Home of the second Steam client; defaults to what steam2 saved
        #[arg(long)]
        steam_home: Option<PathBuf>,
        /// 1, 2, or both
        #[arg(long, default_value = "both")]
        instance: String,
    },
    /// Stops an instance and waits until its prefix is free
    Stop {
        /// 1, 2, or both
        #[arg(long, default_value = "both")]
        instance: String,
    },
    /// Walks one instance from the title screen into the world
    Enter {
        /// 1 or 2, in the order the windows are listed
        #[arg(long, default_value_t = 1)]
        instance: u8,
        /// Fail unless this character is the one that loads
        #[arg(long)]
        character: Option<String>,
    },
    /// Which character an instance should load, so a wrong save is noticed
    Character {
        /// 1 or 2
        #[arg(long)]
        instance: u8,
        /// The character's name; leave it out to see what is remembered
        name: Option<String>,
    },
    /// Saves and quits to the title screen, leaving the game running
    Leave {
        /// 1 or 2, in the order the windows are listed
        #[arg(long, default_value_t = 1)]
        instance: u8,
    },
    /// Prints the line to paste into Steam's launch options
    Options,
    /// Asks the injector to watch the area address for a few seconds
    Watch {
        /// 1 or 2, in the order the windows are listed
        #[arg(long, default_value_t = 1)]
        account: u8,
    },
    /// Brings one instance's window to the front so it receives input
    Focus {
        /// 1 or 2, in the order the windows are listed
        window: usize,
    },
    /// Captures each game window to a PNG
    Shot {
        /// Where to write them; defaults to the harness log directory
        #[arg(long)]
        out: Option<PathBuf>,
    },
}

#[derive(Copy, Clone, PartialEq, Eq, ValueEnum)]
enum LogName {
    /// The DS3OS server
    Server,
    /// The second game instance, as Proton sees it
    Instance2,
    /// What the injector wrote inside the game
    Injector,
    /// The phantom timer patch
    Timer,
    /// Every ds2os-dev command that was run
    Cli,
}

use settings::HarnessConfig;

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Status {
    environment: Environment,
    problems: Vec<env::Problem>,
    server: server::ServerStatus,
    instances: game::InstancesStatus,
    logs: Vec<String>,
}

fn main() {
    let cli = Cli::parse();
    let _ = paths::ensure_dirs();
    record_invocation();

    let code = match run(cli.command) {
        Ok(()) => 0,
        Err(message) => {
            eprintln!("erro: {message}");
            1
        }
    };
    std::process::exit(code);
}

/// Appends the command line to cli.log. When someone reports that nothing
/// worked, the first useful question is what they actually ran.
fn record_invocation() {
    use std::io::Write;
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(paths::cli_log())
    {
        let _ = writeln!(file, "{}  ds2os-dev {}", timestamp(), args.join(" "));
    }
}

fn timestamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // Seconds since the epoch is enough to correlate with the other logs and
    // costs no dependency.
    format!("t={now}")
}

fn run(command: Command) -> Result<(), String> {
    let environment = Environment::resolve();

    match command {
        Command::Doctor { json } => doctor(&environment, json),
        Command::Up { timer_seconds, no_timer, probe_area, no_enter, no_force_zone, keep_fog, auto_rematch, seamless } => {
            up(&environment, timer_seconds, !no_timer, probe_area, no_enter, !no_force_zone, !keep_fog, auto_rematch, seamless)
        }
        Command::Down => {
            let stopped_game = game::stop_second();
            let stopped_server = server::down();
            println!("  segunda instância: {}", yes_no(stopped_game));
            println!("  servidor:          {}", yes_no(stopped_server));
            Ok(())
        }
        Command::Reload => reload(&environment),
        Command::Players => players(&environment),
        Command::Watch { seconds } => {
            println!("  olhando por {seconds}s; só fala quando algo muda");
            watch::run(
                &environment,
                std::time::Duration::from_secs(seconds),
                |event| println!("  {} {}", event.at, event.what),
            )
        }
        Command::Where { instance } => where_is(&environment, &instance),
        Command::Goto { instance, to, to_instance, radius, seconds } => {
            goto(&environment, instance, to, to_instance, radius, seconds)
        }
        Command::Status { json } => status(&environment, json),
        Command::Server { action } => match action {
            ServerAction::Up => {
                let status = server::up(&environment)?;
                print_server(&status);
                Ok(())
            }
            ServerAction::Down => {
                println!("  parado: {}", yes_no(server::down()));
                Ok(())
            }
            ServerAction::Restart => {
                // A client that is in the world reconnects afterwards with the
                // token it already had, which the server now keeps across a
                // restart — so it is let in, but its packet stream cannot
                // resume. It then looks connected and counts as a player while
                // nothing it sends arrives. Saying so here is cheap; finding it
                // out later, from a test that quietly reports nothing, is not.
                for instance in drive::open_instances(&environment) {
                    if drive::locate(&environment, instance) == Where::World {
                        println!(
                            "  atenção: a instância {instance} está no mundo. Reiniciar assim \
                             deixa a conexão dela quebrada em silêncio; `ds2os-dev reload` sai \
                             para o título antes e volta depois"
                        );
                    }
                }
                server::down();
                let status = server::up(&environment)?;
                print_server(&status);
                Ok(())
            }
            ServerAction::Status => {
                print_server(&server::status(&environment));
                Ok(())
            }
        },
        Command::Game { action } => match action {
            GameAction::Prepare { timer_seconds, no_timer, probe_area, watch_reads, area_address, probe_zone, force_zone, remove_fog, auto_rematch, seamless } => {
                // The timer patch installs its own exception handler and single
                // steps through a software breakpoint. Two handlers competing
                // for the same exception would muddy what the watch reports, so
                // the patch stays off while probing.
                let timer = !no_timer && !watch_reads;
                if watch_reads && !no_timer {
                    println!("  timer desligado enquanto o watch estiver ligado");
                }
                prepare(&environment, timer_seconds, timer, probe_area || watch_reads, watch_reads, area_address, probe_zone, force_zone, remove_fog, auto_rematch, seamless)
            }
            GameAction::Launch { steam_home, instance } => {
                let home = resolve_second_steam(steam_home)?;
                for account in accounts(&instance)? {
                    if account == 2 {
                        announce_account(home.as_deref());
                    }
                    let pid = game::launch(&environment, account, home.as_deref())?;
                    println!("  instância {account} iniciada, pid {pid}");
                    println!("    log: {}", paths::instance_log(account).display());
                }
                Ok(())
            }
            GameAction::Stop { instance } => {
                for account in accounts(&instance)? {
                    match game::stop_instance(&environment, account)? {
                        0 => println!("  instância {account}: já estava parada"),
                        n => println!("  instância {account}: {n} processo(s) encerrado(s)"),
                    }
                }
                Ok(())
            }
            GameAction::Enter { instance, character } => {
                let expected = character
                    .or_else(|| drive::expected_character(&HarnessConfig::load(), instance));
                let arrival =
                    drive::enter(&environment, instance, expected.as_deref(), drive::DEFAULT_TIMEOUT)?;
                println!(
                    "  instância {instance}: {} no mundo em {:.0}s",
                    arrival.character, arrival.seconds
                );
                Ok(())
            }
            GameAction::Character { instance, name } => {
                let mut settings = HarnessConfig::load();
                if let Some(name) = name {
                    settings.characters.insert(instance, name.clone());
                    settings.save()?;
                    println!("  instância {instance}: {name}");
                } else if settings.characters.is_empty() {
                    println!("  nenhum personagem anotado");
                    println!("  anote com: ds2os-dev game character --instance 1 Samuel");
                } else {
                    for (instance, name) in &settings.characters {
                        println!("  instância {instance}: {name}");
                    }
                }
                Ok(())
            }
            GameAction::Leave { instance } => {
                let seconds = drive::leave(&environment, instance, drive::DEFAULT_TIMEOUT)?;
                println!("  instância {instance}: de volta ao título em {seconds:.0}s");
                Ok(())
            }
            GameAction::Options => {
                print_launch_options(&environment);
                Ok(())
            }
            GameAction::Watch { account } => {
                let install = environment
                    .installs
                    .iter()
                    .find(|i| i.account == account)
                    .ok_or_else(|| format!("conta {account} não encontrada"))?;
                let trigger = install.game_dir.join("DS2_AreaWatch.trigger");
                std::fs::write(&trigger, b"")
                    .map_err(|e| format!("não consegui criar {}: {e}", trigger.display()))?;
                println!("  dump da estrutura pedido; sai no DS2_AreaProbe.log");
                Ok(())
            }
            GameAction::Shot { out } => shot(&environment, out),
            GameAction::Focus { window } => {
                let target = focus_target(&environment, window)?;
                screen::focus(&target)?;
                println!("  foco em {} ({})", target.id, window);
                Ok(())
            }
        },
        Command::Pad { action } => pad_command(&environment, action),
        Command::Steam2 { action } => steam2(action),
        Command::Logs { which, lines, grep, follow } => {
            let path = log_path(&environment, which)
                .ok_or("esse log não existe neste ambiente")?;
            logs::show(&path, &logs::Options { lines, grep: grep.as_deref(), follow })
                .map_err(|e| e.to_string())
        }
    }
}

/// Turns `1`, `2` or `both` into the accounts to act on.
fn accounts(choice: &str) -> Result<Vec<u8>, String> {
    match choice.trim() {
        "1" => Ok(vec![1]),
        "2" => Ok(vec![2]),
        "both" | "ambas" | "all" => Ok(vec![1, 2]),
        other => Err(format!("instância inválida: {other} (use 1, 2 ou both)")),
    }
}

/// Restarts the server and walks every open instance back into the world.
///
/// The order is the whole point. A client that logged in before the restart is
/// holding a token the new server has never seen, and the game only asks for a
/// new one on its way **into** the title screen. Quitting to the title first
/// and restarting after leaves the client authenticated against a server that
/// no longer exists: it then refuses to enter the world, retries for a minute
/// and drops back to the title with "connection to the game server was lost".
/// So: restart, then quit to title, then come back.
fn reload(environment: &Environment) -> Result<(), String> {
    let instances = drive::open_instances(environment);
    if instances.is_empty() {
        println!("servidor");
        server::down();
        let status = server::up(environment)?;
        print_server(&status);
        println!("\n  nenhuma instância aberta");
        return Ok(());
    }

    // Ask each game where it is, before anything moves. The server's log cannot
    // answer this: it is recreated on every start, and a client that lost its
    // session plays on offline, which the server never sees at all.
    let playing: Vec<u8> = instances
        .iter()
        .copied()
        .filter(|account| drive::locate(environment, *account) == Where::World)
        .collect();

    // Out of the world first, then restart, then back in. The client asks for a
    // session on its way *into* the title screen, so leaving first means it is
    // already holding a valid one when the server comes back — and the server
    // keeps its tokens across a restart (`PersistAuthTokens`), so that session
    // is still good. Restarting first instead means the client is refused,
    // retries for a minute, and drops out with "the connection to the game
    // server was lost".
    if !playing.is_empty() {
        println!("saindo para o título");
        for instance in &playing {
            match drive::leave_now(environment, *instance, drive::DEFAULT_TIMEOUT, false) {
                Ok(seconds) => println!("  instância {instance}: título em {seconds:.0}s"),
                Err(error) => println!("  instância {instance}: {error}"),
            }
        }
        println!();
    }

    println!("servidor");
    server::down();
    let status = server::up(environment)?;
    print_server(&status);

    println!("\nvoltando ao mundo");
    let settings = HarnessConfig::load();
    let mut failure = None;
    for instance in &instances {
        let expected = drive::expected_character(&settings, *instance);
        match drive::enter(environment, *instance, expected.as_deref(), drive::DEFAULT_TIMEOUT) {
            Ok(arrival) => println!(
                "  instância {instance}: {} no mundo em {:.0}s",
                arrival.character, arrival.seconds
            ),
            Err(error) => {
                println!("  instância {instance}: {error}");
                failure.get_or_insert(error);
            }
        }
    }
    match failure {
        Some(error) => Err(error),
        None => Ok(()),
    }
}

/// Expands a leading ~ so `--home ~/steam2` works from any shell.
fn expand_home(raw: &str) -> PathBuf {
    match raw.strip_prefix("~/") {
        Some(rest) => paths::home().join(rest),
        None => PathBuf::from(raw),
    }
}

/// The second Steam home to use: the flag if given, otherwise what was saved.
fn resolve_second_steam(flag: Option<PathBuf>) -> Result<Option<PathBuf>, String> {
    if let Some(home) = flag {
        return Ok(Some(home));
    }
    Ok(HarnessConfig::load().second_steam_home)
}

fn announce_account(home: Option<&std::path::Path>) {
    match home {
        Some(home) => println!("  conta: segunda Steam em {}", home.display()),
        None => println!(
            "  conta: a mesma da primeira instância — as duas não vão conseguir se conectar\n             \x20        rode `ds2os-dev steam2 init` para usar a segunda conta"
        ),
    }
}

fn pad_command(environment: &Environment, action: PadAction) -> Result<(), String> {
    let report = |index: u8, command: String| -> Result<(), String> {
        pad::send(index, &command).map(|_| println!("  ok"))
    };

    match action {
        PadAction::Start { index, foreground } => {
            if pad::running(index) {
                println!("  o pad {index} já está rodando");
                return Ok(());
            }
            if foreground {
                return pad::serve(index);
            }
            // Detached, so the device outlives this invocation. Without that
            // the game would see a controller appear and vanish per command.
            let exe = std::env::current_exe().map_err(|e| e.to_string())?;
            let log = paths::log_dir().join(format!("pad-{index}.log"));
            crate::proc::spawn(
                &exe,
                &["pad", "start", "--foreground", "--index", &index.to_string()],
                &paths::state_dir(),
                &[],
                &log,
                &paths::state_dir().join(format!("pad-{index}.pid")),
                false,
            )
            .map_err(|e| format!("não consegui iniciar o pad: {e}"))?;

            for _ in 0..40 {
                if pad::running(index) {
                    println!("  pad {index} no ar");
                    println!("  log {}", log.display());
                    return Ok(());
                }
                std::thread::sleep(std::time::Duration::from_millis(150));
            }
            Err(format!("o pad {index} não subiu; veja {}", log.display()))
        }
        PadAction::Stop { index } => {
            if !pad::running(index) {
                println!("  o pad {index} não está rodando");
                return Ok(());
            }
            pad::send(index, "quit").map(|_| println!("  parado"))
        }
        PadAction::Press { button, ms, index } => report(index, format!("press {button} {ms}")),
        PadAction::Dpad { direction, ms, index } => report(index, format!("dpad {direction} {ms}")),
        PadAction::Trigger { side, ms, index } => report(index, format!("trigger {side} {ms}")),
        PadAction::Stick { side, x, y, ms, index } => {
            report(index, format!("stick {side} {x} {y} {ms}"))
        }
        PadAction::Seq { script, focus, shot, gap, index } => {
            if let Some(which) = focus {
                let target = focus_target(environment, which)?;
                screen::focus(&target)?;
                println!("  foco em {}", target.id);
            }

            for step in script.split(';').map(str::trim).filter(|s| !s.is_empty()) {
                let parts: Vec<&str> = step.split_whitespace().collect();
                // A wait is handled here rather than in the daemon, so the
                // device is never held open doing nothing.
                if let ["wait", ms] = parts.as_slice() {
                    let ms: u64 = ms.parse().map_err(|_| format!("espera inválida: {ms}"))?;
                    std::thread::sleep(std::time::Duration::from_millis(ms));
                    println!("    esperei {ms}ms");
                    continue;
                }

                pad::send(index, step).map_err(|e| format!("passo \"{step}\": {e}"))?;
                println!("    {step}");
                std::thread::sleep(std::time::Duration::from_millis(gap));
            }

            if shot {
                shot_into(environment, None)?;
            }
            Ok(())
        }
        PadAction::Status { index } => {
            println!("  pad {index}: {}", if pad::running(index) { "no ar" } else { "parado" });
            Ok(())
        }
    }
}

fn steam2(action: Steam2Action) -> Result<(), String> {
    match action {
        Steam2Action::Init { home } => {
            let home = expand_home(&home);
            std::fs::create_dir_all(&home)
                .map_err(|e| format!("não consegui criar {}: {e}", home.display()))?;

            let mut config = HarnessConfig::load();
            config.second_steam_home = Some(home.clone());
            config.save()?;

            println!("  home da segunda Steam: {}", home.display());
            println!();
            println!("  agora, uma vez só:");
            println!("    1. ds2os-dev steam2 run");
            println!("    2. entre com a SEGUNDA conta (é um cliente separado, não desloga o seu)");
            println!("    3. na Steam: Configurações → Downloads → Pastas da Biblioteca");
            println!("       adicione /mnt/ssd/SteamLibrary — o DS2 já instalado aparece como instalado");
            println!("    4. feche o jogo se ela tentar abrir; o ds2os-dev é quem lança");
            println!();
            println!("  depois disso, `ds2os-dev game launch` usa essa conta sozinho");
            Ok(())
        }
        Steam2Action::Run => {
            let home = HarnessConfig::load()
                .second_steam_home
                .ok_or("nenhuma segunda Steam configurada; rode `ds2os-dev steam2 init`")?;
            println!("  abrindo a Steam com HOME={}", home.display());
            std::process::Command::new("steam")
                .env("HOME", &home)
                .stdin(std::process::Stdio::null())
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .spawn()
                .map_err(|e| format!("não consegui abrir a Steam: {e}"))?;
            Ok(())
        }
        Steam2Action::Show => {
            let config = HarnessConfig::load();
            match config.second_steam_home {
                None => {
                    println!("  nenhuma segunda Steam configurada");
                    println!("  rode: ds2os-dev steam2 init");
                }
                Some(home) => {
                    println!("  home       {}", home.display());
                    match ds2os_core::steam::Steam::discover_in(&home) {
                        Ok(steam) => {
                            println!("  cliente    {}", steam.root().display());
                            match steam.find_game(ds2os_core::steam::GameType::DarkSouls2) {
                                Some(install) => {
                                    println!("  ds2        {}", install.install_dir.display())
                                }
                                None => println!(
                                    "  ds2        não visível para essa conta — adicione a pasta \
                                     da biblioteca nas configurações dela"
                                ),
                            }
                        }
                        Err(_) => println!("  cliente    ainda não instalado; rode `ds2os-dev steam2 run`"),
                    }
                }
            }
            Ok(())
        }
    }
}

/// The window a `--focus N` or `game focus N` means.
///
/// N names an **instance**, not a position on screen. The two games put their
/// windows up in whatever order they finish booting, so indexing the window
/// list picked the other account about half the time — and the presses that
/// followed drove the wrong game, silently, because both look alike. The
/// lookup by owning process is the same one the unattended walks already use,
/// and it keeps the positional meaning only for a window that publishes no pid.
/// Everything the server knows about who is connected. The client keeps the
/// same facts somewhere in its memory, and each of them is an afternoon to
/// find; the server was told all of it already.
fn players(environment: &Environment) -> Result<(), String> {
    let server = environment
        .server
        .as_ref()
        .ok_or("o servidor não está compilado; rode `ds2os-dev doctor`")?;
    let list = api::players(server, api::web_port(&server.config))?;

    if list.is_empty() {
        println!("  ninguém conectado");
        return Ok(());
    }

    for player in list {
        println!("  {} (id {}, steam {})", player.name, player.player_id, player.steam_id);
        println!(
            "    nível {}  almas {}  soul memory {}",
            player.soul_level, player.souls, player.soul_memory
        );
        println!(
            "    mortes {}  multiplayer {}  covenant {}",
            player.death_count, player.multiplay_count, player.covenant
        );
        println!(
            "    área {}  estado {}  jogando há {}",
            player.location, player.status, player.play_time
        );
    }
    Ok(())
}

fn install_for(environment: &Environment, account: u8) -> Result<&env::Install, String> {
    environment
        .installs
        .iter()
        .find(|i| i.account == account)
        .ok_or_else(|| format!("conta {account} não encontrada"))
}

/// Prints where each character is standing. Useful on its own, and the only
/// way to get the number that `goto --to` wants.
fn where_is(environment: &Environment, instance: &str) -> Result<(), String> {
    let accounts: Vec<u8> = match instance {
        "both" => vec![1, 2],
        other => vec![other
            .parse()
            .map_err(|_| format!("instância inválida: {other}"))?],
    };

    for account in accounts {
        let install = install_for(environment, account)?;
        match nav::read(&install.game_dir) {
            Some(pose) => println!(
                "  conta {account}: x={:.2} y={:.2} z={:.2}  encarando {:.2},{:.2}  papel {}",
                pose.x, pose.y, pose.z, pose.facing_x, pose.facing_z, pose.archetype
            ),
            None => println!("  conta {account}: sem posição (fora do mundo, ou sem injector)"),
        }
    }
    Ok(())
}

fn goto(
    environment: &Environment,
    account: u8,
    to: Option<String>,
    to_instance: Option<u8>,
    radius: f32,
    seconds: u64,
) -> Result<(), String> {
    let install = install_for(environment, account)?;

    let target = match (to, to_instance) {
        (Some(pair), _) => {
            let (x, z) = pair
                .split_once(',')
                .ok_or_else(|| format!("--to quer `x,z`, recebi `{pair}`"))?;
            (
                x.trim().parse::<f32>().map_err(|_| format!("x inválido: {x}"))?,
                z.trim().parse::<f32>().map_err(|_| format!("z inválido: {z}"))?,
            )
        }
        (None, Some(other)) => {
            let their = install_for(environment, other)?;
            let pose = nav::read(&their.game_dir)
                .ok_or_else(|| format!("a conta {other} não está publicando posição"))?;
            println!("  alvo: conta {other} em x={:.2} z={:.2}", pose.x, pose.z);
            (pose.x, pose.z)
        }
        (None, None) => return Err("escolha --to x,z ou --to-instance N".to_owned()),
    };

    let plan = nav::Plan {
        radius,
        timeout: std::time::Duration::from_secs(seconds),
        ..nav::Plan::default()
    };

    let outcome = nav::walk_to(
        environment,
        &install.game_dir,
        account,
        target,
        plan,
        |step, pose, distance| {
            println!("    {step:>3}  x={:.2} z={:.2}  faltam {:.1} m", pose.x, pose.z, distance);
        },
    )?;

    match outcome {
        nav::Outcome::Arrived { steps, distance } => {
            println!("  chegou em {steps} passos, a {distance:.1} m do alvo");
            Ok(())
        }
        nav::Outcome::Stuck { steps, distance } => {
            Err(format!("travou depois de {steps} passos, ainda a {distance:.1} m"))
        }
        nav::Outcome::Fell { steps, drop } => {
            Err(format!("caiu {drop:.1} m no passo {steps}"))
        }
        nav::Outcome::Teleported { steps, jumped } => Err(format!(
            "saltou {jumped:.1} m no passo {steps}: morreu e renasceu, ou algo o teletransportou.              Confira com `ds2os-dev players`"
        )),
        nav::Outcome::LostPlayer { steps } => {
            Err(format!("perdi o jogador no passo {steps}; carregando área?"))
        }
        nav::Outcome::TimedOut { steps, distance } => {
            Err(format!("tempo esgotado em {steps} passos, ainda a {distance:.1} m"))
        }
    }
}

fn focus_target(environment: &Environment, instance: usize) -> Result<screen::GameWindow, String> {
    let account = u8::try_from(instance).map_err(|_| format!("instância {instance} não existe"))?;
    drive::window_for(environment, account)
}

/// Writes one PNG per game window. Naming them by index keeps the paths stable
/// between calls, so a later capture overwrites the earlier one rather than
/// filling the directory.
fn shot(environment: &Environment, out: Option<PathBuf>) -> Result<(), String> {
    shot_into(environment, out)
}

fn shot_into(environment: &Environment, out: Option<PathBuf>) -> Result<(), String> {
    let dir = out.unwrap_or_else(paths::log_dir);
    let windows = screen::windows()?;

    if windows.is_empty() {
        return Err("nenhuma janela do Dark Souls II aberta".into());
    }

    // `shot-1.png` has to be instance 1's screen. Naming the files by the order
    // X happens to list the windows in made them swap places between runs, and
    // a screenshot of the wrong account is worse than no screenshot: it reads
    // as evidence. Windows whose account cannot be resolved keep the old
    // positional names, after the ones that could.
    let mut named: Vec<(String, &screen::GameWindow)> = Vec::new();
    let mut claimed: Vec<String> = Vec::new();
    for account in 1..=2u8 {
        if let Ok(window) = drive::window_for(environment, account) {
            if let Some(found) = windows.iter().find(|other| other.id == window.id) {
                named.push((format!("shot-{account}.png"), found));
                claimed.push(window.id.clone());
            }
        }
    }
    // A window whose account cannot be resolved keeps a positional name, but
    // never one an account already took: the games leave their X windows
    // behind when they are killed, and an old window overwriting `shot-1.png`
    // is a screenshot of a dead game that reads as the live one.
    for (index, window) in windows.iter().enumerate() {
        if claimed.contains(&window.id) {
            continue;
        }
        let mut name = format!("shot-{}.png", index + 1);
        let mut bump = windows.len();
        while named.iter().any(|(taken, _)| *taken == name) {
            bump += 1;
            name = format!("shot-{bump}.png");
        }
        named.push((name, window));
    }

    for (name, window) in named {
        let path = dir.join(&name);
        match screen::capture(window, &path) {
            Ok(written) => println!(
                "  janela {} ({}x{})  {}",
                window.id,
                window.width,
                window.height,
                written.display()
            ),
            Err(error) => println!("  janela {}  falhou: {error}", window.id),
        }
    }
    Ok(())
}

fn log_path(environment: &Environment, which: LogName) -> Option<PathBuf> {
    match which {
        LogName::Server => Some(paths::server_log()),
        LogName::Instance2 => Some(paths::instance_log(2)),
        LogName::Cli => Some(paths::cli_log()),
        LogName::Injector => game::injector_log(environment),
        LogName::Timer => game::timer_log(environment),
    }
}

fn doctor(environment: &Environment, json: bool) -> Result<(), String> {
    let problems = environment.problems();

    if json {
        let payload = serde_json::json!({
            "environment": environment,
            "problems": problems,
            "ok": problems.is_empty(),
        });
        println!("{}", serde_json::to_string_pretty(&payload).map_err(|e| e.to_string())?);
        return Ok(());
    }

    println!("ambiente");
    row("steam", environment.steam_root.as_ref());
    row("jogo", environment.game_dir.as_ref());
    row("executável", environment.game_exe.as_ref());
    row("prefixo proton", environment.game.as_ref().and_then(|g| g.prefix_path.as_ref()));
    row("proton", environment.proton.as_ref());
    row("servidor", environment.server.as_ref().map(|s| &s.binary));
    row("injector", environment.injector_source.as_ref());
    row("estado", Some(&environment.state_dir));

    if problems.is_empty() {
        println!("\ntudo pronto");
        return Ok(());
    }

    println!("\n{} problema(s)", problems.len());
    for problem in &problems {
        println!("  {}", problem.what);
        println!("    → {}", problem.fix);
    }
    Err("o ambiente não está pronto".into())
}

fn row<T: std::fmt::Debug>(label: &str, value: Option<T>) {
    match value {
        Some(value) => println!("  {label:16} {}", format!("{value:?}").trim_matches('"')),
        None => println!("  {label:16} —"),
    }
}

fn prepare(
    environment: &Environment,
    timer_seconds: f64,
    timer_patch: bool,
    probe_area: bool,
    watch_reads: bool,
    area_address: Option<String>,
    probe_zone: bool,
    force_zone: bool,
    remove_fog: bool,
    auto_rematch: bool,
    seamless: bool,
) -> Result<(), String> {
    if environment.installs.is_empty() {
        return Err("nenhuma instalação do Dark Souls II encontrada".into());
    }

    for install in &environment.installs {
        let prepared = game::prepare(environment, install, timer_seconds, timer_patch, probe_area, watch_reads, area_address.clone(), probe_zone, force_zone, remove_fog, auto_rematch, seamless)?;
        println!("  conta {}", prepared.account);
        println!("    pasta   {}", prepared.game_dir.display());
        if !prepared.copied.is_empty() {
            println!("    copiado {}", prepared.copied.join(", "));
        }
        if force_zone {
            println!("    zona    FORCADA para 103110 (multiplayer em qualquer lugar)");
        }
        if probe_zone {
            println!("    zona    ligada (DS2_MultiPlayZone.log na pasta do jogo)");
        }
        if prepared.probe_area {
            println!(
                "    probe   ligado{} (DS2_AreaProbe.log na pasta do jogo)",
                if watch_reads { " + watch de leituras" } else { "" }
            );
        }
        println!(
            "    timer   {}",
            if timer_patch { format!("{timer_seconds:.0}s") } else { "desligado".to_owned() }
        );
    }
    Ok(())
}

/// The launch options line for each account, which the user pastes into that
/// account's own Steam client.
fn print_launch_options(environment: &Environment) {
    for install in &environment.installs {
        let script = install.game_dir.join("ds2os-launch.sh");
        println!("  conta {}: {}", install.account, quoted(&script));
    }
}

fn quoted(path: &std::path::Path) -> String {
    let text = path.to_string_lossy();
    if text.chars().all(|c| c.is_ascii_alphanumeric() || "._-/".contains(c)) {
        format!("{text} %command%")
    } else {
        format!("'{}' %command%", text.replace('\'', r"'\''"))
    }
}

fn up(
    environment: &Environment,
    timer_seconds: f64,
    timer_patch: bool,
    probe_area: bool,
    no_enter: bool,
    force_zone: bool,
    remove_fog: bool,
    auto_rematch: bool,
    seamless: bool,
) -> Result<(), String> {
    let problems = environment.problems();
    if !problems.is_empty() {
        for problem in &problems {
            eprintln!("  {}\n    → {}", problem.what, problem.fix);
        }
        return Err("o ambiente não está pronto; rode `ds2os-dev doctor`".into());
    }

    // The game enumerates controllers once, at startup, and never looks again,
    // so the virtual pad has to exist before anyone presses Play. Getting that
    // order wrong looks exactly like a pad that does not work.
    println!("gamepad");
    if pad::running(1) {
        println!("  já no ar");
    } else {
        pad_command(environment, PadAction::Start { index: 1, foreground: false })?;
    }

    println!("\nservidor");
    let status = server::up(environment)?;
    print_server(&status);

    // Multiplayer in the closed areas is the point of this server, and the
    // config is read when the injector is injected — so a game `up` started
    // without it is a game that has to be closed and opened again. It used to
    // be a flag on `game prepare` because `up` only printed instructions and a
    // human pressed Play; now that `up` launches the game, leaving it off is
    // how a Majula test quietly fails.
    println!("\njogo");
    prepare(environment, timer_seconds, timer_patch, probe_area, false, None, false, force_zone, remove_fog, auto_rematch, seamless)?;

    println!("\ninstâncias");
    if environment.installs.len() < 2 {
        println!("  só uma conta configurada. As duas instâncias precisam de contas Steam");
        println!("  diferentes, senão a sessão PvP entre elas não conecta.");
        println!("  rode: ds2os-dev steam2 init");
    }

    let home = resolve_second_steam(None)?;
    let mut started = Vec::new();
    for install in &environment.installs {
        let account = install.account;
        match game::launch(environment, account, home.as_deref()) {
            Ok(pid) => {
                println!("  conta {account}: pid {pid}");
                started.push(account);
            }
            Err(error) => println!("  conta {account}: {error}"),
        }
    }

    if no_enter || started.is_empty() {
        println!("\n  os jogos ficam no título; `ds2os-dev game enter --instance N` entra");
        println!("  acompanhe: ds2os-dev logs server -f -g \"logged in\"");
        return Ok(());
    }

    // Both were started before anything is driven, so the two boots overlap:
    // the second is already at its title screen by the time the first is in the
    // world. Driving is serial because there is one pad and one focus.
    println!("\nno mundo");
    let settings = HarnessConfig::load();
    for account in started {
        let expected = drive::expected_character(&settings, account);
        match drive::enter(environment, account, expected.as_deref(), drive::DEFAULT_TIMEOUT) {
            Ok(arrival) => println!(
                "  conta {account}: {} em {:.0}s",
                arrival.character, arrival.seconds
            ),
            Err(error) => println!("  conta {account}: {error}"),
        }
    }
    println!("\n  acompanhe: ds2os-dev logs server -f -g \"logged in\"");
    Ok(())
}

fn status(environment: &Environment, json: bool) -> Result<(), String> {
    let payload = Status {
        problems: environment.problems(),
        server: server::status(environment),
        instances: game::instances_status(environment),
        logs: [
            Some(paths::server_log()),
            Some(paths::instance_log(2)),
            Some(paths::cli_log()),
            game::injector_log(environment),
            game::timer_log(environment),
        ]
        .into_iter()
        .flatten()
        .filter(|p| game::exists(p))
        .map(|p| p.display().to_string())
        .collect(),
        environment: environment.clone(),
    };

    if json {
        println!("{}", serde_json::to_string_pretty(&payload).map_err(|e| e.to_string())?);
        return Ok(());
    }

    println!("servidor");
    print_server(&payload.server);

    println!("\ninstâncias");
    println!("  processos do jogo  {:?}", payload.instances.game_processes);
    println!(
        "  segunda instância  {}",
        payload
            .instances
            .second_instance_pid
            .map(|p| format!("pid {p}"))
            .unwrap_or_else(|| "parada".into())
    );
    println!("  wrapper instalado  {}", yes_no(payload.instances.wrapper_installed));
    println!("  injector pronto    {}", yes_no(payload.instances.injector_ready));

    println!("\nlogs");
    for log in &payload.logs {
        println!("  {log}");
    }

    if !payload.problems.is_empty() {
        println!("\nproblemas");
        for problem in &payload.problems {
            println!("  {}", problem.what);
        }
    }
    Ok(())
}

fn print_server(status: &server::ServerStatus) {
    println!(
        "  estado             {}",
        status.pid.map(|p| format!("no ar, pid {p}")).unwrap_or_else(|| "parado".into())
    );
    println!(
        "  portas             {}",
        if status.ports_listening.is_empty() {
            "nenhuma".to_owned()
        } else {
            status.ports_listening.join(", ")
        }
    );
    println!(
        "  jogadores          {}",
        status.players.map(|p| p.to_string()).unwrap_or_else(|| "—".into())
    );
    println!("  config correta     {}", yes_no(status.config_ok));
}

fn yes_no(value: bool) -> &'static str {
    if value {
        "sim"
    } else {
        "não"
    }
}
