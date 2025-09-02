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

#import <CoreFoundation/CFBase.h>
#import <objc/runtime.h>
#import <wtf/OSObjectPtr.h>
#import <wtf/cf/TypeCastsCF.h>

// To add support for a new OSObject type:
// 1. Import header that defines OSObject type.
// 2. Add type to an existing WTF_OS_OBJECT_*_TYPES(M) macro, or create a new one.
//    a. If a new macro is created, add macro to WTF_OS_OBJECT_TYPES(M).
//    b. If a new macro is created, create OSObjectTypeCastTraits declarations.

#import <Network/Network.h>
#import <dispatch/dispatch.h>

#define WTF_OS_OBJECT_DISPATCH_TYPES(M) \
    M(dispatch_group) \
    M(dispatch_object) \
    M(dispatch_queue) \
    M(dispatch_queue_global) \
    M(dispatch_source)

WTF_EXTERN_C_BEGIN
#define WTF_DECLARE_OS_OBJECT_DISPATCH_BASE_STRUCT(TypeName) \
struct TypeName##_s;
WTF_OS_OBJECT_DISPATCH_TYPES(WTF_DECLARE_OS_OBJECT_DISPATCH_BASE_STRUCT)
#undef WTF_DECLARE_OS_OBJECT_DISPATCH_BASE_STRUCT
WTF_EXTERN_C_END

#define WTF_OS_OBJECT_NETWORK_TYPES(M) \
    M(nw_endpoint) \
    M(nw_resolution_report) \
    M(nw_path)

WTF_EXTERN_C_BEGIN
#define WTF_DECLARE_OS_OBJECT_NETWORK_BASE_STRUCT(TypeName) \
struct TypeName;
WTF_OS_OBJECT_NETWORK_TYPES(WTF_DECLARE_OS_OBJECT_NETWORK_BASE_STRUCT)
#undef WTF_DECLARE_OS_OBJECT_NETWORK_BASE_STRUCT
WTF_EXTERN_C_END

#define WTF_OS_OBJECT_TYPES(M) \
    WTF_OS_OBJECT_DISPATCH_TYPES(M) \
    WTF_OS_OBJECT_NETWORK_TYPES(M)

// Because ARC enablement is a compile-time choice, and we compile this header
// both ways, we need a separate copy of our code when ARC is enabled.
#if __has_feature(objc_arc)
#define dynamicOSObjectCast dynamicOSObjectCastARC
#define osObjectCast osObjectCastARC
#endif

namespace WTF {

template<typename> struct OSObjectTypeCastTraits;
#define WTF_DECLARE_OS_OBJECT_TYPE_CAST_TRAITS_INTERNAL(TypeName, Suffix) \
template<> struct OSObjectTypeCastTraits<TypeName##_t> { \
    using BaseType = struct TypeName##Suffix*; \
};

#define WTF_DECLARE_OS_OBJECT_DISPATCH_TYPE_CAST_TRAITS(TypeName) \
WTF_DECLARE_OS_OBJECT_TYPE_CAST_TRAITS_INTERNAL(TypeName, _s)
WTF_OS_OBJECT_DISPATCH_TYPES(WTF_DECLARE_OS_OBJECT_DISPATCH_TYPE_CAST_TRAITS)
#undef WTF_DECLARE_OS_OBJECT_DISPATCH_TYPE_CAST_TRAITS

#define WTF_DECLARE_OS_OBJECT_NETWORK_TYPE_CAST_TRAITS(TypeName) \
WTF_DECLARE_OS_OBJECT_TYPE_CAST_TRAITS_INTERNAL(TypeName, )
WTF_OS_OBJECT_NETWORK_TYPES(WTF_DECLARE_OS_OBJECT_NETWORK_TYPE_CAST_TRAITS)
#undef WTF_DECLARE_OS_OBJECT_NETWORK_TYPE_CAST_TRAITS

#undef WTF_DECLARE_OS_OBJECT_TYPE_CAST_TRAITS_INTERNAL

// Must define this for isOSObject<T>() when not building with Objective-C.
#ifndef OS_OBJECT_CLASS
#define OS_OBJECT_CLASS(name) OS_##name
#endif

template<typename T> bool isOSObject(CFTypeRef);

#define WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS(TypeName) WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS_INTERNAL(TypeName##_t, STRINGIZE_VALUE_OF(OS_OBJECT_CLASS(TypeName)))
#define WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS_INTERNAL(TypeName, ProtocolString) \
template<> inline bool isOSObject<OSObjectTypeCastTraits<TypeName>::BaseType>(CFTypeRef object) \
{ \
    Class cls = object_getClass(bridge_id_cast(object)); \
    return class_conformsToProtocol(cls, objc_getProtocol(ProtocolString)); \
} \

WTF_OS_OBJECT_TYPES(WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS)
#undef WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS_INTERNAL
#undef WTF_IMPLEMENT_IS_OS_OBJECT_FUNCTIONS

#ifdef __OBJC__

template<typename T> inline bool isOSObject(id object)
{
    return isOSObject<T>(bridgeCFCast(object));
}

template<typename T> inline T osObjectCast(id object)
{
    if (!object)
        return nullptr;

    RELEASE_ASSERT(isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object));

    return static_cast<T>(object);
}

// The dynamicOSObjectCast<T>() methods that have OSObjectPtr<U> arguments use different template types
// in Objective-C++ vs. C++, so these Objective-C method definitions do not create an ODR violation
// with the C++ method definitions below.

template<typename T, typename U, typename = std::enable_if_t<!std::is_same_v<U, T>>> inline OSObjectPtr<T> dynamicOSObjectCast(OSObjectPtr<U>&& object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object.get()))
        return nullptr;

    return adoptOSObject(static_cast<T>(object.leakRef()));
}

template<typename T, typename U, typename = std::enable_if_t<!std::is_same_v<U, T>>> inline OSObjectPtr<T> dynamicOSObjectCast(const OSObjectPtr<U>& object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object.get()))
        return nullptr;

    return static_cast<T*>(object.get());
}

template<typename T> inline T dynamicOSObjectCast(id object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object))
        return nullptr;

    return reinterpret_cast<T>(object);
}

#endif // defined(__OBJC__)

template<typename T> inline T osObjectCast(CFTypeRef object)
{
    if (!object)
        return nullptr;

    RELEASE_ASSERT(isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object));

    return static_cast<T>(const_cast<CF_BRIDGED_TYPE(id) void*>(object));
}

template<typename T> inline T dynamicOSObjectCast(CFTypeRef object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object))
        return nullptr;

    return static_cast<T>(const_cast<CF_BRIDGED_TYPE(id) void*>(object));
}

#ifndef __OBJC__

template<typename T, typename U, typename = std::enable_if_t<!std::is_same_v<U, T>>> inline OSObjectPtr<T> dynamicOSObjectCast(OSObjectPtr<U>&& object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object.get()))
        return nullptr;

    return adoptOSObject(static_cast<T>(object.leakRef()));
}

template<typename T, typename U, typename = std::enable_if_t<!std::is_same_v<U, T>>> inline OSObjectPtr<T> dynamicOSObjectCast(const OSObjectPtr<U>& object)
{
    if (!object)
        return nullptr;

    if (!isOSObject<typename OSObjectTypeCastTraits<T>::BaseType>(object.get()))
        return nullptr;

    return static_cast<T*>(object.get());
}

#endif // !defined(__OBJC__)

} // namespace WTF

using WTF::dynamicOSObjectCast;
using WTF::osObjectCast;
