/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ArrayPixelBuffer.h"
#include <JavaScriptCore/ArrayBufferView.h>
#include <JavaScriptCore/TypedArrayAdaptersForwardDeclarations.h>
#include <JavaScriptCore/TypedArrayInlines.h>

namespace WebCore {

template <typename JSCTypedArrayView, PixelBuffer::Type pixelBufferType, auto... supportedPixelFormats>
class TypedArrayPixelBuffer final : public ArrayPixelBuffer {
public:
    static Ref<TypedArrayPixelBuffer> create(const PixelBufferFormat&, const IntSize&, JSCTypedArrayView&);
    static std::optional<Ref<TypedArrayPixelBuffer>> create(const PixelBufferFormat&, const IntSize&, std::span<const uint8_t> data);

    static RefPtr<TypedArrayPixelBuffer> tryCreate(const PixelBufferFormat&, const IntSize&);
    static RefPtr<TypedArrayPixelBuffer> tryCreate(const PixelBufferFormat&, const IntSize&, Ref<JSC::ArrayBuffer>&&);

    JSCTypedArrayView& data() const LIFETIME_BOUND { return uncheckedDowncast<JSCTypedArrayView>(ArrayPixelBuffer::data()); }
    Ref<JSCTypedArrayView> protectedData() const { return data(); }
    Ref<JSCTypedArrayView> takeData() && { return adoptRef(uncheckedDowncast<JSCTypedArrayView>(WTFMove(*this).ArrayPixelBuffer::takeData().leakRef())); }

    Type type() const override { return pixelBufferType; }
    RefPtr<PixelBuffer> createScratchPixelBuffer(const IntSize&) const override;

private:
    TypedArrayPixelBuffer(const PixelBufferFormat&, const IntSize&, Ref<JSCTypedArrayView>&&);
};

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
Ref<TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>> TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::create(const PixelBufferFormat& format, const IntSize& size, JSCTypedArrayView& data)
{
    ASSERT(((format.pixelFormat == supportedPixelFormats) || ...));
    return adoptRef(*new TypedArrayPixelBuffer(format, size, { data }));
}

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
std::optional<Ref<TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>>> TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::create(const PixelBufferFormat& format, const IntSize& size, std::span<const uint8_t> data)
{
    if (!((format.pixelFormat == supportedPixelFormats) || ...)) {
        ASSERT_NOT_REACHED();
        return std::nullopt;
    }

    auto computedBufferSize = PixelBuffer::computeBufferSize(format.pixelFormat, size);
    if (computedBufferSize.hasOverflowed()) {
        ASSERT_NOT_REACHED();
        return std::nullopt;
    }

    if (data.size_bytes() != computedBufferSize.value()) {
        ASSERT_NOT_REACHED();
        return std::nullopt;
    }

    auto buffer = JSCTypedArrayView::tryCreate(data.data(), data.size_bytes());
    if (!buffer) {
        ASSERT_NOT_REACHED();
        return std::nullopt;
    }

    return TypedArrayPixelBuffer::create(format, size, buffer.releaseNonNull());
}

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
RefPtr<TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>> TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::tryCreate(const PixelBufferFormat& format, const IntSize& size)
{
    ASSERT(supportedPixelFormat(format.pixelFormat));

    if (!((format.pixelFormat == supportedPixelFormats) || ...)) {
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    auto bufferSize = computeBufferSize(format.pixelFormat, size);
    if (bufferSize.hasOverflowed())
        return nullptr;

    auto data = JSCTypedArrayView::tryCreateUninitialized(bufferSize);
    if (!data)
        return nullptr;

    return create(format, size, data.releaseNonNull());
}

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
RefPtr<TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>> TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::tryCreate(const PixelBufferFormat& format, const IntSize& size, Ref<JSC::ArrayBuffer>&& arrayBuffer)
{
    ASSERT(supportedPixelFormat(format.pixelFormat));

    if (!((format.pixelFormat == supportedPixelFormats) || ...)) {
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    auto bufferSize = computeBufferSize(format.pixelFormat, size);
    if (bufferSize.hasOverflowed())
        return nullptr;
    if (bufferSize != arrayBuffer->byteLength())
        return nullptr;

    Ref data = JSCTypedArrayView::create(WTFMove(arrayBuffer));
    return create(format, size, WTFMove(data));
}

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::TypedArrayPixelBuffer(const PixelBufferFormat& format, const IntSize& size, Ref<JSCTypedArrayView>&& data)
    : ArrayPixelBuffer(format, size, WTFMove(data))
{ }

template <typename JSCTypedArrayView, PixelBuffer::Type type, auto... supportedPixelFormats>
RefPtr<PixelBuffer> TypedArrayPixelBuffer<JSCTypedArrayView, type, supportedPixelFormats...>::createScratchPixelBuffer(const IntSize& size) const
{
    return TypedArrayPixelBuffer::tryCreate(format(), size);
}

} // namespace WebCore
