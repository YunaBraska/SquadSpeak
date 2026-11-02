//! Mumble's upstream packet cipher, with the existing protocol voice framing.
use bytes::BytesMut;
use mumble_protocol::voice::{Clientbound, Serverbound, VoicePacket};
use crate::mumble_voice::VoiceWire;
use std::{ffi::c_void, io, ptr::NonNull};
use tokio_util::codec::{Decoder, Encoder};

extern "C" {
    fn squad_mumble_crypt_new(
        key: *const u8,
        encrypt: *const u8,
        decrypt: *const u8,
    ) -> *mut c_void;
    fn squad_mumble_crypt_free(state: *mut c_void);
    fn squad_mumble_crypt_nonce(state: *mut c_void, nonce: *mut u8, set: bool) -> bool;
    fn squad_mumble_crypt_packet(
        state: *mut c_void,
        input: *const u8,
        output: *mut u8,
        length: u32,
        encrypt: bool,
    ) -> bool;
}

pub struct ClientCryptState(NonNull<c_void>, VoiceWire);
// Each state belongs exclusively to one connection task. The upstream object
// contains no thread-local or shared state and is never accessed concurrently.
unsafe impl Send for ClientCryptState {}

impl ClientCryptState {
    pub fn new_from(key: [u8; 16], encrypt: [u8; 16], decrypt: [u8; 16], modern: bool) -> io::Result<Self> {
        NonNull::new(unsafe {
            squad_mumble_crypt_new(key.as_ptr(), encrypt.as_ptr(), decrypt.as_ptr())
        })
        .map(|state| Self(state, VoiceWire::new(modern)))
        .ok_or_else(|| io::Error::other("Mumble encryption initialization failed"))
    }
    pub fn get_encrypt_nonce(&self) -> [u8; 16] {
        let mut nonce = [0; 16];
        unsafe {
            squad_mumble_crypt_nonce(self.0.as_ptr(), nonce.as_mut_ptr(), false);
        }
        nonce
    }
    pub fn set_decrypt_nonce(&mut self, nonce: &[u8; 16]) {
        let mut nonce = *nonce;
        unsafe {
            squad_mumble_crypt_nonce(self.0.as_ptr(), nonce.as_mut_ptr(), true);
        }
    }
}
impl Drop for ClientCryptState {
    fn drop(&mut self) {
        unsafe {
            squad_mumble_crypt_free(self.0.as_ptr());
        }
    }
}
impl Encoder<VoicePacket<Serverbound>> for ClientCryptState {
    type Error = io::Error;
    fn encode(&mut self, packet: VoicePacket<Serverbound>, dst: &mut BytesMut) -> io::Result<()> {
        let mut plain = BytesMut::new();
        self.1.encode(packet, &mut plain)?;
        if plain.is_empty() || plain.len() > 8192 {
            return Err(io::Error::other("Invalid Mumble voice size"));
        }
        dst.resize(plain.len() + 4, 0);
        if !unsafe {
            squad_mumble_crypt_packet(
                self.0.as_ptr(),
                plain.as_ptr(),
                dst.as_mut_ptr(),
                plain.len() as u32,
                true,
            )
        } {
            dst.clear();
            return Err(io::Error::other("Mumble encryption failed"));
        }
        Ok(())
    }
}
impl Decoder for ClientCryptState {
    type Item = VoicePacket<Clientbound>;
    type Error = io::Error;
    fn decode(&mut self, src: &mut BytesMut) -> io::Result<Option<Self::Item>> {
        if src.len() <= 4 || src.len() > 8196 {
            src.clear();
            return Ok(None);
        }
        let mut plain = BytesMut::zeroed(src.len() - 4);
        let valid = unsafe {
            squad_mumble_crypt_packet(
                self.0.as_ptr(),
                src.as_ptr(),
                plain.as_mut_ptr(),
                src.len() as u32,
                false,
            )
        };
        src.clear();
        if !valid {
            return Ok(None);
        }
        self.1.decode(&mut plain)
    }
}
