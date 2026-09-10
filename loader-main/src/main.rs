mod paths;
mod pinned;
mod preflight;
mod settings;

fn main() {
    let settings = settings::Settings::load();

    if std::env::args().any(|arg| arg == "--doctor") {
        preflight::doctor(&settings);
        return;
    }

    preflight::doctor(&settings);
}
