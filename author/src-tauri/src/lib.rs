// MusicPack Author — Tauri backend.
//
// The frontend talks to these commands; each one delegates to the
// `musicpack` CLI in JSON mode through the AuthorService. No .mpack logic
// lives here.
//
// Backend resolution is decided once at startup: a packaged release build
// uses the sidecar bundled next to the app executable, a development build
// uses MUSICPACK_CLI / the CMake build tree / PATH.

mod author_service;

use author_service::AuthorService;
use base64::Engine as _;
use serde::Serialize;
use serde_json::json;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use tauri::{Emitter, Manager, State};

struct AppState {
    service: Mutex<AuthorService>,
    running: Mutex<Option<Child>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ReadImage {
    mime: String,
    data_base64: String,
}

const MAX_IMAGE_BYTES: usize = 20 * 1024 * 1024;

fn mime_for_path(path: &str) -> &'static str {
    let lower = path.to_ascii_lowercase();
    if lower.ends_with(".png") {
        "image/png"
    } else if lower.ends_with(".gif") {
        "image/gif"
    } else if lower.ends_with(".webp") {
        "image/webp"
    } else if lower.ends_with(".bmp") {
        "image/bmp"
    } else {
        "image/jpeg"
    }
}

fn cli_err(e: author_service::AuthorError) -> String {
    e.to_string()
}

/// Repository root for development build-tree probing: `src-tauri -> author -> root`.
fn dev_repo_base() -> PathBuf {
    let mut base = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    base.pop(); // src-tauri -> author
    base.pop(); // author -> repo root
    base
}

/// Development-only PATH probe: is an installed `musicpack` available?
fn musicpack_on_path() -> bool {
    match Command::new("musicpack")
        .arg("--help")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(mut child) => {
            let _ = child.wait();
            true
        }
        Err(_) => false,
    }
}

/// Packaged sidecar path. Tauri's external binaries land next to the app
/// executable (`Contents/MacOS/`), which is exactly how tauri-plugin-shell
/// resolves sidecars (`current_exe().parent()`). Using the same runtime
/// resolution keeps us canonical without granting the webview a shell plugin.
fn bundled_sidecar() -> PathBuf {
    let exe = std::env::current_exe().unwrap_or_default();
    match exe.parent() {
        Some(dir) => dir.join("musicpack"),
        None => PathBuf::from("musicpack"),
    }
}

#[tauri::command]
fn backend_info(state: State<AppState>) -> Result<author_service::BackendInfo, String> {
    state
        .service
        .lock()
        .unwrap()
        .backend_info()
        .map_err(cli_err)
}

#[tauri::command]
fn inspect_album(state: State<AppState>, path: String) -> Result<serde_json::Value, String> {
    state
        .service
        .lock()
        .unwrap()
        .inspect_album(&path)
        .map_err(cli_err)
}

#[tauri::command]
fn validate_draft(
    state: State<AppState>,
    draft_json: String,
) -> Result<serde_json::Value, String> {
    state
        .service
        .lock()
        .unwrap()
        .validate_draft(&draft_json)
        .map_err(cli_err)
}

#[tauri::command]
fn identify_draft(
    state: State<AppState>,
    draft_json: String,
    mbid: Option<String>,
    barcode: Option<String>,
    mb_json: Option<String>,
) -> Result<serde_json::Value, String> {
    state
        .service
        .lock()
        .unwrap()
        .identify_draft(
            &draft_json,
            mbid.as_deref(),
            barcode.as_deref(),
            mb_json.as_deref(),
        )
        .map_err(cli_err)
}

#[tauri::command]
fn create_package(
    state: State<AppState>,
    draft_json: String,
    output_dir: String,
) -> Result<serde_json::Value, String> {
    state
        .service
        .lock()
        .unwrap()
        .create_package(&draft_json, &output_dir)
        .map_err(cli_err)
}

#[tauri::command]
fn verify_package(state: State<AppState>, path: String) -> Result<serde_json::Value, String> {
    state
        .service
        .lock()
        .unwrap()
        .verify_package(&path)
        .map_err(cli_err)
}

#[tauri::command]
fn read_image(path: String) -> Result<ReadImage, String> {
    let bytes = std::fs::read(&path).map_err(|e| format!("cannot read image: {e}"))?;
    if bytes.len() > MAX_IMAGE_BYTES {
        return Err("image is too large".to_string());
    }
    Ok(ReadImage {
        mime: mime_for_path(&path).to_string(),
        data_base64: base64::engine::general_purpose::STANDARD.encode(bytes),
    })
}

/// Builds a sonic analyzer job from the authoring draft: absolute audio
/// paths (sourceRoot + audioPath) plus the app-local model/cache/output
/// locations under the app data directory.
fn build_sonic_job(app: &tauri::AppHandle, draft_json: &str) -> Result<String, String> {
    let draft: serde_json::Value = serde_json::from_str(draft_json)
        .map_err(|e| format!("invalid draft: {e}"))?;
    let source_root = draft
        .get("sourceRoot")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let data_dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data directory: {e}"))?;
    let sonic_dir = data_dir.join("sonic");
    std::fs::create_dir_all(&sonic_dir).map_err(|e| format!("cannot create sonic dir: {e}"))?;
    let model_dir = sonic_dir.join("models");
    let cache_dir = sonic_dir.join("cache");
    let out_path = sonic_dir.join("sonic.json");

    let mut tracks = Vec::new();
    if let Some(media) = draft.get("media").and_then(|m| m.as_array()) {
        for disc in media {
            let disc_no = disc.get("disc").and_then(|d| d.as_i64()).unwrap_or(0);
            if let Some(trs) = disc.get("tracks").and_then(|t| t.as_array()) {
                for tr in trs {
                    let track_no = tr.get("track").and_then(|t| t.as_i64()).unwrap_or(0);
                    let ap = tr.get("audioPath").and_then(|p| p.as_str()).unwrap_or("");
                    let abs = if ap.is_empty() {
                        String::new()
                    } else if Path::new(ap).is_absolute() {
                        ap.to_string()
                    } else {
                        PathBuf::from(&source_root)
                            .join(ap)
                            .to_string_lossy()
                            .into_owned()
                    };
                    tracks.push(json!({ "disc": disc_no, "track": track_no, "path": abs }));
                }
            }
        }
    }
    if tracks.is_empty() {
        return Err("the draft has no tracks to analyse".to_string());
    }
    let job = json!({
        "profile": "musicpack-sonic-openl3-v1",
        "modelDir": model_dir.to_string_lossy(),
        "cacheDir": cache_dir.to_string_lossy(),
        "outPath": out_path.to_string_lossy(),
        "tracks": tracks,
    });
    Ok(job.to_string())
}

/// Runs the sonic analyzer for the current draft. Progress events stream as
/// `sonic-progress` Tauri events; the command resolves when the run finishes
/// (or is cancelled). The resulting sonic document is written to the app data
/// directory; `Draft.sonicAnalysis.path` points at it for `create_package`.
#[tauri::command]
async fn sonic_analyze(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    draft_json: String,
) -> Result<serde_json::Value, String> {
    let job = build_sonic_job(&app, &draft_json)?;
    let (mut child, job_tmp) = {
        let svc = state.service.lock().unwrap();
        svc.sonic_spawn(&job).map_err(cli_err)?
    };
    let stdout = child.stdout.take().ok_or("sonic analyzer produced no stdout")?;
    *state.running.lock().unwrap() = Some(child);

    let app2 = app.clone();
    let reader = std::thread::spawn(move || {
        use std::io::BufRead;
        let mut final_event: Option<serde_json::Value> = None;
        for line in std::io::BufReader::new(stdout).lines() {
            let Ok(line) = line else { continue };
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&line) {
                let _ = app2.emit("sonic-progress", &v);
                let ev = v.get("event").and_then(|e| e.as_str()).unwrap_or("");
                if matches!(ev, "done" | "error" | "cancelled") {
                    final_event = Some(v);
                }
            }
        }
        final_event
    });
    let final_event = reader.join().unwrap_or(None);
    let _ = std::fs::remove_file(&job_tmp);
    *state.running.lock().unwrap() = None;

    let out_path = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data directory: {e}"))?
        .join("sonic/sonic.json");
    let Some(event) = final_event else {
        return Err("sonic analyzer produced no result".to_string());
    };
    let ev = event
        .get("event")
        .and_then(|e| e.as_str())
        .unwrap_or("error");
    if ev == "cancelled" {
        return Ok(json!({ "ok": false, "cancelled": true, "outputPath": out_path }));
    }
    if ev == "error" {
        let message = event
            .get("message")
            .and_then(|m| m.as_str())
            .unwrap_or("sonic analysis failed");
        return Err(format!("sonic analysis failed: {message}"));
    }
    Ok(json!({
        "ok": true,
        "cancelled": false,
        "profile": "musicpack-sonic-openl3-v1",
        "outputPath": out_path,
        "sha256": event.get("sha256"),
        "tracks": event.get("tracks"),
        "contributing": event.get("contributing"),
    }))
}

/// Cancels a running sonic analysis (SIGTERM to the analyzer child).
#[tauri::command]
fn sonic_cancel(state: State<AppState>) -> Result<(), String> {
    let mut running = state.running.lock().unwrap();
    if let Some(child) = running.as_mut() {
        let _ = child.kill();
        let _ = child.wait();
    }
    *running = None;
    Ok(())
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            // Packaged release builds use only the bundled sidecar; nothing
            // is ever picked from PATH. Development uses the dev chain.
            let location = if !cfg!(debug_assertions) {
                AuthorService::resolve_bundled(&bundled_sidecar())
            } else {
                let env_cli = std::env::var("MUSICPACK_CLI").ok();
                AuthorService::resolve_development(
                    env_cli.as_deref(),
                    &dev_repo_base(),
                    musicpack_on_path,
                )
            };
            app.manage(AppState {
                service: Mutex::new(AuthorService::new(location)),
                running: Mutex::new(None),
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            backend_info,
            inspect_album,
            validate_draft,
            identify_draft,
            create_package,
            verify_package,
            read_image,
            sonic_analyze,
            sonic_cancel
        ])
        .run(tauri::generate_context!())
        .expect("error while running MusicPack Author");
}
