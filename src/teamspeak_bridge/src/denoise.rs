use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};

const HOP: usize = 480;

struct Denoise {
    pristine: Vec<DfTract>,
    streams: Vec<DfTract>,
    peaks: [f32; 2],
    gains: [[f32; 3]; 2],
    position: usize,
}

impl Denoise {
    fn reset(&mut self) {
        for (stream, pristine) in self.streams.iter_mut().zip(&self.pristine) {
            // Each channel owns its template. The only shared mutable feature
            // workspace is overwritten on every call and the template never runs.
            // Calling init() again would append a second spectral history.
            *stream = pristine.clone();
        }
        self.peaks = [0.; 2];
        self.gains = [[1.; 3]; 2];
        self.position = 0;
    }

    fn process(&mut self, input: &[f32], output: &mut [f32], snr: &mut f32) -> bool {
        let mut estimates = [f32::INFINITY; 2];
        let channels = self.streams.len();
        let mut source = [0f32; HOP];
        let mut filtered = [0f32; HOP];
        for half in 0..2 {
            for (channel, stream) in self.streams.iter_mut().enumerate() {
                for i in 0..HOP {
                    source[i] = input[(half * HOP + i) * channels + channel].clamp(-1., 1.);
                }
                self.peaks[channel] = source
                    .iter()
                    .map(|value| value.abs())
                    .fold(self.peaks[channel] * 0.995, f32::max);
                let gain = (0.5 / self.peaks[channel].max(1e-12)).min(32.);
                for sample in &mut source {
                    *sample *= gain;
                }
                let Ok(estimate) = stream.process(
                    ArrayView2::from_shape((1, HOP), &source).unwrap(),
                    ArrayViewMut2::from_shape((1, HOP), &mut filtered).unwrap(),
                ) else {
                    return false;
                };
                if !estimate.is_finite() || filtered.iter().any(|value| !value.is_finite()) {
                    return false;
                }
                estimates[channel] = estimates[channel].min(estimate);
                for i in 0..HOP {
                    output[(half * HOP + i) * channels + channel] =
                        filtered[i] / self.gains[channel][self.position];
                }
                self.gains[channel][self.position] = gain;
            }
            self.position = (self.position + 1) % 3;
        }
        *snr = estimates[..channels]
            .iter()
            .copied()
            .fold(f32::NEG_INFINITY, f32::max);
        true
    }
}

#[no_mangle]
pub extern "C" fn squad_denoise_create(channels: i32) -> *mut c_void {
    catch_unwind(|| {
        if channels != 1 && channels != 2 {
            return std::ptr::null_mut();
        }
        let mut pristine = Vec::with_capacity(channels as usize);
        for _ in 0..channels {
            let Ok(model) = DfTract::new(
                DfParams::default(),
                &RuntimeParams::default().with_thresholds(-15., 35., 35.),
            ) else {
                return std::ptr::null_mut();
            };
            if model.hop_size != HOP
                || model.sr != 48000
                || model.fft_size - HOP + model.lookahead * HOP != 1440
            {
                return std::ptr::null_mut();
            }
            pristine.push(model);
        }
        let state = Box::new(Denoise {
            streams: pristine.clone(),
            pristine,
            peaks: [0.; 2],
            gains: [[1.; 3]; 2],
            position: 0,
        });
        Box::into_raw(state).cast()
    })
    .unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn squad_denoise_destroy(state: *mut c_void) {
    if !state.is_null() {
        drop(Box::from_raw(state.cast::<Denoise>()));
    }
}

#[no_mangle]
pub unsafe extern "C" fn squad_denoise_reset(state: *mut c_void) -> bool {
    if state.is_null() {
        return false;
    }
    catch_unwind(AssertUnwindSafe(|| {
        (&mut *state.cast::<Denoise>()).reset();
        true
    }))
    .unwrap_or(false)
}

#[no_mangle]
pub unsafe extern "C" fn squad_denoise_process(
    state: *mut c_void,
    input: *const f32,
    output: *mut f32,
    snr: *mut f32,
) -> bool {
    if state.is_null() || input.is_null() || output.is_null() || snr.is_null() {
        return false;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let state = &mut *state.cast::<Denoise>();
        let count = 2 * HOP * state.streams.len();
        let input = std::slice::from_raw_parts(input, count);
        if input.iter().any(|value| !value.is_finite()) {
            return false;
        }
        state.process(
            input,
            std::slice::from_raw_parts_mut(output, count),
            &mut *snr,
        )
    }))
    .unwrap_or(false)
}
