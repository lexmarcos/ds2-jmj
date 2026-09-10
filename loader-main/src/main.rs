mod paths;
mod pinned;
mod settings;

fn main() {
    let config = pinned::injector_config();
    println!("servidor {} em {}:{}", config.ServerName, config.ServerHostname, config.ServerPort);
    println!("dados    {}", paths::data_dir().display());
    println!("injector {}", paths::injector_dir().display());
    println!("settings {}", paths::settings_path().display());
    println!("fontes   {:?}", paths::injector_sources());
    println!("overrides {}", settings::Settings::load().has_overrides());
}
