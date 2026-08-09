// AuthorService: the only place that touches the `musicpack` CLI.
//
// MusicPack Author does not reimplement any .mpack semantics; it calls the
// existing `musicpack` implementation through its JSON modes. This service
// spawns the CLI, parses the structured JSON responses and maps failures to
// typed errors. The interface is deliberately thin so a direct libmusicpack
// binding can replace the subprocess later without touching the frontend.

use serde_json::Value;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Debug)]
pub enum AuthorError {
    CliNotFound(String),
    Io(String),
    CliFailure { code: Option<String>, message: String },
    Output(String),
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
        }
    }
}

impl std::error::Error for AuthorError {}

pub struct AuthorService {
    cli: PathBuf,
    counter: std::sync::atomic::AtomicU64,
}

impl AuthorService {
    pub fn new() -> Self {
        let cli = Self::resolve_cli().unwrap_or_else(|e| {
            eprintln!("MusicPack Author: {e}");
            PathBuf::from("musicpack")
        });
        AuthorService {
            cli,
            counter: std::sync::atomic::AtomicU64::new(0),
        }
    }

    fn resolve_cli() -> Result<PathBuf, AuthorError> {
        if let Ok(p) = std::env::var("MUSICPACK_CLI") {
            if !p.is_empty() {
                return Ok(PathBuf::from(p));
            }
        }
        // Dev default: the CMake build tree next to the repository.
        let mut base = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        base.pop(); // src-tauri -> author
        base.pop(); // author -> repo root
        let candidates = [
            base.join("build/musicpack/musicpack"),
            base.join("build-static/musicpack/musicpack"),
        ];
        for c in candidates {
            if c.is_file() {
                return Ok(c);
            }
        }
        // Fall back to PATH (e.g. an installed `musicpack`).
        if let Ok(mut cmd) = Command::new("musicpack").arg("--help").stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn() {
            let _ = cmd.wait();
            return Ok(PathBuf::from("musicpack"));
        }
        Err(AuthorError::CliNotFound(
            "cannot find the `musicpack` CLI; set MUSICPACK_CLI to its path".to_string(),
        ))
    }

    fn cli_path(&self) -> &Path {
        &self.cli
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

    pub fn inspect_album(&self, path: &str) -> Result<Value, AuthorError> {
        self.run_json(&["inspect", path, "--json"])
    }

    pub fn validate_draft(&self, draft_json: &str) -> Result<Value, AuthorError> {
        let tmp = self.temp_file("draft.json", draft_json)?;
        let result = self.run_json(&["validate-draft", "--draft", tmp.to_str().unwrap_or(""), "--json"]);
        let _ = std::fs::remove_file(&tmp);
        result
    }

    pub fn identify_draft(
        &self,
        draft_json: &str,
        mbid: Option<&str>,
        barcode: Option<&str>,
        mb_json: Option<&str>,
    ) -> Result<Value, AuthorError> {
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

    pub fn create_package(&self, draft_json: &str, output_dir: &str) -> Result<Value, AuthorError> {
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

    pub fn verify_package(&self, path: &str) -> Result<Value, AuthorError> {
        self.run_json(&["verify", path, "--json"])
    }
}
