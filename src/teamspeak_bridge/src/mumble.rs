//! Mumble 1.4 and 1.5 compatible control-channel adapter. Cryptographic primitives and
//! packet codecs use OpenSSL, the corrected upstream Mumble cipher and mumble-protocol.
use crate::mumble_crypt::ClientCryptState;
use crate::mumble_voice::ControlWire;
use crate::{emit, Queue, MAX_JSON, MAX_QUEUE_BYTES};
use bytes::Bytes;
use futures::{SinkExt, StreamExt};
use mumble_protocol::control::{msgs, ControlPacket};
use mumble_protocol::voice::{VoicePacket, VoicePacketPayload};
use openssl::{
    hash::MessageDigest,
    pkey::PKey,
    ssl::{SslConnector, SslMethod, SslVerifyMode},
    x509::X509,
};
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap};
use tokio::{
    io::AsyncWriteExt,
    net::{lookup_host, TcpStream, UdpSocket},
    time::{timeout, Duration, Instant},
};
use tokio_openssl::SslStream;
use tokio_util::{codec::Framed, udp::UdpFramed};

const AUDIO_LIMIT: usize = 1275;
// Mumble datagrams are capped at 1024 bytes. Reserve room for the cipher,
// protobuf/legacy framing, a full sequence number and the sender session.
const SEND_PACKET_LIMIT: usize = 1024 - 32;
const MAX_CHANNELS: usize = 4096;
const MAX_USERS: usize = 8192;
const MAX_SERVER_TEXT: usize = 4096;

fn opus_samples(packet: &[u8]) -> Option<usize> {
    if packet.is_empty() || packet.len() > AUDIO_LIMIT {
        return None;
    }
    let samples = unsafe {
        audiopus_sys::opus_packet_get_nb_samples(packet.as_ptr(), packet.len() as i32, 48_000)
    };
    (samples >= 480 && samples <= 5760 && samples % 480 == 0).then_some(samples as usize)
}

fn audio_ticks(packet: &[u8]) -> Option<u64> {
    opus_samples(packet).map(|samples| (samples / 480) as u64)
}

fn audio_event(
    session: u32,
    sequence: u64,
    payload: &VoicePacketPayload,
) -> Result<Vec<u8>, &'static str> {
    let VoicePacketPayload::Opus(data, final_packet) = payload else {
        return Err("Mumble server sent unsupported voice audio.");
    };
    if data.len() > AUDIO_LIMIT
        || (!data.is_empty() && opus_samples(data).is_none())
        || (data.is_empty() && !final_packet)
    {
        return Err("Mumble voice packet is invalid.");
    }
    let mut event = Vec::with_capacity(14 + data.len());
    event.push(3);
    event.extend_from_slice(&session.to_be_bytes());
    event.extend_from_slice(&sequence.to_be_bytes());
    event.push(u8::from(*final_packet));
    event.extend_from_slice(data);
    Ok(event)
}

fn queue_audio(queue: &Queue, session: u32, sequence: u64, payload: &VoicePacketPayload) -> bool {
    match audio_event(session, sequence, payload) {
        Ok(event) => {
            queue.push(event);
            true
        }
        Err(message) => {
            emit(queue, json!({"type":"error","message":message}));
            false
        }
    }
}

struct Pending {
    deadline: Instant,
    channel: Option<String>,
    token: Option<String>,
}

fn add_access_token(tokens: &mut Vec<String>, token: &str) -> Result<bool, &'static str> {
    if token.is_empty() || token.len() > 1024 {
        return Err("Invalid Mumble access token.");
    }
    if tokens.iter().any(|known| known == token) {
        return Ok(false);
    }
    if tokens.len() >= 32 || tokens.iter().map(String::len).sum::<usize>() + token.len() > 4096 {
        return Err("Too many Mumble access tokens.");
    }
    tokens.push(token.to_owned());
    Ok(true)
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Options {
    address: String,
    name: Option<String>,
    identity_pem: String,
    password: Option<String>,
    server_key: Option<String>,
    input_muted: Option<bool>,
    output_muted: Option<bool>,
}

fn fail(queue: &Queue, message: &str, password: bool) {
    emit(
        queue,
        json!({"type":"closed","message":message,"passwordRequired":password}),
    );
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn read_state(
    channels: &BTreeMap<u32, Value>,
    users: &BTreeMap<u32, Value>,
    own: u32,
    server: &str,
    pin: &str,
    synced: bool,
    can_send: Option<bool>,
    packet_limit: usize,
) -> Value {
    let user = users.get(&own).cloned().unwrap_or(Value::Null);
    let server = channels
        .get(&0)
        .and_then(|root| root["name"].as_str())
        .filter(|name| !name.trim().is_empty())
        .unwrap_or(server);
    let talk_allowed = can_send.unwrap_or(true)
        && !user["_serverMute"].as_bool().unwrap_or(false)
        && !user["_suppress"].as_bool().unwrap_or(false);
    json!({"type":"state","directoryComplete":synced,"server":{"name":server,"uid":pin},
        "self":own.to_string(),"channel":user["channel"].as_str().unwrap_or(""),"maximumAudioPacket":packet_limit,"canSend":talk_allowed && packet_limit>0,"talkAllowed":talk_allowed,
        "channels":channels.values().cloned().collect::<Vec<_>>(),"members":users.values().cloned().collect::<Vec<_>>()})
}

struct UserUpdate {
    value: Value,
    channel: String,
    previous_channel: String,
}

fn merge_user(id: u32, message: &msgs::UserState, previous: Option<&Value>) -> UserUpdate {
    let empty = Value::Null;
    let old = previous.unwrap_or(&empty);
    let name = if message.has_name() {
        message.name().chars().take(128).collect::<String>()
    } else {
        old["name"].as_str().unwrap_or("").to_owned()
    };
    let channel = if message.has_channel_id() {
        message.channel_id().to_string()
    } else {
        old["channel"].as_str().unwrap_or("0").to_owned()
    };
    let previous_channel = old["channel"].as_str().unwrap_or("").to_owned();
    let hash = if message.has_hash() {
        message.hash().chars().take(128).collect::<String>()
    } else {
        old["uid"].as_str().unwrap_or("").to_owned()
    };
    let comment = if message.has_comment() {
        message.comment().chars().take(100).collect::<String>()
    } else {
        old["metadata"].as_str().unwrap_or("").to_owned()
    };
    let self_mute = if message.has_self_mute() {
        message.self_mute()
    } else {
        old["_selfMute"].as_bool().unwrap_or(false)
    };
    let server_mute = if message.has_mute() {
        message.mute()
    } else {
        old["_serverMute"].as_bool().unwrap_or(false)
    };
    let suppress = if message.has_suppress() {
        message.suppress()
    } else {
        old["_suppress"].as_bool().unwrap_or(false)
    };
    let self_deaf = if message.has_self_deaf() {
        message.self_deaf()
    } else {
        old["_selfDeaf"].as_bool().unwrap_or(false)
    };
    let server_deaf = if message.has_deaf() {
        message.deaf()
    } else {
        old["_serverDeaf"].as_bool().unwrap_or(false)
    };
    UserUpdate {
        value: json!({"id":id.to_string(),"uid":hash,"name":name,"channel":channel,"inputMuted":self_mute||server_mute||suppress,"outputMuted":self_deaf||server_deaf,"metadata":comment,"_selfMute":self_mute,"_serverMute":server_mute,"_suppress":suppress,"_selfDeaf":self_deaf,"_serverDeaf":server_deaf}),
        channel,
        previous_channel,
    }
}

fn emit_bounded_state(queue: &Queue, state: Value) -> bool {
    match serde_json::to_vec(&state) {
        Ok(bytes) if bytes.len() + 1 <= MAX_QUEUE_BYTES => {
            queue.push([vec![1], bytes].concat());
            true
        }
        _ => {
            fail(queue, "Mumble state snapshot exceeded its limit.", false);
            false
        }
    }
}

fn run(
    options: Options,
    mut rx: tokio::sync::mpsc::Receiver<crate::Command>,
    mut cancel: tokio::sync::watch::Receiver<bool>,
    queue: Queue,
) {
    let rt = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(rt) => rt,
        Err(e) => {
            fail(&queue, &e.to_string(), false);
            return;
        }
    };
    rt.block_on(async move {
        macro_rules! cancellable { ($future:expr) => { tokio::select! { biased; changed=cancel.changed()=>{let _=changed;return;}, result=$future=>result } }; }
        if options.address.len() > 300 || options.identity_pem.len() > MAX_JSON {
            fail(&queue, "Invalid Mumble connection options.", false); return;
        }
        let (host, port) = match options.address.rsplit_once(':') {
            Some((h,p)) if !h.is_empty() => (h.trim_matches(['[',']']), p.parse::<u16>().ok().filter(|p| *p != 0)),
            _ => ("", None),
        };
        let Some(port) = port else { fail(&queue, "Invalid Mumble server address.", false); return; };
        let resolved=cancellable!(timeout(Duration::from_secs(5),lookup_host((host,port))));
        let addresses = match resolved {
            Ok(Ok(addresses)) => addresses.take(32).collect::<Vec<_>>(),
            _ => { fail(&queue, "Mumble server lookup timed out or failed.", false); return; }
        };
        if addresses.is_empty() { fail(&queue, "Could not resolve Mumble server.", false); return; }
        let cert = match X509::from_pem(options.identity_pem.as_bytes()) { Ok(v)=>v,Err(_)=>{fail(&queue,"Invalid Mumble client certificate.",false);return;} };
        let key = match PKey::private_key_from_pem(options.identity_pem.as_bytes()) { Ok(v)=>v,Err(_)=>{fail(&queue,"Invalid Mumble client private key.",false);return;} };
        let mut builder = match SslConnector::builder(SslMethod::tls_client()) { Ok(v)=>v,Err(_)=>{fail(&queue,"Could not initialize TLS.",false);return;} };
        if builder.set_min_proto_version(Some(openssl::ssl::SslVersion::TLS1_2)).is_err() {
            fail(&queue,"Could not require TLS 1.2.",false);return;
        }
        builder.set_verify(SslVerifyMode::NONE); // Mumble deployments commonly use self-signed certs; key pin is checked below.
        if builder.set_certificate(&cert).is_err() || builder.set_private_key(&key).is_err() {
            fail(&queue,"Invalid Mumble client identity.",false);return;
        }
        let stream = match cancellable!(timeout(Duration::from_secs(8), TcpStream::connect(addresses.as_slice()))) {
            Ok(Ok(v))=>v,_=>{fail(&queue,"Mumble connection timed out.",false);return;}
        };
        let address = match stream.peer_addr() {
            Ok(address) => address,
            Err(_) => { fail(&queue, "Mumble connection has no remote address.", false); return; }
        };
        let connector = builder.build();
        let mut ssl = match openssl::ssl::Ssl::new(connector.context()) { Ok(v)=>v,Err(_)=>{fail(&queue,"Could not initialize TLS.",false);return;} };
        if ssl.set_hostname(host).is_err() { fail(&queue,"Invalid Mumble server name.",false);return; }
        let mut tls = SslStream::new(ssl, stream).unwrap();
        if !matches!(cancellable!(timeout(Duration::from_secs(8), std::pin::Pin::new(&mut tls).connect())), Ok(Ok(()))) {
            fail(&queue,"Mumble TLS connection failed.",false);return;
        }
        let remote = match tls.ssl().peer_certificate().and_then(|c| c.public_key().ok()).and_then(|k| k.public_key_to_der().ok()) {
            Some(v)=>v,None=>{fail(&queue,"Mumble server did not provide a public key.",false);return;}
        };
        let pin = match openssl::hash::hash(MessageDigest::sha256(), &remote) { Ok(v)=>hex(&v),Err(_)=>{fail(&queue,"Could not verify Mumble server key.",false);return;} };
        if options.server_key.as_ref().is_some_and(|saved| !saved.is_empty() && !saved.eq_ignore_ascii_case(&pin)) {
            fail(&queue,"Mumble server key changed.",false);return;
        }
        let mut framed = Framed::new(tls, ControlWire::new(false));
        macro_rules! send_control { ($writer:ident,$packet:expr) => { cancellable!(timeout(Duration::from_secs(2),$writer.send($packet))) }; }
        let mut version = msgs::Version::new(); version.set_version_v1(0x010500); version.set_version_v2((1u64 << 48) | (5u64 << 32)); version.set_release("SquadSpeak".into()); version.set_os("SquadSpeak".into());
        if !matches!(cancellable!(timeout(Duration::from_secs(3),framed.send(version.into()))),Ok(Ok(()))) { fail(&queue,"Mumble handshake failed.",false);return; }
        let input_muted=options.input_muted.unwrap_or(true); let output_muted=options.output_muted.unwrap_or(true);
        let mut auth = msgs::Authenticate::new(); auth.set_username(options.name.unwrap_or_else(||"SquadSpeak".into())); auth.set_opus(true);
        if let Some(p)=options.password { auth.set_password(p); }
        if !matches!(cancellable!(timeout(Duration::from_secs(3),framed.send(auth.into()))),Ok(Ok(()))) { fail(&queue,"Mumble authentication failed.",false);return; }
        let mut crypt: Option<ClientCryptState> = None;
        let mut own=0u32; let server_name=host.to_string(); let mut synced=false; let mut can_send:Option<bool>=None;
        let mut channels=BTreeMap::<u32,Value>::new(); let mut users=BTreeMap::<u32,Value>::new();
        let mut udp: Option<UdpFramed<ClientCryptState, UdpSocket>> = None;
        let mut udp_ready=false;
        let mut last_udp=Instant::now();
        let mut last_resync=Instant::now();
        let mut access_tokens=Vec::<String>::new();
        let mut pending=HashMap::<String,Pending>::new();
        let mut seq=0u64;
        let overhead=if address.is_ipv4(){44}else{64};
        let mut packet_limit=SEND_PACKET_LIMIT;
        let started=Instant::now();
        let mut ping=tokio::time::interval(Duration::from_secs(15));
        let mut udp_ping=tokio::time::interval(Duration::from_secs(1));
        let mut command_deadlines=tokio::time::interval(Duration::from_millis(250));
        let authentication_deadline=Instant::now()+Duration::from_secs(12);
        let mut audio = crate::AudioQueue::new();
        macro_rules! send_audio { ($data:expr) => {{
            let mut data: Vec<u8> = $data;
            let final_packet = data.is_empty();
            // Protobuf Mumble servers reject empty Opus payloads, including
            // terminators. A standard 20 ms Opus silence frame ends the stream
            // without changing or delaying any captured speech.
            if final_packet && framed.codec().modern { data = vec![0xf8, 0xff, 0xfe]; }
            let ticks = if data.is_empty() { 0 } else { audio_ticks(&data).unwrap_or(0) };
    let payload = VoicePacketPayload::Opus(Bytes::from(data), final_packet);
    let packet = VoicePacket::Audio {
        _dst: std::marker::PhantomData,
        target: 0,
        session_id: (),
        seq_num: seq,
        payload,
        position_info: None,
    };
    seq = seq.saturating_add(ticks as u64);
    if udp_ready {
        if let Some(u) = udp.as_mut() {
            if !matches!(
                timeout(Duration::from_millis(250), u.send((packet, address))).await,
                Ok(Ok(()))
            ) {
                emit(
                    &queue,
                    json!({"type":"error","message":"Mumble UDP voice send failed."}),
                );
            }
        }
    } else if !matches!(
        cancellable!(timeout(
            Duration::from_secs(2),
            framed.send(ControlPacket::UDPTunnel(Box::new(packet)))
        )),
        Ok(Ok(()))
    ) {
        emit(
            &queue,
            json!({"type":"error","message":"Mumble voice tunnel failed."}),
        );
    }
        }}; }
        loop {
            tokio::select! {
                biased;
                changed=cancel.changed()=>{let _=changed;let _=timeout(Duration::from_millis(250),async{let _=framed.flush().await;let _=framed.get_mut().shutdown().await;}).await;return;},
                cmd=rx.recv()=> {
                    match cmd {
                        Some(crate::Command::Close) | None => {
                            let _ = rx.close();
                            let _ = timeout(Duration::from_millis(250), async {
                                let _ = framed.flush().await;
                                let _ = framed.get_mut().shutdown().await;
                            })
                            .await;
                            return;
                        }
                        Some(crate::Command::Audio(data)) => {
                            if synced && data.len() <= packet_limit {
                                if data.is_empty() || audio_ticks(&data).is_some() { audio.push(data); }
                                else { emit(&queue, json!({"type":"error","message":"Invalid Mumble Opus audio packet."})); }
                            }
                        }
                        Some(crate::Command::Json(value)) => {
                            let request = value["request"].as_str().unwrap_or("").to_owned();
                            let action = value["action"].as_str().unwrap_or("");
                            let interrupts = action == "join"
                                || (action == "presence" && (value["inputMuted"] == true || value["outputMuted"] == true));
                            if interrupts && audio.clear() { send_audio!(Vec::new()); }

                            if pending.len() >= 256 {
                                emit(
                                    &queue,
                                    json!({"type":"result","request":request,"ok":false,"error":"Too many pending Mumble commands."}),
                                );
                                continue;
                            }
                            let send_result = match action {
                                "join" => {
                                    let Some(channel) = value["channel"]
                                        .as_str()
                                        .and_then(|s| s.parse::<u32>().ok())
                                    else {
                                        emit(
                                            &queue,
                                            json!({"type":"result","request":request,"ok":false,"error":"Invalid Mumble channel."}),
                                        );
                                        continue;
                                    };
                                    let token = value["password"].as_str().unwrap_or("");
                                    let added = if token.is_empty() {
                                        false
                                    } else {
                                        match add_access_token(&mut access_tokens, token) {
                                            Ok(v) => v,
                                            Err(e) => {
                                                emit(
                                                    &queue,
                                                    json!({"type":"result","request":request,"ok":false,"passwordRequired":true,"error":e}),
                                                );
                                                continue;
                                            }
                                        }
                                    };
                                    if !token.is_empty() {
                                        let mut a = msgs::Authenticate::new();
                                        for t in &access_tokens {
                                            a.mut_tokens().push(t.clone());
                                        }
                                        if !matches!(send_control!(framed, a.into()), Ok(Ok(()))) {
                                            if added {
                                                access_tokens.pop();
                                            }
                                            emit(
                                                &queue,
                                                json!({"type":"result","request":request,"ok":false,"error":"Mumble connection ended."}),
                                            );
                                            continue;
                                        }
                                    }
                                    let mut m = msgs::UserState::new();
                                    m.set_channel_id(channel);
                                    if !matches!(send_control!(framed, m.into()), Ok(Ok(()))) {
                                        if added {
                                            access_tokens.pop();
                                        }
                                        emit(
                                            &queue,
                                            json!({"type":"result","request":request,"ok":false,"error":"Mumble connection ended."}),
                                        );
                                        continue;
                                    }
                                    pending.insert(
                                        request,
                                        Pending {
                                            deadline: Instant::now() + Duration::from_secs(8),
                                            channel: Some(channel.to_string()),
                                            token: if added { Some(token.to_owned()) } else { None },
                                        },
                                    );
                                    continue;
                                }
                                "chat" => {
                                    let text = value["text"].as_str().unwrap_or("");
                                    if text.is_empty() || text.len() > 4096 {
                                        emit(
                                            &queue,
                                            json!({"type":"result","request":request,"ok":false,"error":"Invalid message text."}),
                                        );
                                        continue;
                                    }
                                    let mut m = msgs::TextMessage::new();
                                    m.set_message(text.into());
                                    match value["target"].as_str().unwrap_or("channel") {
                                        "channel" => {
                                            let channel = value["channel"]
                                                .as_str()
                                                .and_then(|s| s.parse().ok())
                                                .or_else(|| {
                                                    users
                                                        .get(&own)
                                                        .and_then(|u| u["channel"].as_str())
                                                        .and_then(|s| s.parse().ok())
                                                })
                                                .unwrap_or(0);
                                            m.mut_channel_id().push(channel);
                                        }
                                        "private" => {
                                            if let Some(id) =
                                                value["client"].as_u64().filter(|id| *id <= u32::MAX as u64)
                                            {
                                                m.mut_session().push(id as u32)
                                            } else {
                                                emit(
                                                    &queue,
                                                    json!({"type":"result","request":request,"ok":false,"error":"Missing or invalid client."}),
                                                );
                                                continue;
                                            }
                                        }
                                        _ => {
                                            emit(
                                                &queue,
                                                json!({"type":"result","request":request,"ok":false,"error":"Unsupported chat target."}),
                                            );
                                            continue;
                                        }
                                    }
                                    send_control!(framed, m.into())
                                }
                                "presence" => {
                                    let mut m = msgs::UserState::new();
                                    if let Some(n) = value["name"].as_str() {
                                        m.set_name(n.into());
                                    }
                                    if let Some(v) = value["inputMuted"].as_bool() {
                                        m.set_self_mute(v);
                                    }
                                    if let Some(v) = value["outputMuted"].as_bool() {
                                        m.set_self_deaf(v);
                                    }
                                    if let Some(comment) = value["metadata"].as_str().filter(|s| s.len() <= 100) {
                                        m.set_comment(comment.into());
                                    }
                                    send_control!(framed, m.into())
                                }
                                _ => {
                                    emit(
                                        &queue,
                                        json!({"type":"result","request":request,"ok":false,"error":"Unsupported command."}),
                                    );
                                    continue;
                                }
                            };
                            if matches!(send_result, Ok(Ok(()))) {
                                if action == "chat" || action == "presence" {
                                    emit(
                                        &queue,
                                        json!({"type":"result","request":request,"ok":true,"confirmedByServer":false}),
                                    );
                                } else {
                                    pending.insert(
                                        request,
                                        Pending {
                                            deadline: Instant::now() + Duration::from_secs(8),
                                            channel: None,
                                            token: None,
                                        },
                                    );
                                }
                            } else {
                                emit(
                                    &queue,
                                    json!({"type":"result","request":request,"ok":false,"error":"Mumble connection ended."}),
                                );
                            }
                        }
                    }
                },
                data=audio.next(), if !audio.packets.is_empty()=> {
                    if data.len() <= packet_limit { send_audio!(data); }
                },
                pkt=framed.next()=> {
                    {
                        let Some(Ok(packet)) = pkt else {
                            fail(&queue, "Mumble connection ended.", false);
                            return;
                        };
                        let mut changed = false;
                        match packet {
                            ControlPacket::Version(m) => {
                                // Advertise both wire formats, then use the server's version.
                                framed.codec_mut().modern = if m.has_version_v2() {
                                    m.version_v2() >= ((1u64 << 48) | (5u64 << 32))
                                } else {
                                    m.version_v1() >= 0x010500
                                };
                            }
                            ControlPacket::CryptSetup(m) => {
                                if (m.has_key() && (!m.has_client_nonce() || !m.has_server_nonce() || m.key().len() != 16))
                                    || (m.has_client_nonce() && m.client_nonce().len() != 16)
                                    || (m.has_server_nonce() && m.server_nonce().len() != 16) {
                                    fail(&queue, "Mumble encryption setup is invalid.", false);
                                    return;
                                }
                                if m.has_key() && m.has_client_nonce() && m.has_server_nonce() {
                                    if let (Ok(k), Ok(c), Ok(s)) = (
                                        m.key().try_into(),
                                        m.client_nonce().try_into(),
                                        m.server_nonce().try_into(),
                                    ) {
                                        match ClientCryptState::new_from(k, c, s, framed.codec().modern) {
                                            Ok(state) => {
                                                if let Some(u) = udp.as_mut() {
                                                    *u.codec_mut() = state;
                                                } else {
                                                    crypt = Some(state);
                                                }
                                                udp_ready = false;
                                            }
                                            Err(_) => {
                                                fail(&queue, "Mumble encryption setup failed.", false);
                                                return;
                                            }
                                        }
                                    }
                                } else if m.has_server_nonce() {
                                    if let (Some(state), Ok(nonce)) =
                                        (udp.as_mut(), m.server_nonce().try_into())
                                    {
                                        state.codec_mut().set_decrypt_nonce(&nonce);
                                    }
                                } else if let Some(state) = udp.as_ref() {
                                    let mut response = msgs::CryptSetup::new();
                                    response.set_client_nonce(state.codec().get_encrypt_nonce().to_vec());
                                    let _ = send_control!(framed, response.into());
                                }
                            }
                            ControlPacket::ServerConfig(m) => {
                                if m.has_max_bandwidth() {
                                    packet_limit = (m.max_bandwidth() as usize / 400)
                                        .saturating_sub(overhead)
                                        .min(SEND_PACKET_LIMIT);
                                    changed = true;
                                }
                            }
                            ControlPacket::ServerSync(m) => {
                                if m.has_max_bandwidth() {
                                    packet_limit = (m.max_bandwidth() as usize / 400)
                                        .saturating_sub(overhead)
                                        .min(SEND_PACKET_LIMIT);
                                }
                                own = m.session();
                                // ServerSync permissions belong to root. The server
                                // may have placed us in a different default channel.
                                can_send = Some(false);
                                if let Some(channel) = users.get(&own)
                                    .and_then(|user| user["channel"].as_str())
                                    .and_then(|channel| channel.parse::<u32>().ok()) {
                                    let mut query = msgs::PermissionQuery::new();
                                    query.set_channel_id(channel);
                                    let _ = send_control!(framed, query.into());
                                }
                                synced = true;
                                changed = true;
                                if input_muted || output_muted {
                                    let mut presence = msgs::UserState::new();
                                    presence.set_self_mute(input_muted);
                                    presence.set_self_deaf(output_muted);
                                    let _ = send_control!(framed, presence.into());
                                }
                                if crypt.is_some() {
                                    let bind = if address.is_ipv4() {
                                        "0.0.0.0:0"
                                    } else {
                                        "[::]:0"
                                    };
                                    if let Ok(sock) = UdpSocket::bind(bind).await {
                                        udp = Some(UdpFramed::new(sock, crypt.take().unwrap()));
                                    }
                                }
                            }
                            ControlPacket::ChannelState(m) => {
                                if m.has_channel_id() {
                                    let id = m.channel_id();
                                    if !channels.contains_key(&id) && channels.len() >= MAX_CHANNELS {
                                        fail(
                                            &queue,
                                            "Mumble channel directory exceeded its limit.",
                                            false,
                                        );
                                        return;
                                    }
                                    let old = channels.get(&id).cloned().unwrap_or(Value::Null);
                                    let parent = if m.has_parent() {
                                        m.parent().to_string()
                                    } else {
                                        old["parent"].as_str().unwrap_or("0").to_owned()
                                    };
                                    let name = if m.has_name() {
                                        m.name().chars().take(128).collect::<String>()
                                    } else {
                                        old["name"].as_str().unwrap_or("").to_owned()
                                    };
                                    let order = if m.has_position() {
                                        m.position().to_string()
                                    } else {
                                        old["order"].as_str().unwrap_or("0").to_owned()
                                    };
                                    channels.insert(id,json!({"id":id.to_string(),"parent":parent,"order":order,"name":name,"password":false,"codec":5}));
                                    changed = true;
                                }
                            }
                            ControlPacket::ChannelRemove(m) => {
                                channels.remove(&m.channel_id());
                                changed = true;
                            }
                            ControlPacket::UserState(m) => {
                                let id = m.session();
                                if !users.contains_key(&id) && users.len() >= MAX_USERS {
                                    fail(&queue, "Mumble user directory exceeded its limit.", false);
                                    return;
                                }
                                let update = merge_user(id, &m, users.get(&id));
                                let channel = update.channel.clone();
                                let prior_channel = update.previous_channel.clone();
                                users.insert(id, update.value);
                                if id == own {
                                    let completed = pending.iter().find_map(|(request, p)| {
                                        (p.channel.as_deref() == Some(channel.as_str())).then_some(request.clone())
                                    });
                                    if let Some(request) = completed {
                                        pending.remove(&request);
                                        emit(
                                            &queue,
                                            json!({"type":"result","request":request,"ok":true,"passwordRequired":false}),
                                        );
                                    }
                                    if prior_channel != channel {
                                        can_send = None;
                                        if let Ok(cid) = channel.parse() {
                                            let mut query = msgs::PermissionQuery::new();
                                            query.set_channel_id(cid);
                                            let _ = send_control!(framed, query.into());
                                        }
                                    }
                                }
                                changed = true;
                            }
                            ControlPacket::PermissionQuery(m) => {
                                let current = users
                                    .get(&own)
                                    .and_then(|u| u["channel"].as_str())
                                    .and_then(|s| s.parse::<u32>().ok());
                                if m.has_permissions() && m.has_channel_id() && current == Some(m.channel_id())
                                {
                                    can_send = Some(m.permissions() & 8 != 0);
                                    changed = true;
                                }
                            }
                            ControlPacket::CodecVersion(m) => {
                                if !m.opus() {
                                    fail(
                                        &queue,
                                        "Mumble server requires an unsupported voice codec.",
                                        false,
                                    );
                                    return;
                                }
                            }
                            ControlPacket::UserRemove(m) => {
                                if m.session() == own {
                                    fail(&queue, "Mumble server removed the local user.", false);
                                    return;
                                }
                                users.remove(&m.session());
                                changed = true;
                            }
                            ControlPacket::TextMessage(m) => {
                                if m.message().len() > MAX_SERVER_TEXT {
                                    emit(
                                        &queue,
                                        json!({"type":"error","message":"Mumble text message exceeded its limit."}),
                                    );
                                    continue;
                                }
                                let mode = if !m.session().is_empty() {
                                    "private"
                                } else if !m.channel_id().is_empty() {
                                    "channel"
                                } else {
                                    "server"
                                };
                                let from = m.actor();
                                let u = users.get(&from).cloned().unwrap_or(Value::Null);
                                let channel = u["channel"].as_str().unwrap_or("");
                                emit(
                                    &queue,
                                    json!({"type":"message","mode":mode,"fromId":from.to_string(),"fromUid":u["uid"].as_str().unwrap_or(""),"fromName":u["name"].as_str().unwrap_or(""),"text":m.message(),"channel":channel}),
                                );
                            }
                            ControlPacket::UDPTunnel(packet) => {
                                if let VoicePacket::Audio {
                                    session_id,
                                    seq_num,
                                    payload,
                                    ..
                                } = *packet
                                {
                                    if !queue_audio(&queue, session_id, seq_num, &payload) {
                                        fail(
                                            &queue,
                                            "Mumble server sent invalid tunneled voice audio.",
                                            false,
                                        );
                                        return;
                                    }
                                }
                            }
                            ControlPacket::Reject(m) => {
                                let wrong = m.type_() == msgs::reject::RejectType::WrongServerPW
                                    || m.type_() == msgs::reject::RejectType::WrongUserPW;
                                fail(&queue, m.reason(), wrong);
                                return;
                            }
                            ControlPacket::PermissionDenied(m) => {
                                let channel = m.channel_id().to_string();
                                if let Some(request) = pending.iter().find_map(|(id, p)| {
                                    (p.channel.as_deref() == Some(channel.as_str())).then_some(id.clone())
                                }) {
                                    if let Some(p) = pending.remove(&request) {
                                        let needs_token = m.type_()
                                            == msgs::permission_denied::DenyType::Permission
                                            && m.permission() == 4;
                                        if needs_token {
                                            if let Some(token) = p.token {
                                                access_tokens.retain(|t| t != &token);
                                            }
                                        }
                                        emit(
                                            &queue,
                                            json!({"type":"result","request":request,"ok":false,"passwordRequired":needs_token,"error":if needs_token{"Mumble denied access to this channel."}else{m.reason()}}),
                                        );
                                    }
                                    continue;
                                }
                                emit(
                                    &queue,
                                    json!({"type":"error","message":m.reason(),"passwordRequired":false}),
                                );
                            }
                            _ => {}
                        }
                        if changed && synced && users.contains_key(&own) {
                            if !emit_bounded_state(
                                &queue,
                                read_state(
                                    &channels,
                                    &users,
                                    own,
                                    &server_name,
                                    &pin,
                                    synced,
                                    can_send,
                                    packet_limit,
                                ),
                            ) {
                                return;
                            }
                        }
                    }
                },
                _=ping.tick()=> {
                    {
                        let mut p = msgs::Ping::new();
                        p.set_timestamp(started.elapsed().as_millis() as u64);
                        let _ = send_control!(framed, p.into());
                    }
                },
                _=command_deadlines.tick()=> {
                    {
                        if (!synced || !users.contains_key(&own)) && Instant::now() >= authentication_deadline {
                            fail(&queue, "Mumble server synchronization timed out.", false);
                            return;
                        }
                        let now = Instant::now();
                        let expired = pending
                            .iter()
                            .filter_map(|(id, p)| (p.deadline <= now).then_some(id.clone()))
                            .collect::<Vec<_>>();
                        for id in expired {
                            if let Some(p) = pending.remove(&id) {
                                if let Some(token) = p.token {
                                    access_tokens.retain(|t| t != &token);
                                }
                                emit(
                                    &queue,
                                    json!({"type":"result","request":id,"ok":false,"error":"Mumble did not confirm the channel change."}),
                                );
                            }
                        }
                    }
                },
                _=udp_ping.tick()=> {
                    {
                        if last_udp.elapsed() > Duration::from_secs(3) {
                            udp_ready = false;
                        }
                        if udp.is_some() && last_udp.elapsed() > Duration::from_secs(5)
                            && last_resync.elapsed() > Duration::from_secs(5) {
                            last_resync = Instant::now();
                            let _ = send_control!(framed, msgs::CryptSetup::new().into());
                        }
                        if let Some(u) = udp.as_mut() {
                            let _ = timeout(
                                Duration::from_millis(250),
                                u.send((
                                    VoicePacket::Ping {
                                        timestamp: started.elapsed().as_millis() as u64,
                                    },
                                    address,
                                )),
                            )
                            .await;
                        }
                    }
                },
                voice=async { match udp.as_mut() { Some(u)=>u.next().await, None=>std::future::pending().await } }=>{
                    if let Some(Ok((packet,source)))=voice { if source==address { udp_ready=true;last_udp=Instant::now();match packet {
                        VoicePacket::Ping{..}=>{}
                        VoicePacket::Audio{session_id,seq_num,payload,..}=>if !queue_audio(&queue,session_id,seq_num,&payload){fail(&queue,"Mumble server sent invalid UDP voice audio.",false);return;}
                    }}}
                }
            }
        }
    });
}

pub fn open(ptr: *const u8, length: usize) -> *mut std::ffi::c_void {
    if ptr.is_null() || length == 0 || length > MAX_JSON {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(ptr, length) };
    let options: Options = match serde_json::from_slice(bytes) {
        Ok(v) => v,
        Err(_) => return std::ptr::null_mut(),
    };
    let (tx, rx) = tokio::sync::mpsc::channel(256);
    let (cancel_tx, cancel_rx) = tokio::sync::watch::channel(false);
    let queue = Queue::default();
    let worker_queue = queue.clone();
    let join = std::thread::spawn(move || {
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            run(options, rx, cancel_rx, worker_queue.clone())
        }))
        .is_err()
        {
            fail(&worker_queue, "Mumble connection failed.", false);
        }
    });
    Box::into_raw(Box::new(crate::Handle {
        tx,
        queue,
        join: std::sync::Mutex::new(Some(join)),
        max_audio: AUDIO_LIMIT,
        cancel: Some(cancel_tx),
    })) as *mut std::ffi::c_void
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalized_mumble_state_keeps_partial_members_and_external_contract() {
        let channels = BTreeMap::from([(
            3,
            json!({"id":"3","parent":"0","order":"0","name":"Lobby","password":false,"codec":5}),
        )]);
        let users = BTreeMap::from([(
            7,
            json!({"id":"7","uid":"cert","name":"Nox","channel":"3","inputMuted":false,"outputMuted":true,"metadata":"{}"}),
        )]);
        let state = read_state(
            &channels,
            &users,
            7,
            "Voice",
            "ab12",
            true,
            Some(true),
            1275,
        );
        assert_eq!(state["type"], "state");
        assert_eq!(state["server"]["uid"], "ab12");
        assert_eq!(state["self"], "7");
        assert_eq!(state["channel"], "3");
        assert_eq!(state["channels"][0]["codec"], 5);
        assert_eq!(state["members"][0]["uid"], "cert");
    }

    #[test]
    fn partial_user_updates_preserve_independent_self_server_and_suppress_flags() {
        let mut first = msgs::UserState::new();
        first.set_session(7);
        first.set_name("Speaker".into());
        first.set_channel_id(3);
        first.set_self_mute(true);
        first.set_self_deaf(true);
        let row = merge_user(7, &first, None).value;
        let mut server_mute = msgs::UserState::new();
        server_mute.set_session(7);
        server_mute.set_mute(true);
        server_mute.set_deaf(true);
        let row = merge_user(7, &server_mute, Some(&row)).value;
        assert!(row["inputMuted"].as_bool().unwrap());
        assert!(row["outputMuted"].as_bool().unwrap());
        let mut unmute_self = msgs::UserState::new();
        unmute_self.set_session(7);
        unmute_self.set_self_mute(false);
        unmute_self.set_self_deaf(false);
        let row = merge_user(7, &unmute_self, Some(&row)).value;
        assert!(row["inputMuted"].as_bool().unwrap());
        assert!(row["outputMuted"].as_bool().unwrap());
        let mut server_unmute = msgs::UserState::new();
        server_unmute.set_session(7);
        server_unmute.set_mute(false);
        server_unmute.set_deaf(false);
        let row = merge_user(7, &server_unmute, Some(&row)).value;
        assert!(!row["inputMuted"].as_bool().unwrap());
        assert!(!row["outputMuted"].as_bool().unwrap());
        let mut suppress = msgs::UserState::new();
        suppress.set_session(7);
        suppress.set_suppress(true);
        assert!(merge_user(7, &suppress, Some(&row)).value["inputMuted"]
            .as_bool()
            .unwrap());
    }

    #[test]
    fn malformed_options_and_oversized_json_are_rejected() {
        assert!(open(std::ptr::null(), 0).is_null());
        assert!(open(b"{}".as_ptr(), 2).is_null());
        let bytes = vec![b' '; MAX_JSON + 1];
        assert!(open(bytes.as_ptr(), bytes.len()).is_null());
    }

    #[test]
    fn access_tokens_are_bounded_and_deduplicated() {
        let mut tokens = Vec::new();
        assert_eq!(add_access_token(&mut tokens, "channel-pass"), Ok(true));
        assert_eq!(add_access_token(&mut tokens, "channel-pass"), Ok(false));
        assert_eq!(tokens, vec!["channel-pass"]);
        assert!(add_access_token(&mut tokens, &"x".repeat(1025)).is_err());
        for i in 0..31 {
            assert_eq!(
                add_access_token(&mut tokens, &format!("token-{i}")),
                Ok(true)
            );
        }
        assert!(add_access_token(&mut tokens, "last-token").is_err());
    }

    #[test]
    fn shared_ffi_audio_handle_accepts_mumble_packet_limit() {
        let (tx, mut rx) = tokio::sync::mpsc::channel(2);
        let handle = Box::into_raw(Box::new(crate::Handle {
            tx,
            queue: Queue::default(),
            join: std::sync::Mutex::new(None),
            max_audio: AUDIO_LIMIT,
            cancel: None,
        })) as *mut std::ffi::c_void;
        let packet = vec![0x55; 1000];
        assert_eq!(crate::ss_ts_audio(handle, packet.as_ptr(), packet.len()), 1);
        assert!(matches!(rx.try_recv(),Ok(crate::Command::Audio(data)) if data==packet));
        let oversized = vec![0; AUDIO_LIMIT + 1];
        assert_eq!(
            crate::ss_ts_audio(handle, oversized.as_ptr(), oversized.len()),
            0
        );
        crate::ss_ts_close(handle);
    }

    #[test]
    fn mumble_audio_uses_ten_millisecond_ticks_and_same_udp_tunnel_event() {
        let mut error = 0;
        let encoder = unsafe {
            audiopus_sys::opus_encoder_create(
                48_000,
                1,
                audiopus_sys::OPUS_APPLICATION_VOIP,
                &mut error,
            )
        };
        assert!(!encoder.is_null());
        assert_eq!(error, audiopus_sys::OPUS_OK);
        let pcm = vec![0i16; 960];
        let mut encoded = vec![0u8; AUDIO_LIMIT];
        let size = unsafe {
            audiopus_sys::opus_encode(
                encoder,
                pcm.as_ptr(),
                pcm.len() as i32,
                encoded.as_mut_ptr(),
                encoded.len() as i32,
            )
        };
        unsafe { audiopus_sys::opus_encoder_destroy(encoder) };
        assert!(size > 0);
        encoded.truncate(size as usize);
        assert_eq!(opus_samples(&encoded), Some(960));
        assert_eq!(audio_ticks(&encoded), Some(2));
        let payload = VoicePacketPayload::Opus(Bytes::from(encoded), false);
        let event = audio_event(0x01020304, 0x0102030405060708, &payload).unwrap();
        assert_eq!(&event[..14], &[3, 1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 0]);
        assert!(
            audio_event(9, 10, &VoicePacketPayload::Opus(Bytes::new(), true))
                .unwrap()
                .len()
                == 14
        );
        assert!(audio_event(9, 10, &VoicePacketPayload::Speex(vec![])).is_err());
    }

    #[test]
    fn unknown_future_mumble_control_packets_are_ignored_safely() {
        let raw = mumble_protocol::control::RawControlPacket {
            id: u16::MAX,
            bytes: Bytes::new(),
        };
        assert!(matches!(
            ControlPacket::<mumble_protocol::voice::Clientbound>::try_from(raw),
            Ok(ControlPacket::Other(_))
        ));
    }

    #[test]
    fn close_cancels_an_inflight_mumble_connect() {
        use openssl::{
            bn::BigNum,
            pkey::PKey,
            rsa::Rsa,
            x509::{X509NameBuilder, X509},
        };
        let key = PKey::from_rsa(Rsa::generate(2048).unwrap()).unwrap();
        let mut name = X509NameBuilder::new().unwrap();
        name.append_entry_by_text("CN", "adapter-test").unwrap();
        let name = name.build();
        let mut cert = X509::builder().unwrap();
        cert.set_version(2).unwrap();
        let serial = BigNum::from_u32(1).unwrap().to_asn1_integer().unwrap();
        cert.set_serial_number(&serial).unwrap();
        cert.set_subject_name(&name).unwrap();
        cert.set_issuer_name(&name).unwrap();
        cert.set_pubkey(&key).unwrap();
        cert.set_not_before(&openssl::asn1::Asn1Time::days_from_now(0).unwrap())
            .unwrap();
        cert.set_not_after(&openssl::asn1::Asn1Time::days_from_now(1).unwrap())
            .unwrap();
        cert.sign(&key, MessageDigest::sha256()).unwrap();
        let pem = [
            cert.build().to_pem().unwrap(),
            key.private_key_to_pem_pkcs8().unwrap(),
        ]
        .concat();
        let options = json!({"address":"192.0.2.1:64738","identityPem":String::from_utf8(pem).unwrap(),"inputMuted":true,"outputMuted":true});
        let encoded = serde_json::to_vec(&options).unwrap();
        let handle = crate::ss_mumble_open(encoded.as_ptr(), encoded.len());
        assert!(!handle.is_null());
        let start = Instant::now();
        crate::ss_ts_close(handle);
        assert!(start.elapsed() < Duration::from_secs(2));
    }
}
