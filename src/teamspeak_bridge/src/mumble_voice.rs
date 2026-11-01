//! Legacy and Mumble 1.5 protobuf voice packet codecs.
//!
//! The regular Mumble UDP packet format is selected by the server's advertised
//! protocol version. TCP UDPTunnel payloads use the same voice packet format.

use bytes::{Buf, Bytes, BytesMut};
use mumble_protocol::{
    control::{msgs, ClientControlCodec, ControlPacket, RawControlCodec, RawControlPacket},
    voice::{ClientVoiceCodec, Clientbound, Serverbound, VoicePacket, VoicePacketPayload},
};
use protobuf::{CodedInputStream, CodedOutputStream};
use std::io;
use tokio_util::codec::{Decoder, Encoder};

const MAX_VOICE_PACKET: usize = 8192;
const MAX_CONTROL_PACKET: usize = 0x7f_ffff;
const UDP_AUDIO: u8 = 0;
const UDP_PING: u8 = 1;

fn invalid_data(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn protobuf_error(_: protobuf::Error) -> io::Error {
    invalid_data("malformed Mumble protobuf voice packet")
}

fn encode_protobuf_voice<D>(packet: VoicePacket<D>) -> io::Result<BytesMut>
where
    D: mumble_protocol::voice::VoicePacketDst,
{
    let mut output = Vec::with_capacity(256);
    match packet {
        VoicePacket::Ping { timestamp } => {
            output.push(UDP_PING);
            let mut coded = CodedOutputStream::vec(&mut output);
            coded.write_uint64(1, timestamp).map_err(protobuf_error)?;
            coded.flush().map_err(protobuf_error)?;
        }
        VoicePacket::Audio {
            target,
            seq_num,
            payload: VoicePacketPayload::Opus(opus, terminator),
            position_info,
            ..
        } => {
            if target > 31 {
                return Err(invalid_data("Mumble voice target is out of range"));
            }
            if position_info.is_some()
                || opus.len() > MAX_VOICE_PACKET - 1
                || (opus.is_empty() && !terminator)
            {
                return Err(invalid_data(
                    "Mumble protobuf audio packet is out of range or positional audio is unsupported",
                ));
            }
            output.push(UDP_AUDIO);
            let mut coded = CodedOutputStream::vec(&mut output);
            coded
                .write_uint32(1, u32::from(target))
                .map_err(protobuf_error)?;
            coded.write_uint64(4, seq_num).map_err(protobuf_error)?;
            coded.write_bytes(5, &opus).map_err(protobuf_error)?;
            coded.write_bool(16, terminator).map_err(protobuf_error)?;
            coded.flush().map_err(protobuf_error)?;
        }
        VoicePacket::Audio { .. } => {
            return Err(invalid_data("Mumble protobuf only supports Opus audio"));
        }
    }
    if output.len() > MAX_VOICE_PACKET {
        return Err(invalid_data("Mumble protobuf voice packet is too large"));
    }
    Ok(BytesMut::from(output.as_slice()))
}

enum ParsedVoice {
    Ping {
        timestamp: u64,
    },
    Audio {
        context: u32,
        sender: u32,
        frame: u64,
        opus: Vec<u8>,
        terminator: bool,
    },
}

fn read_protobuf_voice(packet: &[u8]) -> io::Result<ParsedVoice> {
    if packet.is_empty() || packet.len() > MAX_VOICE_PACKET {
        return Err(invalid_data(
            "Mumble protobuf voice packet has invalid size",
        ));
    }
    let kind = packet[0];
    let mut input = CodedInputStream::from_bytes(&packet[1..]);
    input.set_recursion_limit(32);
    let mut timestamp = 0;
    let mut context = 0;
    let mut sender = 0;
    let mut frame = 0;
    let mut opus = Vec::new();
    let mut terminator = false;
    loop {
        let Some(tag) = input.read_raw_tag_or_eof().map_err(protobuf_error)? else {
            break;
        };
        let field = tag >> 3;
        let wire = tag & 7;
        if field == 0 {
            return Err(invalid_data("invalid Mumble protobuf field tag"));
        }
        match (kind, field) {
            (UDP_PING, 1) if wire == 0 => {
                timestamp = input.read_uint64().map_err(protobuf_error)?;
            }
            (UDP_AUDIO, 1 | 2) if wire == 0 => {
                context = input.read_uint32().map_err(protobuf_error)?;
            }
            (UDP_AUDIO, 3) if wire == 0 => {
                sender = input.read_uint32().map_err(protobuf_error)?;
            }
            (UDP_AUDIO, 4) if wire == 0 => {
                frame = input.read_uint64().map_err(protobuf_error)?;
            }
            (UDP_AUDIO, 5) if wire == 2 => {
                let len = input.read_raw_varint32().map_err(protobuf_error)? as usize;
                if len > MAX_VOICE_PACKET - 1 || opus.len().saturating_add(len) > MAX_VOICE_PACKET {
                    return Err(invalid_data("Mumble Opus payload exceeds the packet limit"));
                }
                opus = input.read_raw_bytes(len as u32).map_err(protobuf_error)?;
            }
            (UDP_AUDIO, 16) if wire == 0 => {
                terminator = input.read_uint32().map_err(protobuf_error)? != 0;
            }
            (UDP_PING, _) | (UDP_AUDIO, _) => {
                if wire == 3 || wire == 4 {
                    return Err(invalid_data("Mumble protobuf group fields are unsupported"));
                }
                protobuf::rt::skip_field_for_tag(tag, &mut input).map_err(protobuf_error)?;
            }
            _ => return Err(invalid_data("unsupported Mumble UDP message type")),
        }
    }
    match kind {
        UDP_PING => Ok(ParsedVoice::Ping { timestamp }),
        UDP_AUDIO if !opus.is_empty() || terminator => Ok(ParsedVoice::Audio {
            context,
            sender,
            frame,
            opus,
            terminator,
        }),
        UDP_AUDIO => Err(invalid_data("Mumble audio packet has no Opus payload")),
        _ => Err(invalid_data("unsupported Mumble UDP message type")),
    }
}

/// Voice codec selected according to the negotiated server protocol version.
pub(crate) struct VoiceWire {
    /// `true` selects the protobuf UDP framing introduced by Mumble 1.5.
    pub(crate) modern: bool,
    legacy: ClientVoiceCodec,
}

impl VoiceWire {
    /// Creates a codec for legacy or Mumble 1.5 protobuf voice packets.
    pub(crate) fn new(modern: bool) -> Self {
        Self {
            modern,
            legacy: ClientVoiceCodec::new(),
        }
    }

    fn decode_modern(
        &mut self,
        src: &mut BytesMut,
    ) -> io::Result<Option<VoicePacket<Clientbound>>> {
        if src.is_empty() {
            return Ok(None);
        }
        let parsed = read_protobuf_voice(src)?;
        src.advance(src.len());
        Ok(Some(match parsed {
            ParsedVoice::Ping { timestamp } => VoicePacket::Ping { timestamp },
            ParsedVoice::Audio {
                context,
                sender,
                frame,
                opus,
                terminator,
            } => {
                let target = u8::try_from(context)
                    .map_err(|_| invalid_data("Mumble audio context exceeds supported range"))?;
                VoicePacket::Audio {
                    _dst: std::marker::PhantomData,
                    target,
                    session_id: sender,
                    seq_num: frame,
                    payload: VoicePacketPayload::Opus(Bytes::from(opus), terminator),
                    position_info: None,
                }
            }
        }))
    }
}

impl Decoder for VoiceWire {
    type Item = VoicePacket<Clientbound>;
    type Error = io::Error;

    fn decode(&mut self, src: &mut BytesMut) -> io::Result<Option<Self::Item>> {
        if src.len() > MAX_VOICE_PACKET {
            return Err(invalid_data("Mumble voice packet is too large"));
        }
        if self.modern {
            self.decode_modern(src)
        } else {
            self.legacy.decode(src)
        }
    }
}

impl Encoder<VoicePacket<Serverbound>> for VoiceWire {
    type Error = io::Error;

    fn encode(&mut self, packet: VoicePacket<Serverbound>, dst: &mut BytesMut) -> io::Result<()> {
        let mut encoded = if self.modern {
            encode_protobuf_voice(packet)?
        } else {
            let mut bytes = BytesMut::new();
            self.legacy.encode(packet, &mut bytes)?;
            bytes
        };
        if encoded.len() > MAX_VOICE_PACKET {
            return Err(invalid_data("Mumble voice packet is too large"));
        }
        dst.extend_from_slice(&encoded.split());
        Ok(())
    }
}

/// Control codec that switches just UDPTunnel payloads with the voice codec.
pub(crate) struct ControlWire {
    /// `true` selects protobuf framing for tunneled voice packets.
    pub(crate) modern: bool,
    legacy: ClientControlCodec,
    raw: RawControlCodec,
    voice: VoiceWire,
}

impl ControlWire {
    /// Creates a codec for legacy or Mumble 1.5 control-channel tunneling.
    pub(crate) fn new(modern: bool) -> Self {
        Self {
            modern,
            legacy: ClientControlCodec::new(),
            raw: RawControlCodec::new(),
            voice: VoiceWire::new(modern),
        }
    }
}

impl Decoder for ControlWire {
    type Item = ControlPacket<Clientbound>;
    type Error = io::Error;

    fn decode(&mut self, src: &mut BytesMut) -> io::Result<Option<Self::Item>> {
        if !self.modern {
            return self.legacy.decode(src);
        }
        if src.len() >= 6 {
            let id = u16::from_be_bytes([src[0], src[1]]);
            let len = u32::from_be_bytes([src[2], src[3], src[4], src[5]]) as usize;
            if id == msgs::id::UDPTunnel && len > MAX_VOICE_PACKET {
                return Err(invalid_data("Mumble tunneled voice packet is too large"));
            }
            if len > MAX_CONTROL_PACKET {
                return Err(invalid_data("Mumble control packet is too large"));
            }
        }
        let Some(raw) = self.raw.decode(src)? else {
            return Ok(None);
        };
        if raw.id == msgs::id::UDPTunnel {
            self.voice.modern = self.modern;
            let mut payload = BytesMut::from(raw.bytes.as_ref());
            let packet = self
                .voice
                .decode(&mut payload)?
                .ok_or_else(|| invalid_data("empty Mumble tunneled voice packet"))?;
            if !payload.is_empty() {
                return Err(invalid_data(
                    "trailing bytes in Mumble tunneled voice packet",
                ));
            }
            Ok(Some(ControlPacket::UDPTunnel(Box::new(packet))))
        } else {
            ControlPacket::<Clientbound>::try_from(raw)
                .map(Some)
                .map_err(protobuf_error)
        }
    }
}

impl Encoder<ControlPacket<Serverbound>> for ControlWire {
    type Error = io::Error;

    fn encode(&mut self, packet: ControlPacket<Serverbound>, dst: &mut BytesMut) -> io::Result<()> {
        if !self.modern {
            return self.legacy.encode(packet, dst);
        }
        let raw = match packet {
            ControlPacket::UDPTunnel(packet) => {
                self.voice.modern = self.modern;
                let mut payload = BytesMut::new();
                self.voice.encode(*packet, &mut payload)?;
                RawControlPacket {
                    id: msgs::id::UDPTunnel,
                    bytes: payload.freeze(),
                }
            }
            other => other.into(),
        };
        if raw.bytes.len() > MAX_CONTROL_PACKET {
            return Err(invalid_data("Mumble control packet is too large"));
        }
        self.raw.encode(raw, dst)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server_audio() -> VoicePacket<Clientbound> {
        VoicePacket::Audio {
            _dst: std::marker::PhantomData,
            target: 0,
            session_id: 150,
            seq_num: 300,
            payload: VoicePacketPayload::Opus(Bytes::from_static(&[0xaa, 0xbb, 0xcc]), true),
            position_info: None,
        }
    }

    #[test]
    fn modern_audio_matches_protocol_wire_fields_and_skips_unknown_fields() {
        let known = [
            UDP_AUDIO, 0x18, 0x96, 0x01, 0x20, 0xac, 0x02, 0x2a, 0x03, 0xaa, 0xbb, 0xcc, 0x80,
            0x01, 0x01,
        ];
        let mut bytes = BytesMut::from(&known[..]);
        let mut codec = VoiceWire::new(true);
        assert_eq!(codec.decode(&mut bytes).unwrap(), Some(server_audio()));
        let mut with_unknown = known.to_vec();
        with_unknown.extend_from_slice(&[0x48, 0x07]); // field 9, varint 7
        with_unknown.extend_from_slice(&[0x32, 0x04, 0, 0, 0, 0]); // packed positional field, ignored
        let mut bytes = BytesMut::from(with_unknown.as_slice());
        assert_eq!(codec.decode(&mut bytes).unwrap(), Some(server_audio()));
    }

    #[test]
    fn modern_audio_and_ping_encode_expected_protobuf() {
        let packet = VoicePacket::Audio {
            _dst: std::marker::PhantomData,
            target: 0,
            session_id: (),
            seq_num: 300,
            payload: VoicePacketPayload::Opus(Bytes::from_static(&[0xaa, 0xbb, 0xcc]), true),
            position_info: None,
        };
        let mut codec = VoiceWire::new(true);
        let mut bytes = BytesMut::new();
        codec.encode(packet, &mut bytes).unwrap();
        assert_eq!(
            &bytes[..],
            &[
                UDP_AUDIO, 0x08, 0x00, 0x20, 0xac, 0x02, 0x2a, 0x03, 0xaa, 0xbb, 0xcc, 0x80, 0x01,
                0x01
            ]
        );
        bytes.clear();
        codec
            .encode(VoicePacket::Ping { timestamp: 150 }, &mut bytes)
            .unwrap();
        assert_eq!(&bytes[..], &[UDP_PING, 0x08, 0x96, 0x01]);
        let mut reply = BytesMut::from(&[UDP_PING, 0x08, 0x96, 0x01][..]);
        assert_eq!(
            codec.decode(&mut reply).unwrap(),
            Some(VoicePacket::Ping { timestamp: 150 })
        );
    }

    #[test]
    fn truncated_and_oversized_voice_packets_fail() {
        let mut codec = VoiceWire::new(true);
        let mut truncated = BytesMut::from(&[UDP_AUDIO, 0x2a, 0x04, 0xaa][..]);
        assert_eq!(
            codec.decode(&mut truncated).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
        let mut oversized = BytesMut::from(vec![UDP_AUDIO; MAX_VOICE_PACKET + 1].as_slice());
        assert_eq!(
            codec.decode(&mut oversized).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[test]
    fn deeply_nested_unknown_groups_fail_with_bounded_recursion() {
        let mut packet = vec![UDP_AUDIO, 0x2a, 0x01, 0xf8];
        packet.extend(std::iter::repeat_n(0x4b, 40)); // field 9 start-group
        packet.extend(std::iter::repeat_n(0x4c, 40)); // field 9 end-group
        let mut codec = VoiceWire::new(true);
        let mut bytes = BytesMut::from(packet.as_slice());
        assert!(codec.decode(&mut bytes).is_err());
    }

    #[test]
    fn legacy_voice_format_remains_unchanged() {
        let mut codec = VoiceWire::new(false);
        let mut bytes = BytesMut::from(&[0x80, 0x07, 0x02, 0x03, 0xaa, 0xbb, 0xcc][..]);
        let packet = codec.decode(&mut bytes).unwrap().unwrap();
        assert!(
            matches!(packet, VoicePacket::Audio { seq_num: 2, payload: VoicePacketPayload::Opus(ref data, false), .. } if data.as_ref() == [0xaa, 0xbb, 0xcc])
        );
    }

    #[test]
    fn control_tunnel_modern_framing_handles_split_input_and_roundtrips() {
        let packet = VoicePacket::Audio {
            _dst: std::marker::PhantomData,
            target: 0,
            session_id: (),
            seq_num: 9,
            payload: VoicePacketPayload::Opus(Bytes::from_static(&[0xf8]), false),
            position_info: None,
        };
        let mut codec = ControlWire::new(false);
        // This is how a version-negotiated Framed codec switches after ServerSync.
        codec.modern = true;
        let mut encoded = BytesMut::new();
        codec
            .encode(ControlPacket::UDPTunnel(Box::new(packet)), &mut encoded)
            .unwrap();
        assert_eq!(
            u16::from_be_bytes([encoded[0], encoded[1]]),
            msgs::id::UDPTunnel
        );
        let split = encoded.len() / 2;
        let tail = encoded.split_off(split);
        assert!(codec.decode(&mut encoded).unwrap().is_none());
        encoded.extend_from_slice(&tail);
        assert!(
            matches!(codec.decode(&mut encoded).unwrap(), Some(ControlPacket::UDPTunnel(packet)) if matches!(packet.as_ref(), VoicePacket::Audio { seq_num: 9, .. }))
        );
    }

    #[test]
    fn control_codec_delegates_unknown_packets_and_enforces_tunnel_limit() {
        let mut codec = ControlWire::new(true);
        let unknown = RawControlPacket {
            id: u16::MAX,
            bytes: Bytes::from_static(&[1, 2]),
        };
        let mut frame = BytesMut::new();
        codec
            .encode(
                ControlPacket::<Serverbound>::Other(unknown.clone()),
                &mut frame,
            )
            .unwrap();
        assert!(
            matches!(codec.decode(&mut frame).unwrap(), Some(ControlPacket::Other(packet)) if packet == unknown)
        );

        let mut too_large = BytesMut::new();
        too_large.extend_from_slice(&msgs::id::UDPTunnel.to_be_bytes());
        too_large.extend_from_slice(&((MAX_VOICE_PACKET + 1) as u32).to_be_bytes());
        assert_eq!(
            codec.decode(&mut too_large).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }
}
