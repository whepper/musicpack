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

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Authoring JSON surface version the GUI expects from the backend.
/// Bump together with the CLI's `MUSICPACK_AUTHOR_API`.
const EXPECTED_AUTHOR_API: u32 = 1;

#[derive(Debug, Clone)]
pub enum AuthorError {
    CliNotFound(String),
    Io(String),
    CliFailure { code: Option<String>, message: String },
    Output(String),
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
        for rel in ["build/musicpack/musicpack", "build-static/musicpack/musicpack"] {
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
        let n = self.counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
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

    // ---- operations ---------------------------------------------------

    pub fn inspect_album(&mut self, path: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        self.run_json(&["inspect", path, "--json"])
    }

    pub fn validate_draft(&mut self, draft_json: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        let tmp = self.temp_file("draft.json", draft_json)?;
        let result = self.run_json(&["validate-draft", "--draft", tmp.to_str().unwrap_or(""), "--json"]);
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
        if let Some(id) = mbid {
            args.push("--mbid".to_string());
            args.push(id.to_string());
        } else if let Some(bc) = barcode {
            args.push("--barcode".to_string());
            args.push(bc.to_string());
        } else if let Some(json) = mb_json {
            let mb = self.temp_file("mb-release.json", json)?;
            args.push("--mb-json".to_string());
            args.push(mb.to_string_lossy().into_owned());
            let result = self.run_json(&args.iter().map(|s| s.as_str()).collect::<Vec<_>>());
            let _ = std::fs::remove_file(&mb);
            let _ = std::fs::remove_file(&tmp);
            return result;
        }
        let refs: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
        let result = self.run_json(&refs);
        let _ = std::fs::remove_file(&tmp);
        result
    }

    pub fn create_package(&mut self, draft_json: &str, output_dir: &str) -> Result<Value, AuthorError> {
        self.ensure_handshake()?;
        let tmp = self.temp_file("draft.json", draft_json)?;
        let result = self.run_json(&[
            "build-draft",
            "--draft",
            tmp.to_str().unwrap_or(""),
            "-o",
            output_dir,
            "--json",
        ]);
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
                let sonic = cli.parent().unwrap_or_else(|| Path::new("")).join("musicpack-sonic");
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
    pub fn sonic_spawn(&self, job_json: &str) -> Result<(std::process::Child, PathBuf), AuthorError> {
        let sonic = self.sonic_resolve()?;
        let job_tmp = self.temp_file("sonic-job.json", job_json)?;
        let child = Command::new(sonic)
            .arg(&job_tmp)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::inherit())
            .spawn()
            .map_err(|e| {
                let _ = std::fs::remove_file(&job_tmp);
                AuthorError::Io(format!("cannot run `musicpack-sonic`: {e}"))
            })?;
        Ok((child, job_tmp))
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
        let err =
            AuthorService::resolve_bundled(&tmp.path().join("MacOS/musicpack")).unwrap_err();
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
        let info = AuthorService::parse_author_api(
            "{\"musicpackVersion\":\"0.1.0\",\"authorApi\":1}\n",
        )
        .unwrap();
        assert_eq!(info.author_api, 1);
        assert_eq!(info.musicpack_version, "0.1.0");
    }

    #[test]
    fn handshake_rejects_mismatched_api() {
        let err = AuthorService::parse_author_api(
            "{\"musicpackVersion\":\"0.2.0\",\"authorApi\":2}\n",
        )
        .unwrap_err();
        let msg = err.to_string();
        assert!(msg.contains("author API 2"), "{msg}");
        assert!(msg.contains("requires 1"), "{msg}");
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
            "#!/bin/sh\nprintf '{\"musicpackVersion\":\"0.1.0\",\"authorApi\":1}\\n'\n",
        );
        let mut service =
            AuthorService::new(Ok(BackendLocation::Bundled(bin)));
        let info = service.ensure_handshake().unwrap();
        assert_eq!(info.author_api, 1);
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
}
