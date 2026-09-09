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
mod server;

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
    Launch,
    /// Stops the second instance
    Stop,
    /// Prints the line to paste into Steam's launch options
    Options,
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
            GameAction::Launch => {
                let pid = game::launch_second(&environment)?;
                println!("  segunda instância iniciada, pid {pid}");
                println!("  log: {}", paths::instance_log(2).display());
                Ok(())
            }
            GameAction::Stop => {
                println!("  parada: {}", yes_no(game::stop_second()));
                Ok(())
            }
            GameAction::Options => {
                let line = game::launch_options(&environment)
                    .ok_or("Dark Souls II não está instalado")?;
                println!("{line}");
                Ok(())
            }
        },
        Command::Logs { which, lines, grep, follow } => {
            let path = log_path(&environment, which)
                .ok_or("esse log não existe neste ambiente")?;
            logs::show(&path, &logs::Options { lines, grep: grep.as_deref(), follow })
                .map_err(|e| e.to_string())
        }
    }
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
    let prepared = game::prepare(environment, timer_seconds, timer_patch)?;
    let wrapper = game::write_wrapper(environment)?;

    println!("  pasta do jogo   {}", prepared.game_dir.display());
    if !prepared.copied.is_empty() {
        println!("  copiado         {}", prepared.copied.join(", "));
    }
    println!("  injector config {}", prepared.injector_config.display());
    println!("  wrapper         {}", wrapper.display());
    println!(
        "  timer           {}",
        if timer_patch { format!("{timer_seconds:.0}s") } else { "desligado".to_owned() }
    );
    Ok(())
}

fn up(
    environment: &Environment,
    timer_seconds: f64,
    timer_patch: bool,
    start_second: bool,
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

    if start_second {
        println!("\nsegunda instância");
        let pid = game::launch_second(environment)?;
        println!("  iniciada, pid {pid}");
        println!("  log {}", paths::instance_log(2).display());
    }

    println!("\nfalta você");
    println!("  1. cole isto nas opções de lançamento do Dark Souls II na Steam:");
    println!(
        "       {}",
        game::launch_options(environment).unwrap_or_else(|| "—".into())
    );
    println!("  2. dê Play na Steam. A Steam é quem inicia a primeira instância.");
    println!("  3. acompanhe: ds2os-dev logs server -f -g client");
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
