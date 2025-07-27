/*
 * Copyright (C) 2014-2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#pragma once

#include <wtf/StdLibExtras.h>

namespace WTF {

// Single Latin-1 character. Complements char8_t for UTF-8, char16_t for UTF-16, char32_t for UTF-32.
struct LChar {
    uint8_t value;

    LChar() = default;
    constexpr LChar(std::integral auto value) : value { static_cast<uint8_t>(value) } { }
    constexpr operator std::integral auto() const { return value; }
    constexpr bool operator !() const { return !value; }
    constexpr bool operator ==(const LChar&) const = default;
    constexpr auto operator <=>(const LChar&) const = default;
};

constexpr bool operator == (LChar a, std::integral auto b)
{
    return a.value == b;
}

constexpr auto operator <=> (LChar a, std::integral auto b)
{
    return a.value <=> b;
}

constexpr LChar operator + (LChar a, std::integral auto b)
{
    return a.value + b;
}

constexpr LChar operator - (LChar a, std::integral auto b)
{
    return a.value - b;
}

constexpr LChar operator & (LChar a, std::integral auto b)
{
    return a.value & b;
}

constexpr LChar operator | (LChar a, std::integral auto b)
{
    return a.value | b;
}

constexpr LChar operator % (LChar a, std::integral auto b)
{
    return a.value % b;
}

constexpr LChar& operator += (LChar& a, std::integral auto b)
{
    a.value += b;
    return a;
}

constexpr LChar& operator -= (LChar& a, std::integral auto b)
{
    a.value -= b;
    return a;
}

constexpr LChar& operator &= (LChar& a, std::integral auto b)
{
    a.value &= b;
    return a;
}

constexpr LChar& operator |= (LChar& a, std::integral auto b)
{
    a.value |= b;
    return a;
}

constexpr LChar& operator %= (LChar& a, std::integral auto b)
{
    a.value %= b;
    return a;
}

template<typename CharacterType>
concept IsStringStorageCharacter = std::same_as<CharacterType, LChar> || std::same_as<CharacterType, char16_t>;

}

using WTF::LChar;
