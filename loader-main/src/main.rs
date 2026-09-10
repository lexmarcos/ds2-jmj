mod launch;
mod paths;
mod pinned;
mod preflight;
mod settings;
mod watch;

fn main() {
    let settings = settings::Settings::load();

    if std::env::args().any(|arg| arg == "--launch") {
        return start(&settings);
    }

    preflight::doctor(&settings);
}

/// Starts the game and reports what the injector says, without a window.
/// The interface will do the same thing with the same pieces.
fn start(settings: &settings::Settings) {
    let ready = match preflight::run(settings) {
        Ok(ready) => ready,
        Err(problem) => {
            println!("bloqueado: {}", problem.message());
            std::process::exit(1);
        }
    };

    let mut watcher = watch::Watcher::new();
    let mut child = match launch::start(&ready) {
        Ok(child) => child,
        Err(error) => {
            println!("{error}");
            std::process::exit(1);
        }
    };
    println!("iniciado, pid {}", child.id());

    loop {
        for signal in watcher.poll() {
            println!("  {signal:?}");
        }
        match child.try_wait() {
            Ok(Some(status)) => {
                println!("o jogo fechou ({status})");
                for signal in watcher.poll() {
                    println!("  {signal:?}");
                }
                return;
            }
            Ok(None) => std::thread::sleep(std::time::Duration::from_millis(500)),
            Err(error) => {
                println!("perdi o jogo de vista: {error}");
                return;
            }
        }
    }
}
