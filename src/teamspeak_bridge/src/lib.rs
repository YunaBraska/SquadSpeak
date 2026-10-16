use base64::{engine::general_purpose::STANDARD, Engine as _};
use futures::StreamExt;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use tokio::runtime::Builder;
use tokio::sync::mpsc;
use tokio::time::{Duration, Instant};
use tsclientlib::{prelude::*, Connection, DisconnectOptions, Identity, OutCommandExt, StreamItem};
use tsproto_packets::packets::{AudioData, CodecType, OutAudio};

const MAX_JSON: usize = 64 * 1024;
const MAX_AUDIO: usize = 4096;
const MAX_QUEUE_BYTES: usize = 512 * 1024;
const MAX_PENDING_COMMANDS: usize = 256;

#[derive(Clone, Default)]
struct Queue(Arc<Mutex<VecDeque<Vec<u8>>>>);

impl Queue {
    fn push(&self, mut value: Vec<u8>) {
        if value.len() > MAX_QUEUE_BYTES {
            value = [
                vec![1],
                br#"{"type":"closed","message":"TeamSpeak state exceeded its limit."}"#.to_vec(),
            ]
            .concat();
        }
        let mut q = self.0.lock().unwrap();
        let mut bytes: usize = q.iter().map(Vec::len).sum();
        while bytes + value.len() > MAX_QUEUE_BYTES {
            let Some(index) = q.iter().position(|entry| entry.first() == Some(&2)) else {
                if value.first() == Some(&1) {
                    q.clear();
                    let mut closed = vec![1];
                    closed.extend_from_slice(br#"{"type":"closed","message":"TeamSpeak event queue exceeded its limit."}"#);
                    q.push_back(closed);
                }
                return;
            };
            let Some(old) = q.remove(index) else {
                return;
            };
            bytes -= old.len();
        }
        q.push_back(std::mem::take(&mut value));
    }
    fn read(&self, out: &mut [u8]) -> i32 {
        let mut q = self.0.lock().unwrap();
        let Some(value) = q.front() else {
            return 0;
        };
        if out.len() < value.len() {
            return -(value.len() as i32);
        }
        let size = value.len();
        out[..size].copy_from_slice(value);
        q.pop_front();
        size as i32
    }
    #[cfg(test)]
    fn pop(&self) -> Option<Vec<u8>> {
        self.0.lock().unwrap().pop_front()
    }
}

enum Command {
    Json(Value),
    Audio(Vec<u8>),
    Close,
}

struct PendingCommand {
    request: String,
    deadline: Instant,
}

struct Handle {
    tx: mpsc::Sender<Command>,
    queue: Queue,
    join: Mutex<Option<JoinHandle<()>>>,
}

#[derive(Deserialize)]
struct Options {
    address: String,
    name: Option<String>,
    identity: Option<Value>,
    password: Option<String>,
    channel: Option<String>,
    #[serde(rename = "channelPassword")]
    channel_password: Option<String>,
    #[serde(rename = "inputMuted")]
    input_muted: Option<bool>,
    #[serde(rename = "outputMuted")]
    output_muted: Option<bool>,
    #[serde(rename = "serverKey")]
    server_key: Option<String>,
}

fn emit(queue: &Queue, value: Value) {
    if let Ok(mut bytes) = serde_json::to_vec(&value) {
        bytes.insert(0, 1);
        queue.push(bytes);
    }
}

fn error_event(error: &tsclientlib::Error) -> Value {
    let password_required = match error {
        tsclientlib::Error::ConnectTs(code) => matches!(
            code,
            tsclientlib::TsError::ClientInvalidPassword
                | tsclientlib::TsError::ChannelInvalidPassword
                | tsclientlib::TsError::ServerInvalidPassword
        ),
        tsclientlib::Error::CommandError(command) => matches!(
            command.error,
            tsclientlib::TsError::ClientInvalidPassword
                | tsclientlib::TsError::ChannelInvalidPassword
                | tsclientlib::TsError::ServerInvalidPassword
        ),
        _ => false,
    };
    json!({"type":"error", "message":error.to_string(), "passwordRequired":password_required})
}

fn result(queue: &Queue, request: &str, ok: bool, error: Option<String>) {
    result_with_password(queue, request, ok, error, false);
}

fn result_with_password(
    queue: &Queue,
    request: &str,
    ok: bool,
    error: Option<String>,
    password_required: bool,
) {
    let mut value =
        json!({"type":"result", "request":request, "ok":ok, "passwordRequired":password_required});
    if let Some(error) = error {
        value["error"] = Value::String(error);
    }
    emit(queue, value);
}

fn codec_number(codec: tsclientlib::Codec) -> u8 {
    match codec {
        tsclientlib::Codec::SpeexNarrowband => 0,
        tsclientlib::Codec::SpeexWideband => 1,
        tsclientlib::Codec::SpeexUltrawideband => 2,
        tsclientlib::Codec::CeltMono => 3,
        tsclientlib::Codec::OpusVoice => 4,
        tsclientlib::Codec::OpusMusic => 5,
    }
}

fn codec_type_number(codec: CodecType) -> Option<u8> {
    match codec {
        CodecType::OpusVoice => Some(4),
        CodecType::OpusMusic => Some(5),
        _ => None,
    }
}

fn run(options: Options, mut rx: mpsc::Receiver<Command>, queue: Queue) {
    let rt = match Builder::new_current_thread().enable_all().build() {
        Ok(r) => r,
        Err(e) => {
            emit(
                &queue,
                json!({"type":"closed","message":e.to_string(),"passwordRequired":false}),
            );
            return;
        }
    };
    rt.block_on(async move {
        let mut cfg = Connection::build(options.address)
            .name(
                options
                    .name
                    .unwrap_or_else(|| "SquadSpeak TeamSpeak".into()),
            )
            .input_muted(options.input_muted.unwrap_or(true))
            .output_muted(options.output_muted.unwrap_or(true));
        if let Some(p) = options.password {
            cfg = cfg.password(p);
        }
        if let Some(c) = options.channel {
            cfg = cfg.channel(c);
        }
        if let Some(p) = options.channel_password {
            cfg = cfg.channel_password(p);
        }
        if let Some(key) = options.server_key.filter(|key| !key.is_empty()) {
            let decoded = match STANDARD.decode(key.as_bytes()) {
                Ok(value) => value,
                Err(_) => { emit(&queue, json!({"type":"closed","message":"Invalid TeamSpeak server key.","passwordRequired":false})); return; }
            };
            cfg = cfg.server(tsclientlib::UidBuf(decoded));
        }
        if let Some(raw) = options.identity {
            if let Ok(id) = serde_json::from_value::<Identity>(raw) {
                cfg = cfg.identity(id);
            } else {
                emit(
                    &queue,
                    json!({"type":"closed","message":"Invalid TeamSpeak identity."}),
                );
                return;
            }
        }
        let mut con = match cfg.connect() {
            Ok(c) => c,
            Err(e) => {
                let mut event = error_event(&e);
                event["type"] = Value::String("closed".into());
                emit(&queue, event);
                return;
            }
        };
        let mut pending = HashMap::<u16, PendingCommand>::new();
        let mut sequence = 0u16;
        let mut subscribed = false;
        let mut deadlines = tokio::time::interval(Duration::from_millis(250));
        enum Wake {
            Command(Option<Command>),
            Event(Option<Result<StreamItem, tsclientlib::Error>>),
            Deadline,
        }
        loop {
            let wake = tokio::select! {
                command = rx.recv() => Wake::Command(command),
                event = next_event(&mut con) => Wake::Event(event),
                _ = deadlines.tick() => Wake::Deadline,
            };
            match wake {
                Wake::Command(Some(command)) => {
                    if !handle_command(&mut con, command, &queue, &mut pending, &mut sequence) {
                        let _ = tokio::time::timeout(Duration::from_millis(300), async {
                            while con.events().next().await.is_some() {}
                        }).await;
                        return;
                    }
                }
                Wake::Command(None) => return,
                Wake::Event(Some(Ok(item))) => handle_event(&mut con, item, &queue, &mut pending, &mut subscribed),
                Wake::Event(Some(Err(error))) => {
                    fail_pending(&queue, &mut pending, "TeamSpeak connection ended.");
                    let mut event = error_event(&error);
                    event["type"] = Value::String("closed".into());
                    emit(&queue, event);
                    return;
                }
                Wake::Event(None) => {
                    fail_pending(&queue, &mut pending, "TeamSpeak connection ended.");
                    emit(&queue, json!({"type":"closed","message":"TeamSpeak connection ended."}));
                    return;
                }
                Wake::Deadline => expire_pending(&queue, &mut pending),
            }
        }
    });
}

async fn next_event(con: &mut Connection) -> Option<Result<StreamItem, tsclientlib::Error>> {
    con.events().next().await
}

fn handle_command(
    con: &mut Connection,
    command: Command,
    queue: &Queue,
    pending: &mut HashMap<u16, PendingCommand>,
    sequence: &mut u16,
) -> bool {
    match command {
        Command::Close => {
            let _ = con.disconnect(DisconnectOptions::new());
            false
        }
        Command::Audio(data) => {
            if data.is_empty() || data.len() > MAX_AUDIO {
                emit(
                    queue,
                    json!({"type":"error","message":"Invalid raw Opus audio packet."}),
                );
                return true;
            }
            let state = match con.get_state() {
                Ok(state) => state,
                Err(e) => {
                    emit(queue, error_event(&e));
                    return true;
                }
            };
            let codec = match state
                .clients
                .get(&state.own_client)
                .and_then(|client| state.channels.get(&client.channel))
                .map(|channel| channel.codec)
            {
                Some(tsclientlib::Codec::OpusVoice) => CodecType::OpusVoice,
                Some(tsclientlib::Codec::OpusMusic) => CodecType::OpusMusic,
                Some(_) => {
                    emit(
                        queue,
                        json!({"type":"error","message":"Current TeamSpeak channel does not support Opus."}),
                    );
                    return true;
                }
                None => {
                    emit(
                        queue,
                        json!({"type":"error","message":"Current TeamSpeak channel is unavailable."}),
                    );
                    return true;
                }
            };
            let id = *sequence;
            *sequence = sequence.wrapping_add(1);
            let packet = OutAudio::new(&AudioData::C2S {
                id,
                codec,
                data: &data,
            });
            if let Err(e) = con.send_audio(packet) {
                emit(queue, error_event(&e));
            }
            true
        }
        Command::Json(value) => {
            let action = value.get("action").and_then(Value::as_str).unwrap_or("");
            let request = value
                .get("request")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_owned();
            if action != "disconnect" && pending.len() >= MAX_PENDING_COMMANDS {
                result(
                    queue,
                    &request,
                    false,
                    Some("Too many pending TeamSpeak commands.".into()),
                );
                return true;
            }
            let command = match action {
                "chat" => {
                    let target = value
                        .get("target")
                        .and_then(Value::as_str)
                        .unwrap_or("channel");
                    let text = match value.get("text").and_then(Value::as_str) {
                        Some(t) if !t.is_empty() && t.len() <= 4096 => t,
                        _ => {
                            result(queue, &request, false, Some("Invalid message text.".into()));
                            return true;
                        }
                    };
                    let target = match target {
                        "server" => tsclientlib::MessageTarget::Server,
                        "channel" => tsclientlib::MessageTarget::Channel,
                        "private" => match value.get("client").and_then(Value::as_u64) {
                            Some(id) if id <= u16::MAX as u64 => {
                                tsclientlib::MessageTarget::Client(tsclientlib::ClientId(id as u16))
                            }
                            _ => {
                                result(queue, &request, false, Some("Missing client.".into()));
                                return true;
                            }
                        },
                        _ => {
                            result(queue, &request, false, Some("Invalid chat target.".into()));
                            return true;
                        }
                    };
                    {
                        let state = match con.get_state() {
                            Ok(s) => s,
                            Err(e) => {
                                result(queue, &request, false, Some(e.to_string()));
                                return true;
                            }
                        };
                        state.send_message(target, text).send_with_result(con)
                    }
                }
                "join" => {
                    let channel = match value
                        .get("channel")
                        .and_then(Value::as_str)
                        .and_then(|s| s.parse::<u64>().ok())
                        .filter(|n| *n <= u64::from(u32::MAX))
                    {
                        Some(id) => tsclientlib::ChannelId(id),
                        None => {
                            result(queue, &request, false, Some("Invalid channel.".into()));
                            return true;
                        }
                    };
                    let state = match con.get_state() {
                        Ok(s) => s,
                        Err(e) => {
                            result(queue, &request, false, Some(e.to_string()));
                            return true;
                        }
                    };
                    let mut move_part = match state.clients.get(&state.own_client) {
                        Some(client) => client.client_move(channel),
                        None => {
                            result(
                                queue,
                                &request,
                                false,
                                Some("Local TeamSpeak client is unavailable.".into()),
                            );
                            return true;
                        }
                    };
                    if let Some(password) = value.get("password").and_then(Value::as_str) {
                        move_part = move_part.set_password(password);
                    }
                    move_part.to_packet().send_with_result(con)
                }
                "presence" => {
                    let state = match con.get_state() {
                        Ok(s) => s,
                        Err(e) => {
                            result(queue, &request, false, Some(e.to_string()));
                            return true;
                        }
                    };
                    let Some(current) = state.clients.get(&state.own_client) else {
                        result(
                            queue,
                            &request,
                            false,
                            Some("Local TeamSpeak client is unavailable.".into()),
                        );
                        return true;
                    };
                    let mut update = state.client_update();
                    let mut changed = false;
                    if let Some(name) = value.get("name").and_then(Value::as_str) {
                        if name.len() > 128 {
                            result(queue, &request, false, Some("Invalid name.".into()));
                            return true;
                        }
                        if name != current.name {
                            update = update.set_name(name);
                            changed = true;
                        }
                    }
                    if let Some(v) = value.get("inputMuted").and_then(Value::as_bool) {
                        if v != current.input_muted {
                            update = update.set_input_muted(v);
                            changed = true;
                        }
                    }
                    if let Some(v) = value.get("outputMuted").and_then(Value::as_bool) {
                        if v != current.output_muted {
                            update = update.set_output_muted(v);
                            changed = true;
                        }
                    }
                    if !changed {
                        result(queue, &request, true, None);
                        return true;
                    }
                    update.to_packet().send_with_result(con)
                }
                "disconnect" => {
                    let sent = con.disconnect(DisconnectOptions::new());
                    match sent {
                        Ok(()) => {
                            result(queue, &request, true, None);
                            emit(queue, json!({"type":"closed","message":"Disconnected."}));
                            return false;
                        }
                        Err(e) => {
                            result(queue, &request, false, Some(e.to_string()));
                            return false;
                        }
                    }
                }
                _ => {
                    result(queue, &request, false, Some("Unsupported command.".into()));
                    return true;
                }
            };
            match command {
                Ok(handle) => {
                    pending.insert(
                        handle.0,
                        PendingCommand {
                            request,
                            deadline: Instant::now() + Duration::from_secs(15),
                        },
                    );
                }
                Err(e) => result(queue, &request, false, Some(e.to_string())),
            }
            true
        }
    }
}

fn expire_pending(queue: &Queue, pending: &mut HashMap<u16, PendingCommand>) {
    let now = Instant::now();
    let expired = pending
        .iter()
        .filter_map(|(id, command)| (command.deadline <= now).then_some(*id))
        .collect::<Vec<_>>();
    for id in expired {
        if let Some(command) = pending.remove(&id) {
            result(
                queue,
                &command.request,
                false,
                Some("TeamSpeak command timed out.".into()),
            );
        }
    }
}

fn fail_pending(queue: &Queue, pending: &mut HashMap<u16, PendingCommand>, message: &str) {
    for (_, command) in pending.drain() {
        result(queue, &command.request, false, Some(message.into()));
    }
}

fn handle_event(
    con: &mut Connection,
    item: StreamItem,
    queue: &Queue,
    pending: &mut HashMap<u16, PendingCommand>,
    subscribed: &mut bool,
) {
    match item {
        StreamItem::IdentityLevelIncreased => {
            let value = con
                .get_options()
                .get_identity()
                .and_then(|identity| serde_json::to_value(identity).ok())
                .unwrap_or(Value::Null);
            emit(queue, json!({"type":"identity","value":value}));
        }
        StreamItem::IdentityLevelIncreasing(level) => {
            emit(queue, json!({"type":"identity","value":level}))
        }
        StreamItem::DisconnectedTemporarily(_) => {
            *subscribed = false;
            fail_pending(
                queue,
                pending,
                "TeamSpeak connection temporarily disconnected.",
            );
            emit(queue, json!({"type":"reconnecting"}));
        }
        StreamItem::MessageResult(handle, response) => {
            if let Some(command) = pending.remove(&handle.0) {
                match response {
                    Ok(()) => result(queue, &command.request, true, None),
                    Err(e) => result_with_password(
                        queue,
                        &command.request,
                        false,
                        Some(e.to_string()),
                        matches!(
                            e.error,
                            tsclientlib::TsError::ClientInvalidPassword
                                | tsclientlib::TsError::ChannelInvalidPassword
                                | tsclientlib::TsError::ServerInvalidPassword
                        ),
                    ),
                }
            }
        }
        StreamItem::Audio(packet) => {
            let data = packet.data().data();
            if let AudioData::S2C {
                id,
                from,
                codec,
                data,
            }
            | AudioData::S2CWhisper {
                id,
                from,
                codec,
                data,
            } = data
            {
                if let Some(code) = codec_type_number(*codec) {
                    let mut out = vec![
                        2,
                        (from >> 8) as u8,
                        *from as u8,
                        (id >> 8) as u8,
                        *id as u8,
                        code,
                    ];
                    out.extend_from_slice(data);
                    queue.push(out);
                }
            }
        }
        StreamItem::MessageEvent(message) => emit_messages(con, message, queue),
        StreamItem::BookEvents(events) => {
            let mut changed = false;
            for event in events {
                if let tsclientlib::events::Event::Message {
                    target,
                    invoker,
                    message,
                } = event
                {
                    let mode = match target {
                        tsclientlib::MessageTarget::Channel => "channel",
                        tsclientlib::MessageTarget::Client(_) => "private",
                        tsclientlib::MessageTarget::Server => "server",
                        tsclientlib::MessageTarget::Poke(_) => continue,
                    };
                    let channel = con
                        .get_state()
                        .ok()
                        .and_then(|state| state.clients.get(&invoker.id))
                        .map(|client| client.channel.0.to_string())
                        .unwrap_or_default();
                    emit(
                        queue,
                        json!({"type":"message", "mode":mode, "fromId":invoker.id.0.to_string(),
                        "fromUid":uid_value(invoker.uid.as_ref()), "fromName":invoker.name, "text":message, "channel":channel}),
                    );
                } else {
                    changed = true;
                }
            }
            if !*subscribed {
                if let Ok(state) = con.get_state() {
                    let _ = state.server.set_subscribed(true).send(con);
                    *subscribed = true;
                }
            }
            if changed {
                emit_state(con, queue);
            }
        }
        StreamItem::AudioChange(_) => emit_state(con, queue),
        _ => {}
    }
}

fn uid_value(uid: Option<&tsclientlib::UidBuf>) -> Value {
    uid.map(|v| Value::String(STANDARD.encode(v.0.as_slice())))
        .unwrap_or(Value::String(String::new()))
}

fn emit_messages(con: &Connection, message: tsclientlib::InMessage, queue: &Queue) {
    if let tsclientlib::InMessage::TextMessage(text) = message {
        let Ok(state) = con.get_state() else {
            return;
        };
        for part in text.iter() {
            let mode = match part.target {
                tsclientlib::TextMessageTargetMode::Server => "server",
                tsclientlib::TextMessageTargetMode::Channel => "channel",
                tsclientlib::TextMessageTargetMode::Client => "private",
                _ => continue,
            };
            let channel = state
                .clients
                .get(&part.invoker_id)
                .map(|c| c.channel.0.to_string())
                .unwrap_or_default();
            emit(
                queue,
                json!({"type":"message", "mode":mode, "fromId":part.invoker_id.0.to_string(), "fromUid":uid_value(part.invoker_uid.as_ref()), "fromName":part.invoker_name, "text":part.message, "channel":channel}),
            );
        }
    }
}

fn emit_state(con: &Connection, queue: &Queue) {
    let Ok(state) = con.get_state() else {
        return;
    };
    let channels = state.channels.values().map(|c| json!({
        "id": c.id.0.to_string(), "parent": c.parent.0.to_string(), "order": c.order.0.to_string(),
        "name": c.name, "password": c.has_password.unwrap_or(false), "codec": codec_number(c.codec),
    })).collect::<Vec<_>>();
    let members = state.clients.values().map(|c| json!({
        "id": c.id.0.to_string(), "uid": uid_value(c.uid.as_ref()), "name": c.name, "channel": c.channel.0.to_string(),
        "inputMuted": c.input_muted, "outputMuted": c.output_muted,
    })).collect::<Vec<_>>();
    emit(
        queue,
        json!({"type":"state", "server":{"name":state.server.name,"uid":state.server.public_key.get_uid()},
        "self":state.own_client.0.to_string(), "channel":state.clients.get(&state.own_client).map(|c| c.channel.0.to_string()).unwrap_or_default(),
        "canSend":con.can_send_audio(),
        "talkAllowed":state.clients.get(&state.own_client).and_then(|client| state.channels.get(&client.channel)
            .map(|channel| !channel.forced_silence && (client.talk_power_granted || client.talk_power >= channel.needed_talk_power.unwrap_or(0)))).unwrap_or(false),
        "channels":channels, "members":members}),
    );
}

#[no_mangle]
pub extern "C" fn ss_ts_identity(out: *mut u8, capacity: usize) -> i32 {
    catch_unwind(|| {
        let bytes = serde_json::to_vec(&Identity::create()).unwrap();
        if out.is_null() {
            return -(bytes.len() as i32);
        }
        if capacity < bytes.len() {
            return -(bytes.len() as i32);
        }
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        }
        bytes.len() as i32
    })
    .unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn ss_ts_open(ptr: *const u8, length: usize) -> *mut c_void {
    catch_unwind(|| {
        if ptr.is_null() || length == 0 || length > MAX_JSON {
            return std::ptr::null_mut();
        }
        let bytes = unsafe { std::slice::from_raw_parts(ptr, length) };
        let options: Options = match serde_json::from_slice(bytes) {
            Ok(v) => v,
            Err(_) => return std::ptr::null_mut(),
        };
        let (tx, rx) = mpsc::channel(256);
        let queue = Queue::default();
        let worker_queue = queue.clone();
        let join = thread::spawn(move || {
            if catch_unwind(AssertUnwindSafe(|| run(options, rx, worker_queue.clone()))).is_err() {
                emit(
                    &worker_queue,
                    json!({"type":"closed","message":"TeamSpeak connection failed."}),
                );
            }
        });
        Box::into_raw(Box::new(Handle {
            tx,
            queue,
            join: Mutex::new(Some(join)),
        })) as *mut c_void
    })
    .unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn ss_ts_command(handle: *mut c_void, ptr: *const u8, length: usize) -> i32 {
    catch_unwind(|| {
        if handle.is_null() || ptr.is_null() || length == 0 || length > MAX_JSON {
            return 0;
        }
        let h = unsafe { &*(handle as *mut Handle) };
        let value = match serde_json::from_slice(unsafe { std::slice::from_raw_parts(ptr, length) })
        {
            Ok(v) => v,
            Err(_) => return 0,
        };
        h.tx.try_send(Command::Json(value)).is_ok() as i32
    })
    .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn ss_ts_audio(handle: *mut c_void, ptr: *const u8, length: usize) -> i32 {
    catch_unwind(|| {
        if handle.is_null() || ptr.is_null() || length == 0 || length > MAX_AUDIO {
            return 0;
        }
        let h = unsafe { &*(handle as *mut Handle) };
        h.tx.try_send(Command::Audio(unsafe {
            std::slice::from_raw_parts(ptr, length).to_vec()
        }))
        .is_ok() as i32
    })
    .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn ss_ts_next(handle: *mut c_void, out: *mut u8, capacity: usize) -> i32 {
    catch_unwind(|| {
        if handle.is_null() || out.is_null() {
            return -1;
        }
        let h = unsafe { &*(handle as *mut Handle) };
        h.queue
            .read(unsafe { std::slice::from_raw_parts_mut(out, capacity) })
    })
    .unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn ss_ts_close(handle: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if handle.is_null() {
            return;
        }
        let h = unsafe { Box::from_raw(handle as *mut Handle) };
        let _ = h.tx.blocking_send(Command::Close);
        let join = { h.join.lock().unwrap().take() };
        if let Some(join) = join {
            let _ = join.join();
        }
    }));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_is_bounded_and_round_trips() {
        let mut bytes = vec![0u8; 4096];
        let size = ss_ts_identity(bytes.as_mut_ptr(), bytes.len());
        assert!(size > 0);
        assert!(serde_json::from_slice::<Identity>(&bytes[..size as usize]).is_ok());
        assert!(ss_ts_identity(std::ptr::null_mut(), 0) < 0);
    }

    #[test]
    fn output_queue_evicts_audio_before_control_events() {
        let queue = Queue::default();
        queue.push([1u8; 400 * 1024].to_vec());
        queue.push([2u8; 100 * 1024].to_vec());
        queue.push([1u8; 100 * 1024].to_vec());
        assert_eq!(queue.pop().map(|v| v[0]), Some(1));
        assert_eq!(queue.pop().map(|v| v[0]), Some(1));
        assert!(queue.pop().is_none());
    }

    #[test]
    fn undersized_reads_keep_the_event_and_overflow_is_explicit() {
        let queue = Queue::default();
        queue.push(vec![1, 2, 3]);
        assert_eq!(queue.read(&mut [0; 2]), -3);
        let mut output = [0; 3];
        assert_eq!(queue.read(&mut output), 3);
        assert_eq!(output, [1, 2, 3]);
        assert_eq!(queue.read(&mut output), 0);
        queue.push(vec![1; MAX_QUEUE_BYTES + 1]);
        let event = queue.pop().unwrap();
        let value: Value = serde_json::from_slice(&event[1..]).unwrap();
        assert_eq!(value["type"], "closed");
    }

    #[test]
    fn codecs_use_team_speak_numeric_values() {
        assert_eq!(codec_number(tsclientlib::Codec::OpusVoice), 4);
        assert_eq!(codec_number(tsclientlib::Codec::OpusMusic), 5);
        assert_eq!(codec_type_number(CodecType::OpusVoice), Some(4));
        assert_eq!(codec_type_number(CodecType::OpusMusic), Some(5));
    }

    #[test]
    fn password_errors_are_structured() {
        let event = error_event(&tsclientlib::Error::ConnectTs(
            tsclientlib::TsError::ServerInvalidPassword,
        ));
        assert_eq!(
            event.get("passwordRequired").and_then(Value::as_bool),
            Some(true)
        );
        assert_eq!(event.get("type").and_then(Value::as_str), Some("error"));
    }

    #[test]
    fn malformed_options_are_rejected_and_close_is_joinable() {
        assert!(ss_ts_open(std::ptr::null(), 0).is_null());
        let options =
            br#"{"address":"127.0.0.1:1","name":"test","inputMuted":true,"outputMuted":true}"#;
        let handle = ss_ts_open(options.as_ptr(), options.len());
        assert!(!handle.is_null());
        ss_ts_close(handle);
    }

    #[test]
    fn invalid_command_and_audio_are_rejected_without_panicking() {
        let options = br#"{"address":"127.0.0.1:1"}"#;
        let handle = ss_ts_open(options.as_ptr(), options.len());
        assert!(!handle.is_null());
        assert_eq!(ss_ts_command(handle, b"{".as_ptr(), 1), 0);
        assert_eq!(ss_ts_audio(handle, std::ptr::null(), 0), 0);
        ss_ts_close(handle);
    }
}
