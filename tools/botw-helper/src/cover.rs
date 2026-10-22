use image::{DynamicImage, ImageFormat, imageops::FilterType};
use std::path::Path;

pub fn decode_tga(bytes: &[u8]) -> image::ImageResult<DynamicImage> {
    image::load_from_memory_with_format(bytes, ImageFormat::Tga)
}

pub fn write_covers(
    icon: &DynamicImage,
    wide: &DynamicImage,
    directory: &Path,
) -> image::ImageResult<()> {
    icon.resize_to_fill(300, 300, FilterType::Lanczos3)
        .save_with_format(directory.join("cover.png"), ImageFormat::Png)?;
    wide.resize_to_fill(620, 300, FilterType::Lanczos3)
        .save_with_format(directory.join("cover-wide.png"), ImageFormat::Png)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn raw_and_rle_tga_preserve_pixels_and_write_tile_dimensions() {
        for depth in [24, 32] {
            for rle in [false, true] {
                for origin in [0, 0x10, 0x20, 0x30] {
                    let mut bytes = vec![0u8; 18];
                    bytes[2] = if rle { 10 } else { 2 };
                    bytes[12] = 2;
                    bytes[14] = 1;
                    bytes[16] = depth;
                    bytes[17] = origin | if depth == 32 { 8 } else { 0 };
                    let alpha = if depth == 32 { 128 } else { 255 };
                    if rle {
                        bytes.push(1); // A raw packet within an RLE image.
                    }
                    let pixels = if origin & 0x10 == 0 {
                        [[0, 0, 255, alpha], [0, 255, 0, alpha]]
                    } else {
                        [[0, 255, 0, alpha], [0, 0, 255, alpha]]
                    };
                    for pixel in pixels {
                        bytes.extend_from_slice(&pixel[..usize::from(depth / 8)]);
                    }
                    let decoded = decode_tga(&bytes).unwrap();
                    assert_eq!(decoded.to_rgba8().get_pixel(0, 0).0, [255, 0, 0, alpha]);
                    assert_eq!(decoded.to_rgba8().get_pixel(1, 0).0, [0, 255, 0, alpha]);
                    let directory = tempfile::tempdir().unwrap();
                    write_covers(&decoded, &decoded, directory.path()).unwrap();
                    for (name, size) in [("cover.png", (300, 300)), ("cover-wide.png", (620, 300))]
                    {
                        let output = image::open(directory.path().join(name)).unwrap();
                        assert_eq!((output.width(), output.height()), size);
                    }
                    if rle {
                        bytes.truncate(18);
                        bytes.push(0x81); // A repeated-pixel packet.
                        bytes.extend_from_slice(&[255, 0, 0, 128][..usize::from(depth / 8)]);
                        let repeated = decode_tga(&bytes).unwrap().to_rgba8();
                        assert_eq!(repeated.get_pixel(0, 0).0, [0, 0, 255, alpha]);
                        assert_eq!(repeated.get_pixel(1, 0).0, [0, 0, 255, alpha]);
                    }
                }
            }
        }
        assert!(decode_tga(&[0; 12]).is_err());
    }
}
