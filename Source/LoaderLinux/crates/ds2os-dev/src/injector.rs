//! The injector's round trip from a push to the game: which CI run built the
//! code in the tree, bringing its binaries here, and which build each running
//! game actually has.
//!
//! `Injector.dll` needs MSVC and is only ever built on CI, so a change to a hook
//! used to cost a hand-typed `gh run download` into `~/Downloads/injector`, with
//! nothing recording which commit that directory came from — and a running game
//! whose DLL was one push behind looked exactly like a hook that did not work.
//!
//! - `fetch` picks the run that built the injector sources the tree has, waits
//!   for it, downloads, checks the bytes are PE images, keeps the previous
//!   directory as `injector.prev`, and writes `manifest.json`. It never installs:
//!   `game prepare` overwrites the DLL of a running game.
//! - `status` compares the manifest, each installation's DLL and the `build`
//!   each running game's receipt announces.
//! - `check` is a syntax check with mingw, before spending a CI run on a typo.
//!   It is **syntax**, not a build: MSVC's SEH is rewritten away, Detours is a
//!   stub of declarations, and mingw's headers are not the Windows SDK's.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::control::Deadline;
use crate::env::Environment;
use crate::output::{self, line};

pub const BINARIES: [&str; 2] = ["Injector.dll", "Injector.exe"];
const WORKFLOW: &str = "injector-linux.yml";
const ARTIFACT: &str = "injector";
/// The paths `injector-linux.yml` builds from; a run whose commit differs from
/// HEAD only outside them built the same injector.
const SOURCES: [&str; 5] = ["Source/Injector", "Source/InjectorLauncher", "Source/Shared", "Source/ThirdParty/detours",
    ".github/workflows/injector-linux.yml"];

// ---------------------------------------------------------------------------
// Runs and selection

#[derive(Debug, Clone, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Run {
    pub database_id: u64,
    pub head_sha: String,
    #[serde(default)]
    pub head_branch: String,
    pub status: String,
    #[serde(default)]
    pub conclusion: String,
    #[serde(default)]
    pub created_at: String,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Pick { Run(u64), Latest, SameCode }

#[derive(Debug, Clone, PartialEq)]
pub struct Choice { pub run: Run, pub same_code: bool }

/// Chooses a run from `gh run list`. By default, the newest run whose commit
/// builds the injector sources HEAD has (`same_code` decides, from git), so a
/// harness-only commit on top of an injector push still finds that push's run.
pub fn select(runs: &[Run], pick: Pick, same_code: &dyn Fn(&str) -> bool) -> Result<Choice, String> {
    let mut newest: Vec<&Run> = runs.iter().collect();
    newest.sort_by(|a, b| b.created_at.cmp(&a.created_at));
    let chosen = match pick {
        Pick::Run(id) => newest.into_iter().find(|r| r.database_id == id)
            .ok_or_else(|| format!("run_not_found: o run {id} não é do {WORKFLOW}"))?,
        Pick::Latest => newest.into_iter().next().ok_or_else(|| format!("no_run: nenhum run do {WORKFLOW} neste branch"))?,
        Pick::SameCode => newest.into_iter().find(|r| same_code(&r.head_sha)).ok_or_else(|| format!(
            "no_run_for_head: nenhum run do {WORKFLOW} construiu o injector que HEAD tem; faça push, ou use --latest / --run"))?,
    };
    Ok(Choice { same_code: same_code(&chosen.head_sha), run: chosen.clone() })
}

fn gh(args: &[&str]) -> Result<String, String> {
    let out = Command::new("gh").args(args).output().map_err(|e| format!("gh_missing: gh não executou: {e}"))?;
    if !out.status.success() {
        return Err(format!("gh_failed: gh {}: {}", args.join(" "), String::from_utf8_lossy(&out.stderr).trim()));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

fn git(repo: &Path, args: &[&str]) -> Result<std::process::Output, String> {
    Command::new("git").arg("-C").arg(repo).args(args).output().map_err(|e| format!("git_missing: {e}"))
}

fn git_text(repo: &Path, args: &[&str]) -> Result<String, String> {
    let out = git(repo, args)?;
    if !out.status.success() {
        return Err(format!("git_failed: git {}: {}", args.join(" "), String::from_utf8_lossy(&out.stderr).trim()));
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_owned())
}

/// Whether the commit has the injector sources HEAD has. A commit git does not
/// know (never fetched) is not the same code.
fn same_code_as_head(repo: &Path, sha: &str) -> bool {
    let mut args = vec!["diff", "--quiet", sha, "HEAD", "--"];
    args.extend(SOURCES);
    git(repo, &args).map(|o| o.status.code() == Some(0)).unwrap_or(false)
}

/// Injector sources changed in the working tree and not committed: no run has them.
fn uncommitted_sources(repo: &Path) -> Vec<String> {
    let mut args = vec!["status", "--porcelain", "--"];
    args.extend(SOURCES);
    git_text(repo, &args).map(|t| t.lines().map(|l| l.get(3..).unwrap_or(l).to_owned()).collect()).unwrap_or_default()
}

const RUN_FIELDS: &str = "databaseId,headSha,headBranch,status,conclusion,createdAt";

fn view(id: u64) -> Result<Run, String> {
    serde_json::from_str(&gh(&["run", "view", &id.to_string(), "--json", RUN_FIELDS])?)
        .map_err(|e| format!("gh_failed: resposta do gh run view ilegível: {e}"))
}

/// Polls the run until it completes; `gh run watch` has no deadline and no JSON.
fn wait(mut run: Run, deadline: Deadline) -> Result<Run, String> {
    let mut last = String::new();
    while run.status != "completed" {
        if run.status != last {
            line(format_args!("run {} {}; aguardando", run.database_id, run.status));
            output::event("injector_run", json!({"runId": run.database_id, "status": run.status}));
            last = run.status.clone();
        }
        deadline.sleep(Duration::from_secs(15)).map_err(|e| format!("{e} (run {} ainda {})", run.database_id, run.status))?;
        run = view(run.database_id)?;
    }
    Ok(run)
}

// ---------------------------------------------------------------------------
// The directory

#[derive(Debug, Clone, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
    pub schema_version: u32,
    pub run_id: u64,
    pub head_sha: String,
    pub branch: String,
    pub fetched_at: String,
    /// sha256 of each binary, by file name.
    pub files: BTreeMap<String, String>,
}

pub fn dir() -> PathBuf { crate::paths::home().join("Downloads/injector") }

fn sibling(dir: &Path, suffix: &str) -> PathBuf {
    let mut name = dir.file_name().unwrap_or_default().to_os_string();
    name.push(suffix);
    dir.with_file_name(name)
}

pub fn read_manifest(dir: &Path) -> Option<Manifest> {
    serde_json::from_slice(&std::fs::read(dir.join("manifest.json")).ok()?).ok()
}

pub fn sha256(path: &Path) -> Option<String> {
    output::fingerprint(path).get("sha256").and_then(Value::as_str).map(str::to_owned)
}

/// Both binaries present and PE images, with their hashes.
fn verify_binaries(dir: &Path) -> Result<BTreeMap<String, String>, String> {
    let mut files = BTreeMap::new();
    for name in BINARIES {
        let path = dir.join(name);
        let mut magic = [0u8; 2];
        std::fs::File::open(&path).and_then(|mut f| f.read_exact(&mut magic))
            .map_err(|e| format!("artifact_incomplete: {}: {e}", path.display()))?;
        if &magic != b"MZ" {
            return Err(format!("artifact_invalid: {} não começa com MZ", path.display()));
        }
        files.insert(name.to_owned(), sha256(&path).ok_or_else(|| format!("artifact_incomplete: {} ilegível", path.display()))?);
    }
    Ok(files)
}

/// `new` becomes `dir`, and what `dir` was becomes `dir.prev` (replacing the
/// previous one). Nothing is moved unless `new` exists.
pub fn rotate(new: &Path, dir: &Path) -> Result<Option<PathBuf>, String> {
    if !new.is_dir() { return Err(format!("rotate: {} não existe", new.display())); }
    let prev = sibling(dir, ".prev");
    let kept = if dir.exists() {
        if prev.exists() {
            std::fs::remove_dir_all(&prev).map_err(|e| format!("rotate: remover {}: {e}", prev.display()))?;
        }
        std::fs::rename(dir, &prev).map_err(|e| format!("rotate: {} -> {}: {e}", dir.display(), prev.display()))?;
        Some(prev)
    } else { None };
    std::fs::rename(new, dir).map_err(|e| format!("rotate: {} -> {}: {e}", new.display(), dir.display()))?;
    Ok(kept)
}

// ---------------------------------------------------------------------------
// What is installed and running

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Build {
    /// The receipt's build is the manifest's commit.
    Current,
    /// Built from another commit.
    Other,
    /// A local build, or CI before `DS2OS_BUILD_SHA`.
    Unknown,
    /// A DLL from before the receipt carried `build`.
    Unrecorded,
    /// Nothing to compare against: no manifest, or no receipt for this boot.
    NoReference,
}

pub fn compare_build(receipt_build: Option<&str>, manifest_sha: Option<&str>, receipt_present: bool) -> Build {
    match (receipt_present, receipt_build, manifest_sha) {
        (false, _, _) => Build::NoReference,
        (true, None, _) => Build::Unrecorded,
        (true, Some("unknown"), _) => Build::Unknown,
        (true, Some(_), None) => Build::NoReference,
        (true, Some(build), Some(sha)) if build.eq_ignore_ascii_case(sha) => Build::Current,
        _ => Build::Other,
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallState {
    pub instance: u8,
    pub installed_sha256: Option<String>,
    pub equals_source: bool,
    pub running: bool,
    pub boot_id: Option<String>,
    pub build: Option<String>,
    pub build_state: Build,
}

fn install_states(env: &Environment, source_dll: Option<&str>, manifest: Option<&Manifest>) -> Vec<InstallState> {
    env.installs.iter().map(|install| {
        let installed = sha256(&install.game_dir.join("Injector.dll"));
        let running = crate::game::compat_data(env, install.account).map(|p| !crate::game::instance_pids(&p).is_empty()).unwrap_or(false);
        let receipt = if running { crate::observe::hooks(&install.game_dir) } else { None };
        let build = receipt.as_ref().and_then(|r| r.get("build")).and_then(Value::as_str).map(str::to_owned);
        InstallState {
            instance: install.account,
            equals_source: installed.is_some() && installed.as_deref() == source_dll,
            installed_sha256: installed,
            running,
            boot_id: receipt.as_ref().and_then(|r| r.get("bootId")).and_then(Value::as_str).map(str::to_owned),
            build_state: compare_build(build.as_deref(), manifest.map(|m| m.head_sha.as_str()), receipt.is_some()),
            build,
        }
    }).collect()
}

/// Whether an installation must be prepared, and a running game relaunched,
/// to have the fetched DLL.
pub fn needs(states: &[InstallState]) -> (bool, bool) {
    let prepare = states.iter().any(|s| !s.equals_source);
    let relaunch = states.iter().any(|s| s.running && (!s.equals_source || s.build_state != Build::Current));
    (prepare, relaunch)
}

// ---------------------------------------------------------------------------
// fetch

fn cancel_ci(shas: &[&str]) -> (Vec<u64>, Vec<String>) {
    let mut cancelled = Vec::new();
    let mut warnings = Vec::new();
    for sha in shas {
        let listed = gh(&["run", "list", "--workflow", "ci.yml", "--commit", sha, "--json", RUN_FIELDS]);
        let runs: Vec<Run> = match listed.and_then(|t| serde_json::from_str(&t).map_err(|e| e.to_string())) {
            Ok(runs) => runs,
            Err(e) => { warnings.push(format!("ci_list_failed: {e}")); continue; }
        };
        for run in runs.into_iter().filter(|r| r.status != "completed") {
            match gh(&["run", "cancel", &run.database_id.to_string()]) {
                Ok(_) => cancelled.push(run.database_id),
                Err(e) => warnings.push(format!("ci_cancel_failed: {} {e}", run.database_id)),
            }
        }
    }
    // Cancelling is a request; confirm it landed.
    let deadline = Deadline::after(Duration::from_secs(60));
    let mut pending = cancelled.clone();
    while !pending.is_empty() {
        pending.retain(|id| view(*id).map(|r| r.status != "completed").unwrap_or(true));
        if pending.is_empty() || deadline.sleep(Duration::from_secs(5)).is_err() { break; }
    }
    for id in &pending { warnings.push(format!("ci_cancel_unconfirmed: o run {id} do ci.yml não chegou a cancelled em 60 s")); }
    (cancelled, warnings)
}

pub fn fetch(env: &Environment, pick: Pick, timeout: Duration) -> Result<(), String> {
    let repo = env.repo_root.clone().ok_or("repo_missing: repositório não encontrado")?;
    let head = git_text(&repo, &["rev-parse", "HEAD"])?;
    let branch = git_text(&repo, &["branch", "--show-current"])?;
    let uncommitted = uncommitted_sources(&repo);

    let run = match pick {
        Pick::Run(id) => view(id)?,
        _ => {
            let listed = gh(&["run", "list", "--workflow", WORKFLOW, "--branch", &branch, "--limit", "30", "--json", RUN_FIELDS])?;
            let runs: Vec<Run> = serde_json::from_str(&listed).map_err(|e| format!("gh_failed: gh run list ilegível: {e}"))?;
            select(&runs, pick, &|sha| same_code_as_head(&repo, sha))?.run
        }
    };
    let same_code = same_code_as_head(&repo, &run.head_sha);
    output::event("injector_selected", json!({"run": run, "sameCodeAsHead": same_code, "head": head}));
    line(format_args!("run {} ({}) de {}{}", run.database_id, run.status, &run.head_sha[..12.min(run.head_sha.len())],
        if same_code { "" } else { " — NÃO é o injector que HEAD tem" }));

    let run = wait(run, Deadline::after(timeout))?;
    if run.conclusion != "success" {
        return Err(format!("injector_build_failed: o run {} terminou {}", run.database_id, run.conclusion));
    }

    let dir = dir();
    let current = read_manifest(&dir);
    let already = current.as_ref().is_some_and(|m| m.run_id == run.database_id
        && verify_binaries(&dir).map(|files| files == m.files).unwrap_or(false));
    let (files, previous) = if already {
        line(format_args!("{} já tem o run {}", dir.display(), run.database_id));
        (current.as_ref().unwrap().files.clone(), None)
    } else {
        let new = sibling(&dir, ".new");
        if new.exists() { std::fs::remove_dir_all(&new).map_err(|e| format!("rotate: remover {}: {e}", new.display()))?; }
        gh(&["run", "download", &run.database_id.to_string(), "-n", ARTIFACT, "-D", &new.to_string_lossy()])?;
        let files = verify_binaries(&new)?;
        let manifest = Manifest {
            schema_version: 1, run_id: run.database_id, head_sha: run.head_sha.clone(),
            branch: run.head_branch.clone(), fetched_at: crate::timeline::format_local(output::now_ms() as i64), files: files.clone(),
        };
        std::fs::write(new.join("manifest.json"), serde_json::to_vec_pretty(&manifest).unwrap())
            .map_err(|e| format!("manifest: {e}"))?;
        let kept = rotate(&new, &dir)?;
        (files, kept)
    };

    let (cancelled, mut warnings) = cancel_ci(&if run.head_sha == head { vec![head.as_str()] } else { vec![run.head_sha.as_str(), head.as_str()] });
    if !same_code {
        warnings.push(format!("not_head_code: o run {} construiu {} e os fontes do injector mudaram até HEAD", run.database_id, run.head_sha));
    }
    if !uncommitted.is_empty() {
        warnings.push(format!("uncommitted_sources: fontes do injector alterados sem commit, que nenhum run tem: {}", uncommitted.join(", ")));
    }

    let manifest = read_manifest(&dir);
    let states = install_states(env, files.get("Injector.dll").map(String::as_str), manifest.as_ref());
    let (needs_prepare, needs_relaunch) = needs(&states);
    for s in &states {
        line(format_args!("instância {}: DLL {}{}", s.instance, if s.equals_source { "igual à baixada" } else { "diferente" },
            if s.running { format!(", rodando build {}", s.build.as_deref().unwrap_or("sem campo build")) } else { String::new() }));
    }
    for w in &warnings { line(format_args!("aviso: {w}")); }
    if needs_prepare {
        line(format_args!("próximo: {}`ds2os-dev game prepare` (ou `up`)", if states.iter().any(|s| s.running) { "feche os jogos, " } else { "" }));
    }
    output::data(json!({
        "runId": run.database_id, "fetchedSha": run.head_sha, "branch": run.head_branch, "head": head,
        "sameCodeAsHead": same_code, "alreadyFetched": already, "dir": dir, "previous": previous,
        "files": files, "installs": states, "needsPrepare": needs_prepare, "needsRelaunch": needs_relaunch,
        "ciCancelled": cancelled, "warnings": warnings,
    }));
    Ok(())
}

// ---------------------------------------------------------------------------
// status

pub fn status(env: &Environment) -> Result<(), String> {
    let source = env.injector_source.clone();
    let manifest = source.as_deref().and_then(read_manifest);
    let source_dll = source.as_ref().and_then(|s| sha256(&s.join("Injector.dll")));
    let manifest_matches = manifest.as_ref().map(|m| m.files.get("Injector.dll") == source_dll.as_ref());
    let states = install_states(env, source_dll.as_deref(), manifest.as_ref());
    let (needs_prepare, needs_relaunch) = needs(&states);

    match (&source, &manifest) {
        (None, _) => line(format_args!("sem diretório de origem com Injector.dll")),
        (Some(dir), None) => line(format_args!("{}: sem manifest.json (não veio de `injector fetch`)", dir.display())),
        (Some(dir), Some(m)) => line(format_args!("{}: run {} de {} ({}){}", dir.display(), m.run_id, m.head_sha, m.fetched_at,
            if manifest_matches == Some(true) { "" } else { " — a DLL não é a do manifest" })),
    }
    for s in &states {
        line(format_args!("instância {}: {}{}", s.instance,
            if s.equals_source { "DLL igual à de origem" } else { "DLL diferente da de origem" },
            if s.running { format!(", boot {} build {} ({:?})", s.boot_id.as_deref().unwrap_or("?"), s.build.as_deref().unwrap_or("-"), s.build_state) } else { ", parada".to_owned() }));
    }
    output::data(json!({
        "source": source, "manifest": manifest, "sourceSha256": source_dll, "manifestMatchesSource": manifest_matches,
        "installs": states, "needsPrepare": needs_prepare, "needsRelaunch": needs_relaunch,
    }));
    Ok(())
}

// ---------------------------------------------------------------------------
// check

const DETOURS_STUB: &str = "#pragma once\n#include <windows.h>\nLONG DetourTransactionBegin();\nLONG DetourUpdateThread(HANDLE);\n\
LONG DetourAttach(PVOID*, PVOID);\nLONG DetourDetach(PVOID*, PVOID);\nLONG DetourTransactionCommit();\n";
const COMPILER: &str = "x86_64-w64-mingw32-g++";

fn is_word(b: u8) -> bool { b.is_ascii_alphanumeric() || b == b'_' }

/// Rewrites what mingw cannot parse into what it can, keeping every line where
/// it was: `__try` becomes `if (1)`, `__except (filter)` becomes `else` (the
/// filter's newlines kept), `_ReturnAddress()` the GCC builtin.
pub fn msvc_rewrite(src: &str) -> String {
    let bytes = src.as_bytes();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    let at_word = |i: usize, word: &str| bytes[i..].starts_with(word.as_bytes())
        && (i == 0 || !is_word(bytes[i - 1]))
        && bytes.get(i + word.len()).is_none_or(|b| !is_word(*b));
    while i < bytes.len() {
        if at_word(i, "__try") {
            out.push_str("if (1)");
            i += "__try".len();
        } else if at_word(i, "__except") {
            let mut j = i + "__except".len();
            while j < bytes.len() && bytes[j].is_ascii_whitespace() { j += 1; }
            if bytes.get(j) != Some(&b'(') { out.push_str("__except"); i += "__except".len(); continue; }
            let mut depth = 0;
            let mut k = j;
            while k < bytes.len() {
                match bytes[k] { b'(' => depth += 1, b')' => { depth -= 1; if depth == 0 { break; } } _ => {} }
                k += 1;
            }
            out.push_str("else");
            out.extend(src[i..(k + 1).min(src.len())].chars().filter(|c| *c == '\n'));
            i = k + 1;
        } else if at_word(i, "_ReturnAddress") && bytes[i + "_ReturnAddress".len()..].starts_with(b"()") {
            out.push_str("__builtin_return_address(0)");
            i += "_ReturnAddress()".len();
        } else {
            let ch = src[i..].chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
        }
    }
    out
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Diagnostic { pub line: u32, pub message: String }

/// `path:line:col: error: message` lines about the checked file; errors inside
/// headers keep their own path in the message.
pub fn parse_errors(stderr: &str, copy: &str) -> Vec<Diagnostic> {
    stderr.lines().filter_map(|l| {
        let (location, message) = l.split_once(": error: ").or_else(|| l.split_once(": fatal error: "))?;
        let mut parts = location.rsplitn(3, ':');
        let (_col, line, path) = (parts.next()?, parts.next()?, parts.next()?);
        let line: u32 = line.parse().ok()?;
        Some(if path == copy { Diagnostic { line, message: message.to_owned() } }
             else { Diagnostic { line: 0, message: format!("{path}:{line}: {message}") } })
    }).collect()
}

/// Errors the baseline does not have, compared by message: line numbers move
/// with any edit above them.
pub fn new_errors(current: &[Diagnostic], baseline: &[Diagnostic]) -> Vec<Diagnostic> {
    let mut left: Vec<&str> = baseline.iter().map(|d| d.message.as_str()).collect();
    current.iter().filter(|d| {
        match left.iter().position(|m| *m == d.message) { Some(k) => { left.remove(k); false } None => true }
    }).cloned().collect()
}

struct Checker { source: PathBuf, stub: PathBuf }

impl Checker {
    fn new(source: PathBuf) -> Result<Self, String> {
        let stub = std::env::temp_dir().join(format!("ds2os-injector-check-{}", std::process::id()));
        let detours = stub.join("ThirdParty/detours/src");
        std::fs::create_dir_all(&detours).and_then(|_| std::fs::create_dir_all(stub.join("copies")))
            .and_then(|_| std::fs::write(detours.join("detours.h"), DETOURS_STUB))
            .and_then(|_| std::fs::write(stub.join("Windows.h"), "#include <windows.h>\n"))
            .map_err(|e| format!("check_setup: {e}"))?;
        Ok(Self { source, stub })
    }

    /// Compiles `content` as if it were `rel` (relative to `Source/`).
    fn errors(&self, rel: &str, content: &str, tag: &str) -> Result<Vec<Diagnostic>, String> {
        let copy = self.stub.join("copies").join(format!("{tag}-{}", rel.replace('/', "_")));
        std::fs::write(&copy, msvc_rewrite(content)).map_err(|e| format!("check_setup: {e}"))?;
        let own_dir = self.source.join(rel).parent().map(Path::to_path_buf).unwrap_or_else(|| self.source.clone());
        let out = Command::new(COMPILER).current_dir(&self.source)
            .args(["-std=c++17", "-fsyntax-only", "-fpermissive", "-w", "-D_WIN32", "-D_M_X64", "-DDS2OS_BUILD_SHA=\"check\""])
            .arg(format!("-I{}", self.stub.display())).arg("-iquote").arg(&own_dir)
            .args(["-I.", "-IInjector", "-IShared", "-IThirdParty", "-IThirdParty/nlohmann/include", "-IThirdParty/curl/include",
                "-IThirdParty/protobuf-2.6.1rc1/src"])
            .arg(&copy).output().map_err(|e| format!("mingw_missing: {COMPILER} não executou: {e}"))?;
        let stderr = String::from_utf8_lossy(&out.stderr);
        let errors = parse_errors(&stderr, &copy.to_string_lossy());
        let _ = std::fs::remove_file(&copy);
        if !out.status.success() && errors.is_empty() {
            return Err(format!("check_failed: {COMPILER} falhou sem erro legível em {rel}: {}", stderr.lines().next().unwrap_or("")));
        }
        Ok(errors)
    }
}

impl Drop for Checker { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.stub); } }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileResult {
    pub file: String,
    /// `ok`, `errors`, or `failed` (the compiler did not answer).
    pub status: &'static str,
    pub errors: Vec<Diagnostic>,
    /// Errors HEAD's version has too; mingw, not this change.
    pub preexisting: usize,
    pub note: Option<String>,
}

struct Job { rel: String, content: String, baseline: Option<String> }

fn check_one(checker: &Checker, job: &Job, tag: usize) -> FileResult {
    let result = checker.errors(&job.rel, &job.content, &format!("{tag}w")).and_then(|current| {
        if current.is_empty() { return Ok((current, 0)); }
        let base = match &job.baseline { Some(b) => checker.errors(&job.rel, b, &format!("{tag}h"))?, None => vec![] };
        let fresh = new_errors(&current, &base);
        Ok((fresh, current.len() - new_errors(&current, &base).len()))
    });
    match result {
        Ok((errors, preexisting)) => FileResult {
            file: job.rel.clone(), status: if errors.is_empty() { "ok" } else { "errors" }, preexisting,
            note: (preexisting > 0).then(|| format!("{preexisting} erro(s) que a versão de HEAD também tem (mingw, não esta mudança)")),
            errors,
        },
        Err(e) => FileResult { file: job.rel.clone(), status: "failed", errors: vec![], preexisting: 0, note: Some(e) },
    }
}

fn run_jobs(checker: &Checker, jobs: &[Job]) -> Vec<FileResult> {
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(jobs.len().max(1));
    let next = std::sync::atomic::AtomicUsize::new(0);
    let results = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..workers {
            scope.spawn(|| loop {
                let k = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if k >= jobs.len() || crate::control::check().is_err() { break; }
                let result = check_one(checker, &jobs[k], k);
                results.lock().unwrap().push((k, result));
            });
        }
    });
    let mut results = results.into_inner().unwrap();
    results.sort_by_key(|(k, _)| *k);
    results.into_iter().map(|(_, r)| r).collect()
}

fn head_version(repo: &Path, rel_to_repo: &str) -> Option<String> {
    let out = git(repo, &["show", &format!("HEAD:{rel_to_repo}")]).ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).into_owned())
}

/// The `.cpp` under `Source/Injector` changed against HEAD or untracked, plus
/// every `.cpp` there that includes a changed header by name.
fn changed_files(repo: &Path) -> Result<Vec<String>, String> {
    let mut paths: Vec<String> = git_text(repo, &["diff", "--name-only", "HEAD", "--", "Source/Injector"])?.lines().map(str::to_owned).collect();
    paths.extend(git_text(repo, &["ls-files", "--others", "--exclude-standard", "--", "Source/Injector"])?.lines().map(str::to_owned));
    let headers: Vec<String> = paths.iter().filter(|p| p.ends_with(".h")).filter_map(|p| Path::new(p).file_name())
        .map(|n| n.to_string_lossy().into_owned()).collect();
    let mut files: Vec<String> = paths.into_iter().filter(|p| p.ends_with(".cpp") && repo.join(p).is_file()).collect();
    if !headers.is_empty() {
        for cpp in all_files(repo) {
            let text = std::fs::read_to_string(repo.join(&cpp)).unwrap_or_default();
            if headers.iter().any(|h| text.lines().any(|l| l.trim_start().starts_with("#include") && l.contains(h.as_str()))) {
                files.push(cpp);
            }
        }
    }
    files.sort();
    files.dedup();
    Ok(files)
}

fn all_files(repo: &Path) -> Vec<String> {
    fn walk(dir: &Path, repo: &Path, out: &mut Vec<String>) {
        for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let path = entry.path();
            if path.is_dir() { walk(&path, repo, out); }
            else if path.extension().is_some_and(|e| e == "cpp") {
                out.push(path.strip_prefix(repo).unwrap_or(&path).to_string_lossy().into_owned());
            }
        }
    }
    let mut out = Vec::new();
    walk(&repo.join("Source/Injector"), repo, &mut out);
    out.sort();
    out
}

/// A path given on the command line, as repo-relative.
fn repo_relative(repo: &Path, given: &Path) -> Result<String, String> {
    let absolute = if given.is_absolute() { given.to_path_buf() } else {
        std::env::current_dir().map_err(|e| e.to_string())?.join(given)
    };
    let absolute = absolute.canonicalize().map_err(|e| format!("file_missing: {}: {e}", given.display()))?;
    let repo = repo.canonicalize().map_err(|e| e.to_string())?;
    absolute.strip_prefix(&repo).map(|p| p.to_string_lossy().into_owned())
        .map_err(|_| format!("file_outside_repo: {}", given.display()))
}

pub fn check(env: &Environment, files: &[PathBuf], all: bool, self_test: bool) -> Result<(), String> {
    let repo = env.repo_root.clone().ok_or("repo_missing: repositório não encontrado")?;
    if Command::new(COMPILER).arg("--version").output().map(|o| !o.status.success()).unwrap_or(true) {
        return Err(format!("mingw_missing: {COMPILER} não está instalado; sem ele não há verificação"));
    }
    let checker = Checker::new(repo.join("Source"))?;
    let to_job = |rel_repo: &str, content: String| Job {
        rel: rel_repo.trim_start_matches("Source/").to_owned(), baseline: head_version(&repo, rel_repo), content,
    };

    if self_test {
        // The positive signal: a line broken on purpose has to come back as an
        // error, at that line.
        let rel = "Source/Injector/Hooks/DarkSouls2/DS2_CrashHook.cpp";
        let original = std::fs::read_to_string(repo.join(rel)).map_err(|e| format!("file_missing: {rel}: {e}"))?;
        let broken_line = original.lines().count() as u32 + 1;
        let broken = format!("{original}{}int ds2os_self_test = ds2os_undeclared_for_self_test;\n",
            if original.ends_with('\n') { "" } else { "\n" });
        let clean = run_jobs(&checker, &[to_job(rel, original)]).remove(0);
        let result = run_jobs(&checker, &[to_job(rel, broken)]).remove(0);
        let caught = result.errors.iter().any(|d| d.line == broken_line && d.message.contains("ds2os_undeclared_for_self_test"));
        output::data(json!({"file": rel, "clean": clean, "broken": result, "brokenLine": broken_line, "caught": caught}));
        line(format_args!("limpo: {}; quebrado na linha {broken_line}: {} erro(s) novo(s)", clean.status, result.errors.len()));
        if clean.status != "ok" { return Err(format!("self_test_failed: a versão intacta de {rel} não passou: {:?}", clean.note)); }
        if !caught { return Err(format!("self_test_failed: o erro plantado na linha {broken_line} não foi apontado")); }
        line(format_args!("autoteste ok: o erro plantado foi apontado na linha certa"));
        return Ok(());
    }

    let selected: Vec<String> = if all { all_files(&repo) }
        else if !files.is_empty() { files.iter().map(|f| repo_relative(&repo, f)).collect::<Result<_, _>>()? }
        else { changed_files(&repo)? };
    if selected.is_empty() {
        output::data(json!({"files": [], "scope": "changed"}));
        output::outcome("inconclusive");
        return Err("nothing_to_check: nenhum .cpp do injector mudou desde HEAD; passe arquivos ou --all".into());
    }
    let mut jobs = Vec::new();
    for rel in &selected {
        if !rel.ends_with(".cpp") { return Err(format!("not_a_source: {rel} não é .cpp")); }
        let content = std::fs::read_to_string(repo.join(rel)).map_err(|e| format!("file_missing: {rel}: {e}"))?;
        jobs.push(to_job(rel, content));
    }
    let started = std::time::Instant::now();
    let results = run_jobs(&checker, &jobs);
    crate::control::check()?;

    for r in &results {
        line(format_args!("{:6} {}{}", r.status, r.file, r.note.as_deref().map(|n| format!("  ({n})")).unwrap_or_default()));
        for d in &r.errors { line(format_args!("         {}:{}: {}", r.file, d.line, d.message)); }
    }
    let with_errors = results.iter().filter(|r| r.status == "errors").count();
    let failed = results.iter().filter(|r| r.status == "failed").count();
    let total_errors: usize = results.iter().map(|r| r.errors.len()).sum();
    output::data(json!({
        "what": "sintaxe (mingw), não compilação (MSVC)",
        "scope": if all { "all" } else if files.is_empty() { "changed" } else { "given" },
        "files": results, "elapsedMs": started.elapsed().as_millis(),
    }));
    if with_errors > 0 {
        return Err(format!("syntax_errors: {total_errors} erro(s) novo(s) em {with_errors} arquivo(s)"));
    }
    if failed > 0 {
        output::outcome("inconclusive");
        return Err(format!("check_failed: o compilador não respondeu para {failed} arquivo(s)"));
    }
    line(format_args!("sintaxe ok em {} arquivo(s) (mingw; não é compilação MSVC)", results.len()));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const LISTED: &str = r#"[
      {"conclusion":"","createdAt":"2026-09-14T15:02:10Z","databaseId":34902259160,"displayTitle":"harness","event":"push","headBranch":"feature/seamless-coop","headSha":"2674517196f038e8e4cea228a3a725f889f315db","status":"in_progress"},
      {"conclusion":"success","createdAt":"2026-09-14T12:40:00Z","databaseId":34899643493,"displayTitle":"nav","event":"push","headBranch":"feature/seamless-coop","headSha":"00a685564382e2cde63e06029bfde41bd88653de","status":"completed"},
      {"conclusion":"success","createdAt":"2026-09-13T20:00:00Z","databaseId":34867052288,"displayTitle":"old","event":"push","headBranch":"feature/seamless-coop","headSha":"8c278604ecbea940c7033f151b647af38f13d5a2","status":"completed"}
    ]"#;

    fn runs() -> Vec<Run> { serde_json::from_str(LISTED).unwrap() }

    #[test]
    fn the_default_is_the_newest_run_with_heads_injector_code() {
        let same = |sha: &str| sha.starts_with("00a6") || sha.starts_with("8c27");
        let choice = select(&runs(), Pick::SameCode, &same).unwrap();
        assert_eq!(choice.run.database_id, 34899643493);
        assert!(choice.same_code);
        // The in-progress run is chosen when it is the one with HEAD's code: fetch waits for it.
        let choice = select(&runs(), Pick::SameCode, &|sha| sha.starts_with("2674")).unwrap();
        assert_eq!(choice.run.status, "in_progress");
        assert!(select(&runs(), Pick::SameCode, &|_| false).unwrap_err().starts_with("no_run_for_head:"));
    }

    #[test]
    fn latest_and_explicit_runs_say_when_they_are_not_heads_code() {
        let choice = select(&runs(), Pick::Latest, &|sha| sha.starts_with("00a6")).unwrap();
        assert_eq!(choice.run.database_id, 34902259160);
        assert!(!choice.same_code);
        let choice = select(&runs(), Pick::Run(34867052288), &|_| true).unwrap();
        assert_eq!(choice.run.head_sha, "8c278604ecbea940c7033f151b647af38f13d5a2");
        assert!(select(&runs(), Pick::Run(1), &|_| true).unwrap_err().starts_with("run_not_found:"));
        assert!(select(&[], Pick::Latest, &|_| true).unwrap_err().starts_with("no_run:"));
    }

    #[test]
    fn rotation_keeps_one_previous_directory() {
        let root = std::env::temp_dir().join(format!("ds2os-rotate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dir = root.join("injector");
        let new = sibling(&dir, ".new");
        std::fs::create_dir_all(&new).unwrap();
        std::fs::write(new.join("Injector.dll"), "first").unwrap();
        assert_eq!(rotate(&new, &dir).unwrap(), None);
        assert_eq!(std::fs::read_to_string(dir.join("Injector.dll")).unwrap(), "first");

        for (content, prev) in [("second", "first"), ("third", "second")] {
            std::fs::create_dir_all(&new).unwrap();
            std::fs::write(new.join("Injector.dll"), content).unwrap();
            assert_eq!(rotate(&new, &dir).unwrap(), Some(root.join("injector.prev")));
            assert_eq!(std::fs::read_to_string(dir.join("Injector.dll")).unwrap(), content);
            assert_eq!(std::fs::read_to_string(root.join("injector.prev/Injector.dll")).unwrap(), prev);
            assert!(!new.exists());
        }
        // Without a new directory nothing moves.
        assert!(rotate(&new, &dir).is_err());
        assert_eq!(std::fs::read_to_string(dir.join("Injector.dll")).unwrap(), "third");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn binaries_must_be_pe_images() {
        let root = std::env::temp_dir().join(format!("ds2os-verify-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join("Injector.dll"), b"MZ\x90\x00").unwrap();
        assert!(verify_binaries(&root).unwrap_err().starts_with("artifact_incomplete:"));
        std::fs::write(root.join("Injector.exe"), b"<html>").unwrap();
        assert!(verify_binaries(&root).unwrap_err().starts_with("artifact_invalid:"));
        std::fs::write(root.join("Injector.exe"), b"MZ").unwrap();
        assert_eq!(verify_binaries(&root).unwrap().len(), 2);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn the_running_build_is_compared_to_the_manifest() {
        let sha = "2674517196f038e8e4cea228a3a725f889f315db";
        assert_eq!(compare_build(Some(sha), Some(sha), true), Build::Current);
        assert_eq!(compare_build(Some(&sha.to_uppercase()), Some(sha), true), Build::Current);
        assert_eq!(compare_build(Some("00a685564382e2cde63e06029bfde41bd88653de"), Some(sha), true), Build::Other);
        assert_eq!(compare_build(Some("unknown"), Some(sha), true), Build::Unknown);
        assert_eq!(compare_build(None, Some(sha), true), Build::Unrecorded);
        assert_eq!(compare_build(Some(sha), None, true), Build::NoReference);
        assert_eq!(compare_build(None, Some(sha), false), Build::NoReference);
    }

    #[test]
    fn a_running_game_with_another_build_needs_a_relaunch() {
        let state = |equals_source, running, build_state| InstallState {
            instance: 1, installed_sha256: None, equals_source, running, boot_id: None, build: None, build_state };
        assert_eq!(needs(&[state(true, false, Build::NoReference), state(true, false, Build::NoReference)]), (false, false));
        assert_eq!(needs(&[state(false, false, Build::NoReference)]), (true, false));
        assert_eq!(needs(&[state(true, true, Build::Current)]), (false, false));
        // Installed already, but the open game booted the old DLL.
        assert_eq!(needs(&[state(true, true, Build::Other)]), (false, true));
        assert_eq!(needs(&[state(false, true, Build::Unrecorded)]), (true, true));
    }

    #[test]
    fn seh_is_rewritten_without_moving_lines() {
        let src = "void f() {\n    __try {\n        g();\n    }\n    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION\n        ? (h(1), EXCEPTION_EXECUTE_HANDLER) : EXCEPTION_CONTINUE_SEARCH) {\n        k(_ReturnAddress());\n    }\n    int __trying = my__try;\n}\n";
        let out = msvc_rewrite(src);
        assert_eq!(out.lines().count(), src.lines().count());
        assert!(out.contains("    if (1) {"));
        assert!(out.contains("    else\n {"), "{out}");
        assert!(out.contains("k(__builtin_return_address(0));"));
        assert!(out.contains("int __trying = my__try;"));
        assert!(!out.contains("GetExceptionCode"));
        assert_eq!(out.lines().position(|l| l.contains("k(")), src.lines().position(|l| l.contains("k(")));
    }

    #[test]
    fn only_errors_head_does_not_have_count() {
        let copy = "/tmp/x/copies/0w-Injector_Hooks_A.cpp";
        let stderr = format!("{copy}:12:5: error: 'foo' was not declared in this scope\n{copy}:40:1: error: expected ';' before '}}' token\n\
Injector/Hooks/A.h:3:9: error: redefinition of 'x'\n{copy}: In function 'void f()':\n{copy}:12:5: note: suggested alternative\n");
        let current = parse_errors(&stderr, copy);
        assert_eq!(current, vec![
            Diagnostic { line: 12, message: "'foo' was not declared in this scope".into() },
            Diagnostic { line: 40, message: "expected ';' before '}' token".into() },
            Diagnostic { line: 0, message: "Injector/Hooks/A.h:3: redefinition of 'x'".into() },
        ]);
        // The same message at another line in HEAD's version is not new.
        let head = vec![Diagnostic { line: 30, message: "expected ';' before '}' token".into() }];
        let fresh = new_errors(&current, &head);
        assert_eq!(fresh.len(), 2);
        assert_eq!(fresh[0].line, 12);
    }
}
