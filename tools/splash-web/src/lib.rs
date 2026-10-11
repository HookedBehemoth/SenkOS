use lz4::{BlockMode, BlockSize, ContentChecksum, EncoderBuilder, liblz4::BlockChecksum};
use std::{
    alloc::{Layout, alloc, dealloc},
    io::{Cursor, Write},
    slice,
    sync::Mutex,
};
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

const WIDTH: usize = 640;
const HEIGHT: usize = 480;
const PIXEL_BYTES: usize = WIDTH * HEIGHT * 4;
static RESULT: Mutex<Option<Box<[u8]>>> = Mutex::new(None);

fn splash(pixels: &[u8], bg: [u8; 3]) -> Result<Vec<u8>, u32> {
    if pixels.len() != PIXEL_BYTES {
        return Err(1);
    }
    let mut left = WIDTH;
    let mut top = HEIGHT;
    let mut right = 0;
    let mut bottom = 0;
    for y in 0..HEIGHT {
        for x in 0..WIDTH {
            let offset = (y * WIDTH + x) * 4;
            if pixels[offset..offset + 3] != bg {
                left = left.min(x);
                top = top.min(y);
                right = right.max(x + 1);
                bottom = bottom.max(y + 1);
            }
        }
    }
    let (left, top, right, bottom) = if right == 0 {
        (WIDTH / 2, HEIGHT / 2, WIDTH / 2 + 1, HEIGHT / 2 + 1)
    } else {
        (
            left.min(WIDTH - right),
            top.min(HEIGHT - bottom),
            right.max(WIDTH - left),
            bottom.max(HEIGHT - top),
        )
    };
    let crop_width = right - left;
    let crop_height = bottom - top;
    let mut raw = Vec::with_capacity(8 + crop_width * crop_height * 4);
    raw.extend_from_slice(&(crop_width as u16).to_le_bytes());
    raw.extend_from_slice(&(crop_height as u16).to_le_bytes());
    raw.extend_from_slice(&[bg[2], bg[1], bg[0], 0]);
    for y in top..bottom {
        for x in left..right {
            let offset = (y * WIDTH + x) * 4;
            raw.extend_from_slice(&[pixels[offset + 2], pixels[offset + 1], pixels[offset], 0]);
        }
    }

    // Match mksplash.py's lz4 -12, with independent blocks for U-Boot.
    let mut encoder = EncoderBuilder::new()
        .level(12)
        .block_size(BlockSize::Max1MB)
        .block_mode(BlockMode::Independent)
        .block_checksum(BlockChecksum::NoBlockChecksum)
        .checksum(ContentChecksum::ChecksumEnabled)
        .build(Vec::new())
        .map_err(|_| 3u32)?;
    encoder.write_all(&raw).map_err(|_| 3u32)?;
    let (bytes, result) = encoder.finish();
    result.map_err(|_| 3u32)?;
    Ok(bytes)
}

fn bundle(pixels: &[u8], color: u32) -> Result<Vec<u8>, u32> {
    let bg = [(color >> 16) as u8, (color >> 8) as u8, color as u8];
    let splash = splash(pixels, bg)?;
    let mut archive = ZipWriter::new(Cursor::new(Vec::new()));
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Stored);
    archive
        .start_file("splash-custom.lz4", options)
        .map_err(|_| 3u32)?;
    archive.write_all(&splash).map_err(|_| 3u32)?;
    archive
        .start_file("splash.txt", options)
        .map_err(|_| 3u32)?;
    archive
        .write_all(b"splash=splash-custom.lz4\n")
        .map_err(|_| 3u32)?;
    archive
        .finish()
        .map(|cursor| cursor.into_inner())
        .map_err(|_| 3u32)
}

#[unsafe(no_mangle)]
pub extern "C" fn allocate(length: usize) -> *mut u8 {
    let Ok(layout) = Layout::array::<u8>(length) else {
        return std::ptr::null_mut();
    };
    unsafe { alloc(layout) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn deallocate(pointer: *mut u8, length: usize) {
    if let (false, Ok(layout)) = (pointer.is_null(), Layout::array::<u8>(length)) {
        unsafe {
            dealloc(pointer, layout);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn make(pointer: *const u8, length: usize, color: u32) -> u32 {
    if pointer.is_null() || length != PIXEL_BYTES {
        return 1;
    }
    let pixels = unsafe { slice::from_raw_parts(pointer, length) };
    match bundle(pixels, color) {
        Ok(bytes) => {
            *RESULT.lock().unwrap() = Some(bytes.into_boxed_slice());
            0
        }
        Err(code) => {
            *RESULT.lock().unwrap() = None;
            code
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn result_pointer() -> *const u8 {
    RESULT
        .lock()
        .unwrap()
        .as_ref()
        .map_or(std::ptr::null(), |bytes| bytes.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn result_length() -> usize {
    RESULT
        .lock()
        .unwrap()
        .as_ref()
        .map_or(0, |bytes| bytes.len())
}

#[unsafe(no_mangle)]
pub extern "C" fn clear_result() {
    *RESULT.lock().unwrap() = None;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    #[test]
    fn crops_and_packages_for_bootloader() {
        let mut pixels = vec![0; PIXEL_BYTES];
        for pixel in pixels.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[0x12, 0x34, 0x56, 255]);
        }
        let offset = ((HEIGHT / 2 - 1) * WIDTH + (WIDTH / 2 - 1)) * 4;
        pixels[offset..offset + 4].copy_from_slice(&[255, 0, 0, 255]);
        let zip = bundle(&pixels, 0x123456).unwrap();
        let mut archive = zip::ZipArchive::new(Cursor::new(zip)).unwrap();
        let mut selector = String::new();
        archive
            .by_name("splash.txt")
            .unwrap()
            .read_to_string(&mut selector)
            .unwrap();
        assert_eq!(selector, "splash=splash-custom.lz4\n");
        let mut frame = Vec::new();
        archive
            .by_name("splash-custom.lz4")
            .unwrap()
            .read_to_end(&mut frame)
            .unwrap();
        assert_eq!(frame[4] & 0x20, 0x20);
        let mut raw = Vec::new();
        lz4::Decoder::new(Cursor::new(frame))
            .unwrap()
            .read_to_end(&mut raw)
            .unwrap();
        assert_eq!(&raw[..8], &[2, 0, 2, 0, 0x56, 0x34, 0x12, 0]);
        assert_eq!(&raw[8..12], &[0, 0, 255, 0]);
    }
}
