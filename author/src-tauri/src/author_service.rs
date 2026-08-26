// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// AuthorService: the only place that touches the `musicpack` CLI.
//
// MusicPack Author does not reimplement any .mpack semantics; it calls the
// existing `musicpack` implementation through its JSON modes. This service
// spawns the CLI, parses the structured JSON responses and maps failures to
// typed errors. The interface is deliberately thin so a direct libmusicpack
// binding can replace the subprocess later without touching the frontend.
//
// Backend resolution is split into two explicit regimes (BackendLocation):
//   - Bundled: the sidecar shipped inside the application bundle. Packaged
//     apps locate it through Tauri's runtime path API and NEVER fall back to
//     a `musicpack` from PATH or from a build tree.
//   - Development: MUSICPACK_CLI -> build tree -> PATH, for `tauri dev`.

use crate::musicbrainz::{self, MbError};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

/// Authoring JSON surface version the GUI expects from the backend.
/// Bump together with the CLI's `MUSICPACK_AUTHOR_API`.
/// Version 2 adds `encode-draft`; version 3 removes its `--ffmpeg` argument
/// (FLAC/WAV decode is native); version 4 moves MusicBrainz transport here;
/// version 6 adds package open (`inspect` on an .mpack) and in-place save
/// (`build-draft --replace --sync-tags`); version 7 keeps track
/// `representations[]` in the draft so Author saves preserve the Phase 3
/// alternates.
const EXPECTED_AUTHOR_API: u32 = 7;

#[derive(Debug, Clone)]
pub enum AuthorError {
    CliNotFound(String),
    Io(String),
    CliFailure {
        code: Option<String>,
        message: String,
    },
    Output(String),
    MusicBrainz(MbError),
    IncompatibleBackend {
        expected: u32,
        found: u32,
        version: String,
    },
}

impl std::fmt::Display for AuthorError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            AuthorError::CliNotFound(msg) => write!(f, "{msg}"),
            AuthorError::Io(msg) => write!(f, "{msg}"),
            AuthorError::CliFailure {
                code: Some(code),
                message,
            } => write!(f, "[{code}] {message}"),
            AuthorError::CliFailure {
                code: None,
                message,
            } => write!(f, "{message}"),
            AuthorError::Output(msg) => write!(f, "{msg}"),
            AuthorError::MusicBrainz(error) => write!(f, "{error}"),
            AuthorError::IncompatibleBackend {
                expected,
                found,
                version,
            } => write!(
                f,
                "incompatible authoring backend: {version} speaks author API {found}, \
                 MusicPack Author requires {expected}; update the bundled backend"
            ),
        }
    }
}

impl std::error::Error for AuthorError {}

/// Where the authoring backend lives. Packaged and development resolution are
/// deliberately distinct: a packaged app must not silently pick up an
/// unrelated `musicpack` from the user's environment.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BackendLocation {
    /// The sidecar shipped inside the .app bundle.
    Bundled(PathBuf),
    /// A development binary (MUSICPACK_CLI, build tree, or PATH).
    Development(PathBuf),
}

impl BackendLocation {
    pub fn path(&self) -> &Path {
        match self {
            BackendLocation::Bundled(p) | BackendLocation::Development(p) => p,
        }
    }

    pub fn kind(&self) -> &'static str {
        match self {
            BackendLocation::Bundled(_) => "bundled",
            BackendLocation::Development(_) => "development",
        }
    }
}

/// Capability handshake reported by `musicpack author-api-version --json`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AuthorApiInfo {
    pub musicpack_version: String,
    pub author_api: u32,
}

impl AuthorApiInfo {
    pub fn verify(&self) -> Result<(), AuthorError> {
        if self.author_api == EXPECTED_AUTHOR_API {
            Ok(())
        } else {
            Err(AuthorError::IncompatibleBackend {
                expected: EXPECTED_AUTHOR_API,
                found: self.author_api,
                version: self.musicpack_version.clone(),
            })
        }
    }
}

/// Structured backend facts surfaced to the frontend.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BackendInfo {
    pub musicpack_version: String,
    pub author_api: u32,
    pub location: String,
}

pub struct AuthorService {
    location: Result<BackendLocation, AuthorError>,
    handshake: Option<Result<AuthorApiInfo, AuthorError>>,
    counter: std::sync::atomic::AtomicU64,
    last_mb_request: Option<Instant>,
}

impl AuthorService {
    /// `location` is the outcome of `resolve_bundled`/`resolve_development`;
    /// an `Err` is kept so the app still starts and commands surface the
    /// actionable resolution error instead of crashing at launch.
    pub fn new(location: Result<BackendLocation, AuthorError>) -> Self {
        AuthorService {
            location,
            handshake: None,
            counter: std::sync::atomic::AtomicU64::new(0),
            last_mb_request: None,
        }
    }

    /// Packaged-app resolution: the backend sidecar next to the bundle's
    /// executable. Never consults PATH or the build tree.
    pub fn resolve_bundled(sidecar: &Path) -> Result<BackendLocation, AuthorError> {
        if sidecar.is_file() {
            Ok(BackendLocation::Bundled(sidecar.to_path_buf()))
        } else {
            Err(AuthorError::CliNotFound(format!(
                "bundled authoring backend not found at {}; \
                 reinstall MusicPack Author",
                sidecar.display()
            )))
        }
    }

    /// Development resolution: MUSICPACK_CLI, then the CMake build trees next
    /// to the repository, then `musicpack` on PATH.
    pub fn resolve_development(
        env: Option<&str>,
        repo_base: &Path,
        on_path: impl Fn() -> bool,
    ) -> Result<BackendLocation, AuthorError> {
        if let Some(p) = env.map(str::trim).filter(|s| !s.is_empty()) {
            return Ok(BackendLocation::Development(PathBuf::from(p)));
        }
        for rel in [
            "build/musicpack/musicpack",
            "build-static/musicpack/musicpack",
        ] {
            let candidate = repo_base.join(rel);
            if candidate.is_file() {
                return Ok(BackendLocation::Development(candidate));
            }
        }
        if on_path() {
            return Ok(BackendLocation::Development(PathBuf::from("musicpack")));
        }
        Err(AuthorError::CliNotFound(
            "cannot find the `musicpack` CLI; build it with \
             `cmake --build build -j --target musicpack_cmd` or set MUSICPACK_CLI"
                .to_string(),
        ))
    }

    /// Whether the backend is usable, and the actionable error otherwise.
    fn ensure_ready(&self) -> Result<(), AuthorError> {
        match &self.location {
            Ok(_) => Ok(()),
            Err(e) => Err(e.clone()),
        }
    }

    fn cli_path(&self) -> &Path {
        self.location
            .as_ref()
            .expect("cli_path only called after ensure_ready")
            .path()
    }

    /// Runs `musicpack author-api-version` once and caches the verdict.
    /// Every backend operation funnels through here, so an incompatible or
    /// missing backend fails fast with a clear error.
    pub fn ensure_handshake(&mut self) -> Result<AuthorApiInfo, AuthorError> {
        if let Some(result) = &self.handshake {
            return result.clone();
        }
        let result = self.fetch_handshake();
        self.handshake = Some(result.clone());
        result
    }

    fn fetch_handshake(&self) -> Result<AuthorApiInfo, AuthorError> {
        self.ensure_ready()?;
        let output = Command::new(self.cli_path())
            .arg("author-api-version")
            .output()
            .map_err(|e| AuthorError::Io(format!("cannot run `musicpack`: {e}")))?;
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(AuthorError::CliFailure {
                code: None,
                message: if stderr.trim().is_empty() {
                    "backend does not support author-api-version".to_string()
                } else {
                    stderr.trim().to_string()
                },
            });
        }
        Self::parse_author_api(&String::from_utf8_lossy(&output.stdout))
    }

    /// Pure parser + gate over `author-api-version --json` stdout.
    fn parse_author_api(stdout: &str) -> Result<AuthorApiInfo, AuthorError> {
        let value: Value = serde_json::from_str(stdout.trim()).map_err(|e| {
            AuthorError::Output(format!("backend handshake returned invalid JSON: {e}"))
        })?;
        let info = AuthorApiInfo {
            musicpack_version: value
                .get("musicpackVersion")
                .and_then(|s| s.as_str())
                .unwrap_or_default()
                .to_string(),
            author_api: value.get("authorApi").and_then(|n| n.as_u64()).unwrap_or(0) as u32,
        };
        info.verify()?;
        Ok(info)
    }

    pub fn backend_info(&mut self) -> Result<BackendInfo, AuthorError> {
        let info = self.ensure_handshake()?;
        Ok(BackendInfo {
            musicpack_version: info.musicpack_version,
            author_api: info.author_api,
            location: self
                .location
                .as_ref()
                .map(|l| l.kind().to_string())
                .unwrap_or_else(|_| "unresolved".to_string()),
        })
    }

    fn temp_file(&self, name: &str, contents: &str) -> Result<PathBuf, AuthorError> {
        let n = self
            .counter
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "musicpack-author-{}-{n}-{name}",
            std::process::id()
        ));
        let mut f = std::fs::File::create(&path)
            .map_err(|e| AuthorError::Io(format!("cannot create temp file: {e}")))?;
        f.write_all(contents.as_bytes())
            .map_err(|e| AuthorError::Io(format!("cannot write temp file: {e}")))?;
        f.flush()
            .map_err(|e| AuthorError::Io(format!("cannot flush temp file: {e}")))?;
        Ok(path)
    }

    /// Runs the CLI with `args` and parses the JSON document on stdout.
    fn run_json(&self, args: &[&str]) -> Result<Value, AuthorError> {
        let output = Command::new(self.cli_path())
            .args(args)
            .output()
            .map_err(|e| AuthorError::Io(format!("cannot run `musicpack`: {e}")))?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        if !output.status.success() {
            // Prefer the CLI's JSON error envelope, then its stderr.
            if let Ok(v) = serde_json::from_str::<Value>(stdout.trim()) {
                if let Some(e) = v.get("error") {
                    let code = e
                        .get("code")
                        .and_then(|c| c.as_str())
                        .map(|s| s.to_string());
                    let message = e
                        .get("message")
                        .and_then(|m| m.as_str())
                        .map(|s| s.to_string())
                        .unwrap_or_else(|| "unknown CLI error".to_string());
                    return Err(AuthorError::CliFailure { code, message });
                }
            }
            let detail = if stderr.trim().is_empty() {
                stdout.trim().to_string()
            } else {
                stderr.trim().to_string()
            };
            return Err(AuthorError::CliFailure {
                code: None,
                message: if detail.is_empty() {
                    "musicpack command failed".to_string()
                } else {
                    detail
                },
            });
        }
        serde_json::from_str(stdout.trim())
            .map_err(|e| AuthorError::Output(format!("CLI returned non-JSON output: {e}")))
    }

    fn pace_musicbrainz(&mut self) {
        const MINIMUM_INTERVAL: Duration = Duration::from_secs(1);
        if let Some(last) = self.last_mb_request {
            if let Some(remaining) = MINIMUM_INTERVAL.checked_sub(last.elapsed()) {
                std::thread::sleep(remaining);
            }
        }
        self.last_mb_request = Some(Instant::now());
    }

    fn identify_result(value: Value) -> Result<Value, AuthorError> {
        if let Some(candidates) = value.get("candidates") {
            return Ok(serde_json::json!({
                "kind": "candidates",
                "candidates": candidates,
            }));
        }
        if value.get("draft").is_some()
            && value.get("confidence").is_some()
            && value.get("applied").is_some()
        {
            return Ok(serde_json::json!({
                "kind": "applied",
                "draft": value["draft"],
                "confidence": value["confidence"],
                "applied": value["applied"],
            }));
        }
        Err(AuthorError::Output(
            "backend returned an invalid identify response".to_string(),
        ))
    }

    // ---- operations ---------------------------------------------------

    pub fn inspect_album(&mut self, path: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        self.run_json(&["inspect", path, "--json"])
    }

    pub fn validate_draft(&mut self, draft_json: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        let tmp = self.temp_file("draft.json", draft_json)?;
        let result = self.run_json(&[
            "validate-draft",
            "--draft",
            tmp.to_str().unwrap_or(""),
            "--json",
        ]);
        let _ = std::fs::remove_file(&tmp);
        result
    }

    pub fn identify_draft(
        &mut self,
        draft_json: &str,
        mbid: Option<&str>,
        barcode: Option<&str>,
        mb_json: Option<&str>,
    ) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        let tmp = self.temp_file("draft.json", draft_json)?;
        let mut args: Vec<String> = vec![
            "identify-draft".to_string(),
            "--draft".to_string(),
            tmp.to_string_lossy().into_owned(),
            "--json".to_string(),
        ];
        let mb_tmp = if let Some(id) = mbid {
            self.pace_musicbrainz();
            let response = match musicbrainz::fetch_release(id) {
                Ok(response) => response,
                Err(error) => {
                    let _ = std::fs::remove_file(&tmp);
                    return Err(AuthorError::MusicBrainz(error));
                }
            };
            let mb = match self.temp_file("mb-release.json", &response) {
                Ok(mb) => mb,
                Err(error) => {
                    let _ = std::fs::remove_file(&tmp);
                    return Err(error);
                }
            };
            args.push("--mbid".to_string());
            args.push(id.to_string());
            args.push("--mb-json".to_string());
            args.push(mb.to_string_lossy().into_owned());
            Some(mb)
        } else if let Some(bc) = barcode {
            self.pace_musicbrainz();
            let response = match musicbrainz::fetch_barcode_search(bc) {
                Ok(response) => response,
                Err(error) => {
                    let _ = std::fs::remove_file(&tmp);
                    return Err(AuthorError::MusicBrainz(error));
                }
            };
            let mb = match self.temp_file("mb-search.json", &response) {
                Ok(mb) => mb,
                Err(error) => {
                    let _ = std::fs::remove_file(&tmp);
                    return Err(error);
                }
            };
            args.push("--mb-search-json".to_string());
            args.push(mb.to_string_lossy().into_owned());
            Some(mb)
        } else if let Some(json) = mb_json {
            let mb = match self.temp_file("mb-release.json", json) {
                Ok(mb) => mb,
                Err(error) => {
                    let _ = std::fs::remove_file(&tmp);
                    return Err(error);
                }
            };
            args.push("--mb-json".to_string());
            args.push(mb.to_string_lossy().into_owned());
            Some(mb)
        } else {
            None
        };
        let refs: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
        let result = self.run_json(&refs).and_then(Self::identify_result);
        if let Some(mb) = mb_tmp {
            let _ = std::fs::remove_file(mb);
        }
        let _ = std::fs::remove_file(&tmp);
        result
    }

    pub fn create_package(
        &mut self,
        draft_json: &str,
        output_dir: &str,
        replace: bool,
        sync_tags: bool,
    ) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        let tmp = self.temp_file("draft.json", draft_json)?;
        let mut args: Vec<String> = vec![
            "build-draft".to_string(),
            "--draft".to_string(),
            tmp.to_string_lossy().into_owned(),
            "-o".to_string(),
            output_dir.to_string(),
        ];
        if replace {
            args.push("--replace".to_string());
        }
        if sync_tags {
            args.push("--sync-tags".to_string());
        }
        args.push("--json".to_string());
        let refs: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
        let result = self.run_json(&refs);
        let _ = std::fs::remove_file(&tmp);
        result
    }

    pub fn verify_package(&mut self, path: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        self.run_json(&["verify", path, "--json"])
    }

    // ---- Sonic analysis ----------------------------------------------

    /// Resolves the `musicpack-sonic` analyzer binary. Bundled apps use the
    /// sidecar next to the CLI; development uses MUSICPACK_SONIC, then the
    /// CMake build tree. Like the CLI, analyzer selection is trusted
    /// configuration — a package-provided profile never triggers it.
    fn sonic_resolve(&self) -> Result<PathBuf, AuthorError> {
        match &self.location {
            Ok(BackendLocation::Bundled(cli)) => {
                let sonic = cli
                    .parent()
                    .unwrap_or_else(|| Path::new(""))
                    .join("musicpack-sonic");
                if sonic.is_file() {
                    Ok(sonic)
                } else {
                    Err(AuthorError::CliNotFound(format!(
                        "bundled sonic analyzer not found at {}; reinstall MusicPack Author",
                        sonic.display()
                    )))
                }
            }
            Ok(BackendLocation::Development(cli)) => {
                if let Ok(p) = std::env::var("MUSICPACK_SONIC") {
                    let p = PathBuf::from(p.trim());
                    if !p.as_os_str().is_empty() && p.is_file() {
                        return Ok(p);
                    }
                }
                let base = cli.parent().unwrap_or_else(|| Path::new("."));
                for cand in [
                    base.join("../sonic/musicpack-sonic"),
                    base.join("sonic/musicpack-sonic"),
                ] {
                    if cand.is_file() {
                        return Ok(cand);
                    }
                }
                Err(AuthorError::CliNotFound(
                    "cannot find the `musicpack-sonic` analyzer; build it with \
                     `cmake --build build -j --target musicpack_sonic_cmd \
                     -DSONIC_ONNXRUNTIME_DIR=<onnxruntime>` or set MUSICPACK_SONIC"
                        .to_string(),
                ))
            }
            Err(e) => Err(e.clone()),
        }
    }

    /// Spawns the sonic analyzer with a job document file. Returns the child
    /// (stdout piped, for progress events) and the job temp path (removed by
    /// the caller). The child handle is kept for cancellation.
    pub fn sonic_spawn(
        &self,
        job_json: &str,
    ) -> Result<(std::process::Child, PathBuf), AuthorError> {
        let sonic = self.sonic_resolve()?;
        let job_tmp = self.temp_file("sonic-job.json", job_json)?;
        let child = Command::new(sonic)
            .arg(&job_tmp)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| {
                let _ = std::fs::remove_file(&job_tmp);
                AuthorError::Io(format!("cannot run `musicpack-sonic`: {e}"))
            })?;
        Ok((child, job_tmp))
    }

    // ---- Waveform envelope generation -------------------------------

    /// Spawns the `musicpack` CLI's `waveform-draft` subcommand with the
    /// given draft JSON. Returns the child handle (for cancellation) plus its
    /// temporary draft path, which the caller removes after the child exits.
    pub fn waveform_spawn(
        &self,
        draft_json: &str,
        staging: &Path,
    ) -> Result<(std::process::Child, PathBuf), AuthorError> {
        let cli = self.cli_path().to_path_buf();
        let draft_tmp = self.temp_file("waveform-draft.json", draft_json)?;
        let child = Command::new(cli)
            .arg("waveform-draft")
            .arg("--draft")
            .arg(&draft_tmp)
            .arg("-o")
            .arg(staging)
            .arg("--json")
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| {
                let _ = std::fs::remove_file(&draft_tmp);
                AuthorError::Io(format!("cannot run `musicpack waveform-draft`: {e}"))
            })?;
        Ok((child, draft_tmp))
    }

    // ---- FLAC -> Musepack encoding ----------------------------------

    /// Resolves the `mpcenc` encoder binary. Bundled apps use the sidecar
    /// next to the CLI; development uses MUSICPACK_MPCENC, the CMake build
    /// tree, then PATH. Like the CLI, encoder selection is trusted
    /// configuration — the GUI never discovers an encoder from package data.
    /// FLAC/WAV sources are decoded natively by the bundled backend, so no
    /// external decoder is resolved here.
    pub fn encode_resolve_mpcenc(&self) -> Result<PathBuf, AuthorError> {
        match &self.location {
            Ok(BackendLocation::Bundled(cli)) => {
                let p = cli.parent().unwrap_or_else(|| Path::new("")).join("mpcenc");
                if p.is_file() {
                    Ok(p)
                } else {
                    Err(AuthorError::CliNotFound(format!(
                        "bundled mpcenc not found at {}; reinstall MusicPack Author",
                        p.display()
                    )))
                }
            }
            Ok(BackendLocation::Development(cli)) => {
                if let Ok(v) = std::env::var("MUSICPACK_MPCENC") {
                    let p = PathBuf::from(v.trim());
                    if !p.as_os_str().is_empty() && p.is_file() {
                        return Ok(p);
                    }
                }
                let base = cli.parent().unwrap_or_else(|| Path::new("."));
                for cand in [base.join("../mpcenc/mpcenc"), base.join("mpcenc/mpcenc")] {
                    if cand.is_file() {
                        return Ok(cand);
                    }
                }
                if command_on_path("mpcenc") {
                    return Ok(PathBuf::from("mpcenc"));
                }
                Err(AuthorError::CliNotFound(
                    "cannot find `mpcenc`; build it with `cmake --build build -j \
                     --target mpcenc` or set MUSICPACK_MPCENC"
                        .to_string(),
                ))
            }
            Err(e) => Err(e.clone()),
        }
    }

    /// Spawns `musicpack encode-draft` for the current draft. Returns the
    /// child (stdout piped, for progress events) and the draft temp path
    /// (removed by the caller). The child handle is kept for cancellation.
    /// The bundled backend decodes FLAC/WAV sources natively, so no external
    /// decoder is passed.
    pub fn encode_spawn(
        &self,
        draft_json: &str,
        staging: &Path,
        quality: &str,
        mpcenc: &Path,
    ) -> Result<(std::process::Child, PathBuf), AuthorError> {
        let draft_tmp = self.temp_file("encode-draft.json", draft_json)?;
        let child = Command::new(self.cli_path())
            .arg("encode-draft")
            .arg("--draft")
            .arg(&draft_tmp)
            .arg("-o")
            .arg(staging)
            .arg("--quality")
            .arg(quality)
            .arg("--mpcenc")
            .arg(mpcenc)
            .arg("--json")
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| {
                let _ = std::fs::remove_file(&draft_tmp);
                AuthorError::Io(format!("cannot run `musicpack encode-draft`: {e}"))
            })?;
        Ok((child, draft_tmp))
    }

    /// Creates a fresh staging directory for an encode run. The GUI owns it
    /// until the run finishes; successful runs keep it for the build step,
    /// failures/cancels remove it (see `cleanup_staging`).
    pub fn encode_staging_dir(&self) -> Result<PathBuf, AuthorError> {
        for _ in 0..100 {
            let n = self
                .counter
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let dir = std::env::temp_dir().join(format!(
                "musicpack-author-encode-{}-{n}",
                std::process::id()
            ));
            match std::fs::create_dir(&dir) {
                Ok(()) => return Ok(dir),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => {
                    return Err(AuthorError::Io(format!(
                        "cannot create staging directory: {error}"
                    )))
                }
            }
        }
        Err(AuthorError::Io(
            "cannot allocate a fresh staging directory".to_string(),
        ))
    }

    /// Removes a staging directory created by `encode_staging_dir`. Refuses
    /// to delete anything that is not an Author-created staging directory
    /// (defense in depth against a stray path from the frontend).
    pub fn cleanup_staging(&self, dir: &str) -> Result<(), AuthorError> {
        let d = PathBuf::from(dir);
        let temp = std::fs::canonicalize(std::env::temp_dir())
            .map_err(|e| AuthorError::Io(format!("cannot resolve temporary directory: {e}")))?;
        let parent = d.parent().and_then(|p| std::fs::canonicalize(p).ok());
        let prefix = format!("musicpack-author-encode-{}", std::process::id());
        let name = d
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        if !name.starts_with(&prefix) || parent.as_deref() != Some(temp.as_path()) {
            return Err(AuthorError::Output(format!(
                "refusing to remove {dir}: not a MusicPack Author staging directory"
            )));
        }
        std::fs::remove_dir_all(&d)
            .map_err(|e| AuthorError::Io(format!("cannot remove staging directory: {e}")))?;
        Ok(())
    }
}

/// Whether `name` resolves to an executable on PATH (dev-mode discovery only;
/// packaged apps never consult PATH).
fn command_on_path(name: &str) -> bool {
    match Command::new("which").arg(name).output() {
        Ok(o) => o.status.success(),
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use tempfile::TempDir;

    fn no_path() -> bool {
        false
    }

    fn on_path() -> bool {
        true
    }

    fn make_cli(tmp: &Path, name: &str, script: &str) -> PathBuf {
        let dir = tmp.join(name);
        fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("musicpack");
        fs::write(&bin, script).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&bin).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&bin, perms).unwrap();
        }
        bin
    }

    #[test]
    fn identify_results_use_the_frontend_discriminator() {
        let applied = AuthorService::identify_result(serde_json::json!({
            "draft": {"album": {}},
            "confidence": "exact",
            "applied": true,
        }))
        .unwrap();
        assert_eq!(applied["kind"], "applied");
        let candidates = AuthorService::identify_result(serde_json::json!({
            "candidates": [{"releaseId": "id"}],
        }))
        .unwrap();
        assert_eq!(candidates["kind"], "candidates");
    }

    // ---- backend resolution ----

    #[test]
    fn dev_uses_musicpack_cli_override() {
        let tmp = TempDir::new().unwrap();
        let env = "/some/where/musicpack";
        let loc = AuthorService::resolve_development(Some(env), tmp.path(), no_path).unwrap();
        assert!(matches!(
            &loc,
            BackendLocation::Development(p) if p == Path::new("/some/where/musicpack")
        ));
    }

    #[test]
    fn dev_ignores_blank_override() {
        let tmp = TempDir::new().unwrap();
        let loc = AuthorService::resolve_development(Some("   "), tmp.path(), no_path).unwrap_err();
        assert!(matches!(loc, AuthorError::CliNotFound(_)));
    }

    #[test]
    fn dev_prefers_build_tree_over_path() {
        let tmp = TempDir::new().unwrap();
        make_cli(tmp.path(), "build/musicpack", "#!/bin/sh\n");
        let loc = AuthorService::resolve_development(None, tmp.path(), on_path).unwrap();
        assert!(matches!(
            &loc,
            BackendLocation::Development(p) if p.ends_with("build/musicpack/musicpack")
        ));
    }

    #[test]
    fn dev_falls_back_to_build_static_tree() {
        let tmp = TempDir::new().unwrap();
        make_cli(tmp.path(), "build-static/musicpack", "#!/bin/sh\n");
        let loc = AuthorService::resolve_development(None, tmp.path(), no_path).unwrap();
        assert!(matches!(
            &loc,
            BackendLocation::Development(p) if p.ends_with("build-static/musicpack/musicpack")
        ));
    }

    #[test]
    fn dev_falls_back_to_path() {
        let tmp = TempDir::new().unwrap();
        let loc = AuthorService::resolve_development(None, tmp.path(), on_path).unwrap();
        assert_eq!(loc.path(), Path::new("musicpack"));
    }

    #[test]
    fn dev_missing_backend_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let err = AuthorService::resolve_development(None, tmp.path(), no_path).unwrap_err();
        let msg = err.to_string();
        assert!(msg.contains("MUSICPACK_CLI"), "hints at the fix: {msg}");
    }

    #[test]
    fn bundled_resolves_from_sidecar() {
        let tmp = TempDir::new().unwrap();
        let bin = make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let loc = AuthorService::resolve_bundled(&bin).unwrap();
        assert!(matches!(loc, BackendLocation::Bundled(_)));
    }

    #[test]
    fn bundled_missing_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let err = AuthorService::resolve_bundled(&tmp.path().join("MacOS/musicpack")).unwrap_err();
        assert!(err.to_string().contains("reinstall MusicPack Author"));
    }

    #[test]
    fn production_never_consults_env_tree_or_path() {
        // A packaged app must not select a MUSICPACK_CLI override, a build
        // tree binary, or a PATH binary: only the bundled sidecar counts.
        let tmp = TempDir::new().unwrap();
        make_cli(tmp.path(), "build/musicpack", "#!/bin/sh\n");
        let env_bin = make_cli(tmp.path(), "env", "#!/bin/sh\n");
        let missing = tmp.path().join("MacOS/musicpack");
        let err = AuthorService::resolve_bundled(&missing).unwrap_err();
        assert!(matches!(err, AuthorError::CliNotFound(_)));
        // Resolving via the "production" entrypoint cannot hit the build tree
        // even though it exists next to us.
        let resolved = AuthorService::resolve_bundled(&env_bin);
        assert!(matches!(
            resolved,
            Ok(BackendLocation::Bundled(p)) if p == env_bin
        ));
    }

    // ---- author-API handshake ----

    #[test]
    fn handshake_accepts_matching_api() {
        let info =
            AuthorService::parse_author_api("{\"musicpackVersion\":\"0.1.0\",\"authorApi\":7}\n")
                .unwrap();
        assert_eq!(info.author_api, 7);
        assert_eq!(info.musicpack_version, "0.1.0");
    }

    #[test]
    fn handshake_rejects_mismatched_api() {
        let err =
            AuthorService::parse_author_api("{\"musicpackVersion\":\"0.2.0\",\"authorApi\":3}\n")
                .unwrap_err();
        let msg = err.to_string();
        assert!(msg.contains("author API 3"), "{msg}");
        assert!(msg.contains("requires 7"), "{msg}");
    }

    #[test]
    fn handshake_rejects_invalid_json() {
        let err = AuthorService::parse_author_api("not json").unwrap_err();
        assert!(matches!(err, AuthorError::Output(_)));
    }

    #[cfg(unix)]
    #[test]
    fn handshake_runs_a_real_backend_process() {
        let tmp = TempDir::new().unwrap();
        let bin = make_cli(
            tmp.path(),
            "MacOS",
            "#!/bin/sh\nprintf '{\"musicpackVersion\":\"0.1.0\",\"authorApi\":7}\\n'\n",
        );
        let mut service = AuthorService::new(Ok(BackendLocation::Bundled(bin)));
        let info = service.ensure_handshake().unwrap();
        assert_eq!(info.author_api, 7);
        // Cached: a second call still succeeds.
        assert!(service.ensure_handshake().is_ok());
    }

    #[cfg(unix)]
    #[test]
    fn handshake_old_backend_is_an_error() {
        let tmp = TempDir::new().unwrap();
        let bin = make_cli(
            tmp.path(),
            "MacOS",
            "#!/bin/sh\necho \"musicpack: unknown command\" >&2\nexit 2\n",
        );
        let mut service = AuthorService::new(Ok(BackendLocation::Bundled(bin)));
        let err = service.ensure_handshake().unwrap_err();
        assert!(err.to_string().contains("unknown command"));
    }

    // ---- sonic analyzer resolution ----

    fn make_sonic(tmp: &Path, rel: &str) -> PathBuf {
        let dir = tmp.join(rel);
        fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("musicpack-sonic");
        fs::write(&bin, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&bin).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&bin, perms).unwrap();
        }
        bin
    }

    #[test]
    fn sonic_dev_prefers_env_override() {
        let tmp = TempDir::new().unwrap();
        let sonic = make_sonic(tmp.path(), "custom");
        let loc = AuthorService::resolve_development(Some("/p/cli"), tmp.path(), no_path).unwrap();
        let svc = AuthorService::new(Ok(loc));
        std::env::set_var("MUSICPACK_SONIC", &sonic);
        let p = svc.sonic_resolve().unwrap();
        std::env::remove_var("MUSICPACK_SONIC");
        assert_eq!(p, sonic);
    }

    #[test]
    fn sonic_dev_prefers_build_tree() {
        let tmp = TempDir::new().unwrap();
        let sonic = make_sonic(tmp.path(), "build/sonic");
        let cli = make_cli(tmp.path(), "build/musicpack", "#!/bin/sh\n");
        let loc = AuthorService::resolve_development(None, tmp.path(), on_path).unwrap();
        assert_eq!(loc.path(), &cli);
        let svc = AuthorService::new(Ok(loc));
        let p = svc.sonic_resolve().unwrap();
        assert_eq!(
            std::fs::canonicalize(&p).unwrap(),
            std::fs::canonicalize(&sonic).unwrap()
        );
    }

    #[test]
    fn sonic_dev_missing_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let loc = AuthorService::resolve_development(Some("/p/cli"), tmp.path(), no_path).unwrap();
        let svc = AuthorService::new(Ok(loc));
        let err = svc.sonic_resolve().unwrap_err();
        let msg = err.to_string();
        assert!(msg.contains("MUSICPACK_SONIC"), "{msg}");
    }

    #[test]
    fn sonic_bundled_sits_next_to_cli() {
        let tmp = TempDir::new().unwrap();
        make_sonic(tmp.path(), "MacOS");
        let cli = make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli.clone())));
        let p = svc.sonic_resolve().unwrap();
        assert_eq!(p, cli.parent().unwrap().join("musicpack-sonic"));
    }

    #[test]
    fn sonic_bundled_missing_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let cli = make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli)));
        let err = svc.sonic_resolve().unwrap_err();
        assert!(err.to_string().contains("reinstall MusicPack Author"));
    }

    #[cfg(unix)]
    #[test]
    fn sonic_spawn_passes_the_verified_model_dir_through() {
        // A fake analyzer that echoes the modelDir it received in the job.
        let tmp = TempDir::new().unwrap();
        make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let sonic = tmp.path().join("MacOS/musicpack-sonic");
        fs::write(
            &sonic,
            "#!/bin/sh\ngrep -o '\"modelDir\":\"[^\"]*\"' \"$1\"\n",
        )
        .unwrap();
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&sonic).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&sonic, perms).unwrap();
        }
        let cli = tmp.path().join("MacOS/musicpack");
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli)));

        let job = format!(
            r#"{{"profile":"musicpack-sonic-openl3-v1","modelDir":"/verified/models/profile","tracks":[]}}"#
        );
        let (mut child, _tmp) = svc.sonic_spawn(&job).unwrap();
        use std::io::Read;
        let mut out = String::new();
        let child_out = child.stdout.as_mut().unwrap();
        child_out.read_to_string(&mut out).unwrap();
        let _ = child.wait();
        assert!(
            out.contains("\"modelDir\":\"/verified/models/profile\""),
            "analyzer received the verified model dir: {out}"
        );
    }

    #[cfg(unix)]
    #[test]
    fn waveform_spawn_passes_draft_path() {
        // A fake `musicpack` that echoes the --draft file and exits.
        let tmp = TempDir::new().unwrap();
        let cli_dir = tmp.path().join("MacOS");
        fs::create_dir_all(&cli_dir).unwrap();
        let cli = cli_dir.join("musicpack");
        // $3 is the argument after `waveform-draft --draft`.
        fs::write(&cli, "#!/bin/sh\ncat \"$3\"\n").unwrap();
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&cli).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&cli, perms).unwrap();
        }
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli)));

        let staging = tmp.path().join("wf-out");
        let draft = r#"{"sourceRoot":"/music/A","media":[{"disc":1,"tracks":[{"track":1,"audioPath":"01.mpc"}]}]}"#;
        let (mut child, draft_tmp) = svc.waveform_spawn(draft, &staging).unwrap();
        use std::io::Read;
        let mut out = String::new();
        let child_out = child.stdout.as_mut().unwrap();
        child_out.read_to_string(&mut out).unwrap();
        let _ = child.wait();
        assert!(out.contains("\"media\""), "draft was passed by path: {out}");
        assert!(out.contains("01.mpc"), "draft track preserved: {out}");
        let _ = fs::remove_file(draft_tmp);
    }

    // ---- mpcenc resolution + encode spawn -------------------------------

    fn make_mpcenc(tmp: &Path, rel: &str) -> PathBuf {
        let dir = tmp.join(rel);
        fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("mpcenc");
        fs::write(&bin, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(&bin).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&bin, perms).unwrap();
        }
        bin
    }

    #[test]
    fn mpcenc_bundled_sits_next_to_cli() {
        let tmp = TempDir::new().unwrap();
        make_mpcenc(tmp.path(), "MacOS");
        let cli = make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let parent = cli.parent().unwrap().to_path_buf();
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli)));
        let p = svc.encode_resolve_mpcenc().unwrap();
        assert_eq!(p, parent.join("mpcenc"));
    }

    #[test]
    fn mpcenc_bundled_missing_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let cli = make_cli(tmp.path(), "MacOS", "#!/bin/sh\n");
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(cli)));
        let err = svc.encode_resolve_mpcenc().unwrap_err();
        assert!(err.to_string().contains("reinstall MusicPack Author"));
    }

    #[test]
    fn mpcenc_dev_prefers_env_override() {
        let tmp = TempDir::new().unwrap();
        let mpcenc = make_mpcenc(tmp.path(), "custom");
        let loc = AuthorService::resolve_development(Some("/p/cli"), tmp.path(), no_path).unwrap();
        let svc = AuthorService::new(Ok(loc));
        std::env::set_var("MUSICPACK_MPCENC", &mpcenc);
        let p = svc.encode_resolve_mpcenc().unwrap();
        std::env::remove_var("MUSICPACK_MPCENC");
        assert_eq!(p, mpcenc);
    }

    #[test]
    fn mpcenc_dev_prefers_build_tree() {
        let tmp = TempDir::new().unwrap();
        let mpcenc = make_mpcenc(tmp.path(), "build/mpcenc");
        let cli = make_cli(tmp.path(), "build/musicpack", "#!/bin/sh\n");
        let loc = AuthorService::resolve_development(None, tmp.path(), on_path).unwrap();
        assert_eq!(loc.path(), &cli);
        let svc = AuthorService::new(Ok(loc));
        let p = svc.encode_resolve_mpcenc().unwrap();
        assert_eq!(
            std::fs::canonicalize(&p).unwrap(),
            std::fs::canonicalize(&mpcenc).unwrap()
        );
    }

    #[test]
    fn mpcenc_dev_missing_is_actionable() {
        let tmp = TempDir::new().unwrap();
        let loc = AuthorService::resolve_development(Some("/p/cli"), tmp.path(), no_path).unwrap();
        let svc = AuthorService::new(Ok(loc));
        // Point PATH at an empty directory so `which mpcenc` cannot succeed.
        std::env::set_var("PATH", tmp.path());
        let err = svc.encode_resolve_mpcenc().unwrap_err();
        std::env::remove_var("PATH");
        let msg = err.to_string();
        assert!(msg.contains("MUSICPACK_MPCENC"), "{msg}");
    }

    #[test]
    fn cleanup_staging_refuses_foreign_paths() {
        let tmp = TempDir::new().unwrap();
        let svc = AuthorService::new(Err(AuthorError::CliNotFound("unused".into())));
        let err = svc
            .cleanup_staging(tmp.path().to_str().unwrap())
            .unwrap_err();
        assert!(err
            .to_string()
            .contains("not a MusicPack Author staging directory"));
    }

    #[test]
    fn encode_staging_dir_is_precreated_and_empty() {
        let svc = AuthorService::new(Err(AuthorError::CliNotFound("unused".into())));
        let dir = svc.encode_staging_dir().unwrap();
        assert!(dir.is_dir());
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        svc.cleanup_staging(dir.to_str().unwrap()).unwrap();
    }

    #[test]
    fn cleanup_staging_removes_own_staging() {
        let svc = AuthorService::new(Err(AuthorError::CliNotFound("unused".into())));
        // synthesize the exact staging name the service would create
        let name = format!("musicpack-author-encode-{}-42", std::process::id());
        let dir = std::env::temp_dir().join(&name);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("x.mpc"), "data").unwrap();
        svc.cleanup_staging(dir.to_str().unwrap()).unwrap();
        assert!(!dir.exists());
    }

    #[cfg(unix)]
    #[test]
    fn create_package_passes_replace_and_sync_tags() {
        // A fake backend that answers the handshake and records its arguments
        // (all run_json needs afterwards is an empty JSON object).
        let tmp = TempDir::new().unwrap();
        let argv_file = tmp.path().join("argv.txt");
        std::env::set_var("MUSICPACK_TEST_ARGV", &argv_file);
        let bin = make_cli(
            tmp.path(),
            "MacOS",
            "#!/bin/sh\n\
             if [ \"$1\" = \"author-api-version\" ]; then\n\
             printf '{\"musicpackVersion\":\"0.1.0\",\"authorApi\":7}\\n'\n\
             exit 0\n\
             fi\n\
             printf '%s\\n' \"$@\" > \"$MUSICPACK_TEST_ARGV\"\nprintf '{}'\n",
        );
        let mut svc = AuthorService::new(Ok(BackendLocation::Bundled(bin)));

        svc.create_package("{\"schema\":\"musicpack-draft\"}", "/pkg/out.mpack", true, true)
            .unwrap();
        let argv = fs::read_to_string(&argv_file).unwrap();
        assert!(argv.contains("build-draft"), "command dispatched: {argv}");
        assert!(argv.contains("--replace"), "in-place save requested: {argv}");
        assert!(argv.contains("--sync-tags"), "tag projection requested: {argv}");

        svc.create_package("{}", "/pkg/plain.mpack", false, false)
            .unwrap();
        let argv = fs::read_to_string(&argv_file).unwrap();
        assert!(
            !argv.contains("--replace") && !argv.contains("--sync-tags"),
            "flags absent by default: {argv}"
        );
        std::env::remove_var("MUSICPACK_TEST_ARGV");
    }

    #[cfg(unix)]
    #[test]
    fn encode_spawn_passes_tools_and_staging_through() {
        // A fake backend that echoes its encode-draft arguments.
        let tmp = TempDir::new().unwrap();
        let bin = make_cli(tmp.path(), "MacOS", "#!/bin/sh\necho \"$@\"\n");
        let svc = AuthorService::new(Ok(BackendLocation::Bundled(bin)));
        let staging = tmp.path().join("stage");
        let mpcenc = tmp.path().join("mpcenc");
        let (mut child, draft_tmp) = svc
            .encode_spawn("{\"schema\":\"musicpack-draft\"}", &staging, "6.0", &mpcenc)
            .unwrap();
        use std::io::Read;
        let mut out = String::new();
        child
            .stdout
            .as_mut()
            .unwrap()
            .read_to_string(&mut out)
            .unwrap();
        let _ = child.wait();
        let _ = std::fs::remove_file(&draft_tmp);
        assert!(out.contains("encode-draft"), "command dispatched: {out}");
        assert!(
            out.contains(&format!("--mpcenc {}", mpcenc.display())),
            "{out}"
        );
        assert!(
            !out.contains("ffmpeg"),
            "no external decoder is passed: {out}"
        );
        assert!(out.contains(&format!("-o {}", staging.display())), "{out}");
        assert!(out.contains("--quality 6.0"), "{out}");
    }
}
