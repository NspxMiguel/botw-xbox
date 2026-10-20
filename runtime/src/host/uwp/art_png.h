// SPDX-License-Identifier: MIT
#pragma once
#include "art.h"
#include <wincodec.h>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "windowscodecs.lib")
#endif
namespace uwp {
// WIC is used only for an optional player-provided PNG; dump artwork uses our TGA decoder.
inline std::optional<ArtImage> decode_art_png(std::span<const uint8_t> bytes) {
    using Microsoft::WRL::ComPtr;
    if (bytes.empty() || bytes.size() > art_byte_limit)
        return std::nullopt;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return std::nullopt;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)))
        return std::nullopt;
    // WIC's API is non-const; retain our own writable bytes for the lifetime of the decoder.
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    if (FAILED(stream->InitializeFromMemory(input.data(), DWORD(input.size()))))
        return std::nullopt;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder)))
        return std::nullopt;
    GUID format{};
    if (FAILED(decoder->GetContainerFormat(&format)) || !IsEqualGUID(format, GUID_ContainerFormatPng))
        return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return std::nullopt;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        uint64_t(width) * height > art_byte_limit / 4)
        return std::nullopt;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    ArtImage image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    if (FAILED(converter->CopyPixels(nullptr, width * 4, UINT(image.rgba.size()), image.rgba.data())))
        return std::nullopt;
    return image;
}
} // namespace uwp
