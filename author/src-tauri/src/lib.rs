// MusicPack Author — Tauri backend.
//
// The frontend talks to these commands; each one delegates to the
// `musicpack` CLI in JSON mode through the AuthorService. No .mpack logic
// lives here.

mod author_service;

use author_service::AuthorService;
use base64::Engine as _;
use serde::Serialize;
use std::sync::Mutex;
use tauri::State;

struct AppState {
    service: Mutex<AuthorService>,
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

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState {
            service: Mutex::new(AuthorService::new()),
        })
        .invoke_handler(tauri::generate_handler![
            inspect_album,
            validate_draft,
            identify_draft,
            create_package,
            verify_package,
            read_image
        ])
        .run(tauri::generate_context!())
        .expect("error while running MusicPack Author");
}
