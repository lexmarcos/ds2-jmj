//! ds2os-dev - debug harness for a local Dark Souls II server.
//!
//! Brings up the local DS3OS server, configures the game to talk to it, and
//! runs the second instance so PvP can be tested with a single copy of the
//! game. It deliberately has nothing to do with the public master server: this
//! is for one machine, one server, two clients.
//!
//! Instance 1 belongs to Steam. The harness configures it and notices when it
//! appears, but Steam is what starts it.

mod env;
mod game;
mod logs;
mod paths;
mod proc;
mod screen;
mod server;
mod settings;

use std::path::PathBuf;

use clap::{Parser, Subcommand, ValueEnum};
use serde::Serialize;

use env::Environment;

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
        /// Prepare everything but do not start the second instance
        #[arg(long)]
        no_second: bool,
    },
    /// Stops the server and the second instance
    Down,
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
    },
    /// Starts the second instance in its own Proton prefix
    Launch {
        /// Home of the second Steam client; defaults to what steam2 saved
        #[arg(long)]
        steam_home: Option<PathBuf>,
    },
    /// Stops the second instance
    Stop,
    /// Prints the line to paste into Steam's launch options
    Options,
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
        Command::Up { timer_seconds, no_timer, no_second } => {
            up(&environment, timer_seconds, !no_timer, !no_second)
        }
        Command::Down => {
            let stopped_game = game::stop_second();
            let stopped_server = server::down();
            println!("  segunda instância: {}", yes_no(stopped_game));
            println!("  servidor:          {}", yes_no(stopped_server));
            Ok(())
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
            GameAction::Prepare { timer_seconds, no_timer } => {
                prepare(&environment, timer_seconds, !no_timer)
            }
            GameAction::Launch { steam_home } => {
                let home = resolve_second_steam(steam_home)?;
                announce_account(home.as_deref());
                let pid = game::launch_second(&environment, home.as_deref())?;
                println!("  segunda instância iniciada, pid {pid}");
                println!("  log: {}", paths::instance_log(2).display());
                Ok(())
            }
            GameAction::Stop => {
                println!("  parada: {}", yes_no(game::stop_second()));
                Ok(())
            }
            GameAction::Options => {
                print_launch_options(&environment);
                Ok(())
            }
            GameAction::Shot { out } => shot(out),
        },
        Command::Steam2 { action } => steam2(action),
        Command::Logs { which, lines, grep, follow } => {
            let path = log_path(&environment, which)
                .ok_or("esse log não existe neste ambiente")?;
            logs::show(&path, &logs::Options { lines, grep: grep.as_deref(), follow })
                .map_err(|e| e.to_string())
        }
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

/// Writes one PNG per game window. Naming them by index keeps the paths stable
/// between calls, so a later capture overwrites the earlier one rather than
/// filling the directory.
fn shot(out: Option<PathBuf>) -> Result<(), String> {
    let dir = out.unwrap_or_else(paths::log_dir);
    let windows = screen::windows()?;

    if windows.is_empty() {
        return Err("nenhuma janela do Dark Souls II aberta".into());
    }

    for (index, window) in windows.iter().enumerate() {
        let path = dir.join(format!("shot-{}.png", index + 1));
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

fn prepare(environment: &Environment, timer_seconds: f64, timer_patch: bool) -> Result<(), String> {
    if environment.installs.is_empty() {
        return Err("nenhuma instalação do Dark Souls II encontrada".into());
    }

    for install in &environment.installs {
        let prepared = game::prepare(environment, install, timer_seconds, timer_patch)?;
        println!("  conta {}", prepared.account);
        println!("    pasta   {}", prepared.game_dir.display());
        if !prepared.copied.is_empty() {
            println!("    copiado {}", prepared.copied.join(", "));
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
    _start_second: bool,
) -> Result<(), String> {
    let problems = environment.problems();
    if !problems.is_empty() {
        for problem in &problems {
            eprintln!("  {}\n    → {}", problem.what, problem.fix);
        }
        return Err("o ambiente não está pronto; rode `ds2os-dev doctor`".into());
    }

    println!("servidor");
    let status = server::up(environment)?;
    print_server(&status);

    println!("\njogo");
    prepare(environment, timer_seconds, timer_patch)?;

    println!("\nopções de lançamento");
    print_launch_options(environment);

    println!("\nfalta você");
    if environment.installs.len() < 2 {
        println!("  só uma conta configurada. As duas instâncias precisam de contas Steam");
        println!("  diferentes, senão a sessão PvP entre elas não conecta.");
        println!("  rode: ds2os-dev steam2 init");
    } else {
        println!("  1. cole a linha da conta 1 nas opções de lançamento do DS2 nessa Steam");
        println!("  2. cole a linha da conta 2 nas opções de lançamento da OUTRA Steam");
        println!("     (abra-a com: ds2os-dev steam2 run)");
        println!("  3. dê Play nas duas");
    }
    println!("  acompanhe: ds2os-dev logs server -f -g \"logged in\"");
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
