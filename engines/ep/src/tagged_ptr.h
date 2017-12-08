/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <memory>

#if !__x86_64__ && !_M_X64
#error "TaggedPtr is x64 specific code.  Not tested on other architectures"
#endif

/*
 * This class provides a tagged pointer, which means it is a pointer however
 * the top 16 bits (the tag) can be used to hold user data.  This works
 * because on x86-64 it only addresses using the bottom 48-bits (the top 16-bits
 * are not used).
 *
 * A unsigned 16 bit value can be stored in the pointer using the setTag method.
 * The value can be retrieved using the setTag method.
 *
 * To avoid an address error being raised it is important to mask out the top
 * 16-bits when using the pointer.  This is achieved by using the getOBj method.
 * setObj is used to set the pointer value, without affecting the tag.
 */

template <typename T>
class TaggedPtr {
public:
    typedef T element_type;

    // Need to define all methods which unique_ptr expects from a pointer type
    TaggedPtr() : raw(0) {
    }

    TaggedPtr(T* obj) : TaggedPtr() {
        set(obj);
    }

    TaggedPtr(T* obj, uint16_t tag) : TaggedPtr() {
        set(obj);
        setTag(tag);
    }

    bool operator!=(const T* other) const {
        return get() != other;
    }

    bool operator==(const T* other) const {
        return !operator!=(other);
    }

    operator bool() const {
        return raw != 0;
    }

    // Implement pointer operator to allow existing code to transparently
    // access the underlying object (ignoring the tag).
    T* operator->() const noexcept {
        return get();
    }

    // Required by MS Compiler
    T& operator*() const noexcept {
        return *get();
    }

    void setTag(uint16_t tag) {
        raw = extractPointer(raw);
        raw |= (uintptr_t(tag) << 48ull);
    }

    uint16_t getTag() const {
        return (extractTag(raw) >> 48ull);
    }

    void set(const T* obj) {
        raw = extractTag(raw);
        raw |= extractPointer(reinterpret_cast<uintptr_t>(obj));
    }

    T* get() const {
        return reinterpret_cast<T*>(extractPointer(raw));
    }

    // Required by MS Compiler
    static T* pointer_to(element_type& r) {
        return std::addressof(r);
    }

private:
    uintptr_t extractPointer(uintptr_t ptr) const {
        return (ptr & 0x0000ffffffffffffull);
    }

    uintptr_t extractTag(uintptr_t ptr) const {
        return (ptr & 0xffff000000000000ull);
    }

    // Tag held in top 16 bits.
    uintptr_t raw;
};