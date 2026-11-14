// java.lang, java.io and java.util: the parts of CLDC 1.0 the game calls.

#include "natives.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace vm {

namespace {
struct StringBufferData : Native {
    std::u16string text;
};

struct IntegerData : Native {
    std::int32_t value = 0;
};

struct VectorData : Native {
    std::vector<Object*> items;
    void trace(Marker& out) override { out.insert(out.end(), items.begin(), items.end()); }
};

struct HashtableData : Native {
    std::vector<std::pair<Object*, Object*>> entries;
    void trace(Marker& out) override {
        for (auto& [key, value] : entries) {
            out.push_back(key);
            out.push_back(value);
        }
    }
};

struct EnumerationData : Native {
    std::vector<Object*> items;
    std::size_t at = 0;
    void trace(Marker& out) override { out.insert(out.end(), items.begin(), items.end()); }
};

struct RandomData : Native {
    std::int64_t seed = 0;
    void set(std::int64_t value) { seed = (value ^ 0x5DEECE66DLL) & ((1LL << 48) - 1); }
    std::int32_t next(int bits) {
        seed = static_cast<std::int64_t>((static_cast<std::uint64_t>(seed) * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1));
        return static_cast<std::int32_t>(seed >> (48 - bits));
    }
};

struct ThreadData : Native {
    Object* runnable = nullptr;
    void trace(Marker& out) override { out.push_back(runnable); }
};

// Reads from a Java byte array or from bytes owned by the stream.
struct ByteInput : Native {
    Array* array = nullptr;
    Bytes own;
    std::size_t at = 0;
    std::size_t size() const { return array ? static_cast<std::size_t>(array->length) : own.size(); }
    int read() {
        if (at >= size()) return -1;
        return array ? static_cast<std::uint8_t>(array->b()[at++]) : own[at++];
    }
    void trace(Marker& out) override { out.push_back(array); }
};

struct WrappedStream : Native {
    Object* inner = nullptr;
    void trace(Marker& out) override { out.push_back(inner); }
};

struct ByteOutput : Native {
    Bytes data;
};

std::int64_t now() {
    return gameMillis();
}

Instance* instance(Object* object) {
    return static_cast<Instance*>(object);
}

template <class T> T& attach(Object* object) {
    auto data = std::make_unique<T>();
    auto& result = *data;
    instance(object)->native = std::move(data);
    return result;
}

Array* arrayArg(VM& vm, Object* object) {
    if (!object) vm.throwNew("java/lang/NullPointerException");
    return static_cast<Array*>(object);
}

void checkRange(VM& vm, Array* array, int offset, int length) {
    if (offset < 0 || length < 0 || offset > array->length - length) {
        vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
    }
}

int readByte(VM& vm, Object* stream) {
    if (!stream) vm.throwNew("java/lang/NullPointerException");
    auto* native = instance(stream)->native.get();
    if (auto* bytes = dynamic_cast<ByteInput*>(native)) {
        return bytes->read();
    }
    if (auto* wrapped = dynamic_cast<WrappedStream*>(native)) {
        return readByte(vm, wrapped->inner);
    }
    vm.fatal("cannot read from a stream of class " + stream->cls->name);
}

int need(VM& vm, Object* stream) {
    const int value = readByte(vm, stream);
    if (value < 0) vm.throwNew("java/io/EOFException");
    return value;
}

std::int64_t readBig(VM& vm, Object* stream, int bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        value = value << 8 | static_cast<std::uint64_t>(need(vm, stream));
    }
    return static_cast<std::int64_t>(value);
}

int readInto(VM& vm, Object* stream, Array* target, int offset, int length, bool fully) {
    checkRange(vm, target, offset, length);
    int count = 0;
    while (count < length) {
        const int value = readByte(vm, stream);
        if (value < 0) {
            if (fully) vm.throwNew("java/io/EOFException");
            break;
        }
        target->b()[offset + count++] = static_cast<std::int8_t>(value);
    }
    return count == 0 && length > 0 ? -1 : count;
}

void writeByte(VM& vm, Object* stream, int value) {
    if (!stream) vm.throwNew("java/lang/NullPointerException");
    auto* native = instance(stream)->native.get();
    if (auto* bytes = dynamic_cast<ByteOutput*>(native)) {
        bytes->data.push_back(static_cast<std::uint8_t>(value));
    } else if (auto* wrapped = dynamic_cast<WrappedStream*>(native)) {
        writeByte(vm, wrapped->inner, value);
    } else {
        vm.fatal("cannot write to a stream of class " + stream->cls->name);
    }
}

void writeBig(VM& vm, Object* stream, std::int64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        writeByte(vm, stream, static_cast<int>(value >> (8 * i)) & 0xFF);
    }
}

bool sameKey(Object* left, Object* right) {
    if (left == right) return true;
    if (!left || !right || left->cls != right->cls || left->cls->elem) return false;
    auto* a = instance(left)->native.get();
    auto* b = instance(right)->native.get();
    if (auto* text = dynamic_cast<StringData*>(a)) {
        return text->text == static_cast<StringData*>(b)->text;
    }
    if (auto* number = dynamic_cast<IntegerData*>(a)) {
        return number->value == static_cast<IntegerData*>(b)->value;
    }
    return false;
}

std::int32_t hashOf(const std::u16string& text) {
    std::uint32_t hash = 0;
    for (const char16_t ch : text) {
        hash = hash * 31 + ch;
    }
    return static_cast<std::int32_t>(hash);
}

std::u16string textOf(VM& vm, Object* object) {
    if (!object) return u"null";
    if (object->cls->elem == 0) {
        auto* native = instance(object)->native.get();
        if (auto* text = dynamic_cast<StringData*>(native)) return text->text;
        if (auto* buffer = dynamic_cast<StringBufferData*>(native)) return buffer->text;
        if (auto* number = dynamic_cast<IntegerData*>(native)) return widen(std::to_string(number->value));
        if (dynamic_cast<ThrowableData*>(native)) return widen(vm.describe(object));
    }
    return widen(object->cls->name);
}

std::int64_t parseNumber(VM& vm, Object* string, int radix, bool isLong) {
    const auto text = narrow(vm.str(string));
    std::size_t i = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) i = 1;
    if (i >= text.size()) vm.throwNew("java/lang/NumberFormatException", text);
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        const char ch = text[i];
        const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                        : ch >= 'a' && ch <= 'z' ? ch - 'a' + 10
                        : ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10 : 99;
        if (digit >= radix) vm.throwNew("java/lang/NumberFormatException", text);
        value = value * radix + digit;
        if (!isLong && value > 2147483648LL) vm.throwNew("java/lang/NumberFormatException", text);
    }
    value = negative ? -value : value;
    if (!isLong && value > 2147483647LL) vm.throwNew("java/lang/NumberFormatException", text);
    return value;
}

Object* newEnumeration(VM& vm, std::vector<Object*> items) {
    auto data = std::make_unique<EnumerationData>();
    data->items = std::move(items);
    return vm.make("java/util/Enumeration$Items", std::move(data));
}

void defineLang(VM& vm) {
    const char* object = "java/lang/Object";
    vm.def(object, "<init>:()V", [](VM&, Value*) { return Value(); });
    vm.def(object, "getClass:()Ljava/lang/Class;", [](VM& vm, Value* a) { return A(vm.mirror(a[0].a->cls)); });
    vm.def(object, "hashCode:()I", [](VM&, Value* a) {
        return I(static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(a[0].a) >> 4));
    });
    vm.def(object, "equals:(Ljava/lang/Object;)Z", [](VM&, Value* a) { return I(a[0].a == a[1].a); });
    vm.def(object, "toString:()Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(textOf(vm, a[0].a))); });
    vm.def(object, "wait:()V", [](VM& vm, Value* a) { vm.waitOn(a[0].a); return Value(); });
    vm.def(object, "notify:()V", [](VM& vm, Value*) { vm.notifyAll(); return Value(); });
    vm.def(object, "notifyAll:()V", [](VM& vm, Value*) { vm.notifyAll(); return Value(); });

    const char* cls = "java/lang/Class";
    vm.def(cls, "getResourceAsStream:(Ljava/lang/String;)Ljava/io/InputStream;", [](VM& vm, Value* a) {
        return A(openResource(vm, narrow(vm.str(a[1].a))));
    });
    vm.def(cls, "getName:()Ljava/lang/String;", [](VM& vm, Value* a) {
        auto name = vm.payload<ClassData>(a[0].a).cls->name;
        std::replace(name.begin(), name.end(), '/', '.');
        return A(vm.newString(name));
    });

    // Throwables
    const char* throwable = "java/lang/Throwable";
    vm.def(throwable, "<init>:()V", [](VM&, Value* a) {
        if (!instance(a[0].a)->native) attach<ThrowableData>(a[0].a);
        return Value();
    });
    vm.def(throwable, "<init>:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        attach<ThrowableData>(a[0].a).message = a[1].a ? vm.str(a[1].a) : u"";
        return Value();
    });
    vm.def(throwable, "getMessage:()Ljava/lang/String;", [](VM& vm, Value* a) {
        auto* data = dynamic_cast<ThrowableData*>(instance(a[0].a)->native.get());
        return A(data && !data->message.empty() ? vm.newString(data->message) : nullptr);
    });
    vm.def(throwable, "toString:()Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(vm.describe(a[0].a))); });
    vm.def(throwable, "printStackTrace:()V", [](VM& vm, Value* a) {
        if (verbose) std::fprintf(stderr, "[game] %s\n%s", vm.describe(a[0].a).c_str(), vm.trace().c_str());
        return Value();
    });
    vm.defineNative("java/lang/Exception", throwable);
    vm.defineNative("java/lang/Error", throwable);
    vm.defineNative("java/lang/OutOfMemoryError", "java/lang/Error");
    vm.defineNative("java/lang/StackOverflowError", "java/lang/Error");
    const char* runtime = "java/lang/RuntimeException";
    vm.defineNative(runtime, "java/lang/Exception");
    for (const char* name : {"java/lang/NullPointerException", "java/lang/ArithmeticException",
                             "java/lang/ClassCastException", "java/lang/NegativeArraySizeException",
                             "java/lang/ArrayStoreException", "java/lang/IllegalArgumentException",
                             "java/lang/IllegalStateException", "java/lang/IllegalMonitorStateException",
                             "java/lang/IndexOutOfBoundsException", "java/lang/SecurityException"}) {
        vm.defineNative(name, runtime);
    }
    vm.defineNative("java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
    vm.defineNative("java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException");
    vm.defineNative("java/lang/NumberFormatException", "java/lang/IllegalArgumentException");
    vm.defineNative("java/lang/InterruptedException", "java/lang/Exception");
    vm.defineNative("java/lang/ClassNotFoundException", "java/lang/Exception");
    vm.defineNative("java/io/IOException", "java/lang/Exception");
    vm.defineNative("java/io/EOFException", "java/io/IOException");
    vm.defineNative("java/io/UTFDataFormatException", "java/io/IOException");

    // String
    const char* string = "java/lang/String";
    vm.def(string, "<init>:()V", [](VM&, Value* a) { attach<StringData>(a[0].a); return Value(); });
    vm.def(string, "<init>:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        attach<StringData>(a[0].a).text = vm.str(a[1].a);
        return Value();
    });
    vm.def(string, "<init>:([B)V", [](VM& vm, Value* a) {
        auto* bytes = arrayArg(vm, a[1].a);
        auto& text = attach<StringData>(a[0].a).text;
        for (int i = 0; i < bytes->length; ++i) text += static_cast<char16_t>(static_cast<std::uint8_t>(bytes->b()[i]));
        return Value();
    });
    vm.def(string, "<init>:([BII)V", [](VM& vm, Value* a) {
        auto* bytes = arrayArg(vm, a[1].a);
        checkRange(vm, bytes, a[2].i, a[3].i);
        auto& text = attach<StringData>(a[0].a).text;
        for (int i = 0; i < a[3].i; ++i) text += static_cast<char16_t>(static_cast<std::uint8_t>(bytes->b()[a[2].i + i]));
        return Value();
    });
    vm.def(string, "<init>:([C)V", [](VM& vm, Value* a) {
        auto* chars = arrayArg(vm, a[1].a);
        attach<StringData>(a[0].a).text.assign(reinterpret_cast<char16_t*>(chars->c()), chars->length);
        return Value();
    });
    vm.def(string, "<init>:([CII)V", [](VM& vm, Value* a) {
        auto* chars = arrayArg(vm, a[1].a);
        checkRange(vm, chars, a[2].i, a[3].i);
        attach<StringData>(a[0].a).text.assign(reinterpret_cast<char16_t*>(chars->c()) + a[2].i, a[3].i);
        return Value();
    });
    vm.def(string, "length:()I", [](VM& vm, Value* a) { return I(static_cast<int>(vm.str(a[0].a).size())); });
    vm.def(string, "charAt:(I)C", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        if (static_cast<std::uint32_t>(a[1].i) >= text.size()) vm.throwNew("java/lang/StringIndexOutOfBoundsException");
        return I(text[a[1].i]);
    });
    vm.def(string, "equals:(Ljava/lang/Object;)Z", [](VM& vm, Value* a) {
        return I(a[1].a && a[1].a->cls == a[0].a->cls && vm.str(a[0].a) == vm.str(a[1].a));
    });
    vm.def(string, "hashCode:()I", [](VM& vm, Value* a) { return I(hashOf(vm.str(a[0].a))); });
    vm.def(string, "toString:()Ljava/lang/String;", [](VM&, Value* a) { return a[0]; });
    vm.def(string, "compareTo:(Ljava/lang/String;)I", [](VM& vm, Value* a) {
        const auto& left = vm.str(a[0].a);
        const auto& right = vm.str(a[1].a);
        const auto shared = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < shared; ++i) {
            if (left[i] != right[i]) return I(left[i] - right[i]);
        }
        return I(static_cast<int>(left.size()) - static_cast<int>(right.size()));
    });
    vm.def(string, "startsWith:(Ljava/lang/String;)Z", [](VM& vm, Value* a) {
        return I(vm.str(a[0].a).compare(0, vm.str(a[1].a).size(), vm.str(a[1].a)) == 0);
    });
    vm.def(string, "startsWith:(Ljava/lang/String;I)Z", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        const auto& prefix = vm.str(a[1].a);
        return I(a[2].i >= 0 && static_cast<std::size_t>(a[2].i) <= text.size() && text.compare(a[2].i, prefix.size(), prefix) == 0);
    });
    vm.def(string, "endsWith:(Ljava/lang/String;)Z", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        const auto& suffix = vm.str(a[1].a);
        return I(text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0);
    });
    vm.def(string, "indexOf:(I)I", [](VM& vm, Value* a) {
        return I(static_cast<int>(vm.str(a[0].a).find(static_cast<char16_t>(a[1].i))));
    });
    vm.def(string, "indexOf:(II)I", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        const auto from = static_cast<std::size_t>(std::max(0, a[2].i));
        return I(from >= text.size() ? -1 : static_cast<int>(text.find(static_cast<char16_t>(a[1].i), from)));
    });
    vm.def(string, "indexOf:(Ljava/lang/String;)I", [](VM& vm, Value* a) {
        return I(static_cast<int>(vm.str(a[0].a).find(vm.str(a[1].a))));
    });
    vm.def(string, "indexOf:(Ljava/lang/String;I)I", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        const auto from = static_cast<std::size_t>(std::max(0, a[2].i));
        return I(from > text.size() ? -1 : static_cast<int>(text.find(vm.str(a[1].a), from)));
    });
    vm.def(string, "lastIndexOf:(I)I", [](VM& vm, Value* a) {
        return I(static_cast<int>(vm.str(a[0].a).rfind(static_cast<char16_t>(a[1].i))));
    });
    vm.def(string, "substring:(I)Ljava/lang/String;", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        if (a[1].i < 0 || static_cast<std::size_t>(a[1].i) > text.size()) vm.throwNew("java/lang/StringIndexOutOfBoundsException");
        return A(vm.newString(text.substr(a[1].i)));
    });
    vm.def(string, "substring:(II)Ljava/lang/String;", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        if (a[1].i < 0 || a[2].i < a[1].i || static_cast<std::size_t>(a[2].i) > text.size()) {
            vm.throwNew("java/lang/StringIndexOutOfBoundsException");
        }
        return A(vm.newString(text.substr(a[1].i, a[2].i - a[1].i)));
    });
    vm.def(string, "trim:()Ljava/lang/String;", [](VM& vm, Value* a) {
        const auto& text = vm.str(a[0].a);
        std::size_t first = 0, last = text.size();
        while (first < last && text[first] <= u' ') ++first;
        while (last > first && text[last - 1] <= u' ') --last;
        return A(vm.newString(text.substr(first, last - first)));
    });
    vm.def(string, "replace:(CC)Ljava/lang/String;", [](VM& vm, Value* a) {
        auto text = vm.str(a[0].a);
        std::replace(text.begin(), text.end(), static_cast<char16_t>(a[1].i), static_cast<char16_t>(a[2].i));
        return A(vm.newString(std::move(text)));
    });
    vm.def(string, "getBytes:()[B", [](VM& vm, Value* a) {
        const auto text = vm.str(a[0].a);
        auto* bytes = vm.newArray('B', static_cast<int>(text.size()));
        for (std::size_t i = 0; i < text.size(); ++i) bytes->b()[i] = static_cast<std::int8_t>(text[i] < 256 ? text[i] : '?');
        return A(bytes);
    });
    vm.def(string, "toCharArray:()[C", [](VM& vm, Value* a) {
        const auto text = vm.str(a[0].a);
        auto* chars = vm.newArray('C', static_cast<int>(text.size()));
        std::memcpy(chars->c(), text.data(), 2 * text.size());
        return A(chars);
    });
    vm.def(string, "toLowerCase:()Ljava/lang/String;", [](VM& vm, Value* a) {
        auto text = vm.str(a[0].a);
        for (auto& ch : text) if (ch >= u'A' && ch <= u'Z') ch += 32;
        return A(vm.newString(std::move(text)));
    });
    vm.def(string, "toUpperCase:()Ljava/lang/String;", [](VM& vm, Value* a) {
        auto text = vm.str(a[0].a);
        for (auto& ch : text) if (ch >= u'a' && ch <= u'z') ch -= 32;
        return A(vm.newString(std::move(text)));
    });
    vm.defStatic(string, "valueOf:(I)Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(std::to_string(a[0].i))); });
    vm.defStatic(string, "valueOf:(C)Ljava/lang/String;", [](VM& vm, Value* a) {
        return A(vm.newString(std::u16string(1, static_cast<char16_t>(a[0].i))));
    });
    vm.defStatic(string, "valueOf:(Ljava/lang/Object;)Ljava/lang/String;", [](VM& vm, Value* a) {
        return A(vm.newString(textOf(vm, a[0].a)));
    });

    // StringBuffer
    const char* buffer = "java/lang/StringBuffer";
    vm.def(buffer, "<init>:()V", [](VM&, Value* a) { attach<StringBufferData>(a[0].a); return Value(); });
    vm.def(buffer, "<init>:(I)V", [](VM&, Value* a) { attach<StringBufferData>(a[0].a); return Value(); });
    vm.def(buffer, "<init>:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        attach<StringBufferData>(a[0].a).text = vm.str(a[1].a);
        return Value();
    });
    vm.def(buffer, "append:(Ljava/lang/String;)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += a[1].a ? vm.str(a[1].a) : u"null";
        return a[0];
    });
    vm.def(buffer, "append:(Ljava/lang/Object;)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += textOf(vm, a[1].a);
        return a[0];
    });
    vm.def(buffer, "append:(C)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += static_cast<char16_t>(a[1].i);
        return a[0];
    });
    vm.def(buffer, "append:(I)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += widen(std::to_string(a[1].i));
        return a[0];
    });
    vm.def(buffer, "append:(J)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += widen(std::to_string(a[1].j));
        return a[0];
    });
    vm.def(buffer, "append:(Z)Ljava/lang/StringBuffer;", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text += a[1].i ? u"true" : u"false";
        return a[0];
    });
    vm.def(buffer, "length:()I", [](VM& vm, Value* a) {
        return I(static_cast<int>(vm.payload<StringBufferData>(a[0].a).text.size()));
    });
    vm.def(buffer, "setLength:(I)V", [](VM& vm, Value* a) {
        vm.payload<StringBufferData>(a[0].a).text.resize(std::max(0, a[1].i));
        return Value();
    });
    vm.def(buffer, "charAt:(I)C", [](VM& vm, Value* a) {
        const auto& text = vm.payload<StringBufferData>(a[0].a).text;
        if (static_cast<std::uint32_t>(a[1].i) >= text.size()) vm.throwNew("java/lang/StringIndexOutOfBoundsException");
        return I(text[a[1].i]);
    });
    vm.def(buffer, "toString:()Ljava/lang/String;", [](VM& vm, Value* a) {
        return A(vm.newString(vm.payload<StringBufferData>(a[0].a).text));
    });

    // Numbers
    const char* integer = "java/lang/Integer";
    vm.def(integer, "<init>:(I)V", [](VM&, Value* a) { attach<IntegerData>(a[0].a).value = a[1].i; return Value(); });
    vm.def(integer, "intValue:()I", [](VM& vm, Value* a) { return I(vm.payload<IntegerData>(a[0].a).value); });
    vm.def(integer, "hashCode:()I", [](VM& vm, Value* a) { return I(vm.payload<IntegerData>(a[0].a).value); });
    vm.def(integer, "equals:(Ljava/lang/Object;)Z", [](VM&, Value* a) { return I(sameKey(a[0].a, a[1].a)); });
    vm.def(integer, "toString:()Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(textOf(vm, a[0].a))); });
    vm.defStatic(integer, "parseInt:(Ljava/lang/String;)I", [](VM& vm, Value* a) {
        return I(static_cast<std::int32_t>(parseNumber(vm, a[0].a, 10, false)));
    });
    vm.defStatic(integer, "parseInt:(Ljava/lang/String;I)I", [](VM& vm, Value* a) {
        return I(static_cast<std::int32_t>(parseNumber(vm, a[0].a, a[1].i, false)));
    });
    vm.defStatic(integer, "toString:(I)Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(std::to_string(a[0].i))); });
    vm.defStatic("java/lang/Long", "parseLong:(Ljava/lang/String;)J", [](VM& vm, Value* a) {
        return J(parseNumber(vm, a[0].a, 10, true));
    });
    const char* math = "java/lang/Math";
    vm.defStatic(math, "abs:(I)I", [](VM&, Value* a) {
        return I(a[0].i < 0 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a[0].i)) : a[0].i);
    });
    vm.defStatic(math, "abs:(J)J", [](VM&, Value* a) {
        return J(a[0].j < 0 ? static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(a[0].j)) : a[0].j);
    });
    vm.defStatic(math, "max:(II)I", [](VM&, Value* a) { return I(std::max(a[0].i, a[1].i)); });
    vm.defStatic(math, "min:(II)I", [](VM&, Value* a) { return I(std::min(a[0].i, a[1].i)); });
    vm.defStatic(math, "max:(JJ)J", [](VM&, Value* a) { return J(std::max(a[0].j, a[2].j)); });
    vm.defStatic(math, "min:(JJ)J", [](VM&, Value* a) { return J(std::min(a[0].j, a[2].j)); });

    // System and threads
    const char* system = "java/lang/System";
    vm.defStatic(system, "currentTimeMillis:()J", [](VM&, Value*) { return J(now()); });
    vm.defStatic(system, "gc:()V", [](VM&, Value*) { return Value(); });
    vm.defStatic(system, "getProperty:(Ljava/lang/String;)Ljava/lang/String;", [](VM& vm, Value* a) {
        const auto key = narrow(vm.str(a[0].a));
        const char* value = key == "microedition.platform" ? "Nokia5800 XpressMusic"
                          : key == "microedition.locale" ? "en"
                          : key == "microedition.encoding" ? "ISO-8859-1"
                          : key == "microedition.configuration" ? "CLDC-1.0"
                          : key == "microedition.profiles" ? "MIDP-2.0" : nullptr;
        if (verbose) std::fprintf(stderr, "[runtime] System.getProperty(%s) -> %s\n", key.c_str(), value ? value : "null");
        return A(value ? vm.newString(std::string(value)) : nullptr);
    });
    vm.defStatic(system, "arraycopy:(Ljava/lang/Object;ILjava/lang/Object;II)V", [](VM& vm, Value* a) {
        auto* source = arrayArg(vm, a[0].a);
        auto* target = arrayArg(vm, a[2].a);
        if (source->cls->elem != target->cls->elem) vm.throwNew("java/lang/ArrayStoreException");
        checkRange(vm, source, a[1].i, a[4].i);
        checkRange(vm, target, a[3].i, a[4].i);
        const auto size = static_cast<std::size_t>(source->cls->elem == 'B' ? 1 : source->cls->elem == 'C' || source->cls->elem == 'S' ? 2
                                                  : source->cls->elem == 'I' ? 4 : 8);
        std::memmove(static_cast<char*>(target->data) + size * a[3].i,
                     static_cast<char*>(source->data) + size * a[1].i, size * a[4].i);
        return Value();
    });
    vm.def("java/io/PrintStream", "println:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        if (verbose) std::fprintf(stderr, "[game] %s\n", narrow(textOf(vm, a[1].a)).c_str());
        return Value();
    });
    vm.addStaticField(vm.findClass(system), "out", "Ljava/io/PrintStream;", A(vm.make("java/io/PrintStream", nullptr)));

    const char* thread = "java/lang/Thread";
    vm.defineNative("java/lang/Runnable");
    vm.def(thread, "<init>:()V", [](VM&, Value* a) { attach<ThreadData>(a[0].a); return Value(); });
    vm.def(thread, "<init>:(Ljava/lang/Runnable;)V", [](VM&, Value* a) {
        attach<ThreadData>(a[0].a).runnable = a[1].a;
        return Value();
    });
    vm.def(thread, "start:()V", [](VM& vm, Value* a) {
        auto* runnable = vm.payload<ThreadData>(a[0].a).runnable;
        vm.startThread(runnable ? runnable : a[0].a);
        return Value();
    });
    vm.def(thread, "setPriority:(I)V", [](VM&, Value*) { return Value(); });
    vm.def(thread, "isAlive:()Z", [](VM&, Value*) { return I(1); });
    vm.defStatic(thread, "yield:()V", [](VM& vm, Value*) { vm.yield(); return Value(); });
    vm.defStatic(thread, "sleep:(J)V", [](VM& vm, Value* a) {
        const auto duration = std::chrono::microseconds(static_cast<std::int64_t>(std::max<std::int64_t>(0, a[0].j) * 1000 / timeScale));
        vm.lock.unlock();
        std::this_thread::sleep_for(duration);
        vm.lock.lock();
        return Value();
    });
}

void defineIo(VM& vm) {
    const char* input = "java/io/InputStream";
    vm.def(input, "read:()I", [](VM& vm, Value* a) { return I(readByte(vm, a[0].a)); });
    vm.def(input, "read:([B)I", [](VM& vm, Value* a) {
        auto* target = arrayArg(vm, a[1].a);
        return I(readInto(vm, a[0].a, target, 0, target->length, false));
    });
    vm.def(input, "read:([BII)I", [](VM& vm, Value* a) {
        return I(readInto(vm, a[0].a, arrayArg(vm, a[1].a), a[2].i, a[3].i, false));
    });
    vm.def(input, "skip:(J)J", [](VM& vm, Value* a) {
        std::int64_t skipped = 0;
        while (skipped < a[1].j && readByte(vm, a[0].a) >= 0) ++skipped;
        return J(skipped);
    });
    vm.def(input, "available:()I", [](VM&, Value* a) {
        Object* stream = a[0].a;
        while (auto* wrapped = dynamic_cast<WrappedStream*>(instance(stream)->native.get())) stream = wrapped->inner;
        auto* bytes = dynamic_cast<ByteInput*>(instance(stream)->native.get());
        return I(bytes ? static_cast<int>(bytes->size() - bytes->at) : 0);
    });
    vm.def(input, "close:()V", [](VM&, Value*) { return Value(); });

    const char* byteInput = "java/io/ByteArrayInputStream";
    vm.defineNative(byteInput, input);
    vm.def(byteInput, "<init>:([B)V", [](VM& vm, Value* a) {
        attach<ByteInput>(a[0].a).array = arrayArg(vm, a[1].a);
        return Value();
    });

    const char* dataInput = "java/io/DataInputStream";
    vm.defineNative(dataInput, input);
    vm.def(dataInput, "<init>:(Ljava/io/InputStream;)V", [](VM&, Value* a) {
        attach<WrappedStream>(a[0].a).inner = a[1].a;
        return Value();
    });
    vm.def(dataInput, "readByte:()B", [](VM& vm, Value* a) { return I(static_cast<std::int8_t>(need(vm, a[0].a))); });
    vm.def(dataInput, "readUnsignedByte:()I", [](VM& vm, Value* a) { return I(need(vm, a[0].a)); });
    vm.def(dataInput, "readBoolean:()Z", [](VM& vm, Value* a) { return I(need(vm, a[0].a) != 0); });
    vm.def(dataInput, "readShort:()S", [](VM& vm, Value* a) { return I(static_cast<std::int16_t>(readBig(vm, a[0].a, 2))); });
    vm.def(dataInput, "readUnsignedShort:()I", [](VM& vm, Value* a) { return I(static_cast<std::int32_t>(readBig(vm, a[0].a, 2))); });
    vm.def(dataInput, "readChar:()C", [](VM& vm, Value* a) { return I(static_cast<std::int32_t>(readBig(vm, a[0].a, 2))); });
    vm.def(dataInput, "readInt:()I", [](VM& vm, Value* a) { return I(static_cast<std::int32_t>(readBig(vm, a[0].a, 4))); });
    vm.def(dataInput, "readLong:()J", [](VM& vm, Value* a) { return J(readBig(vm, a[0].a, 8)); });
    vm.def(dataInput, "readFully:([B)V", [](VM& vm, Value* a) {
        auto* target = arrayArg(vm, a[1].a);
        readInto(vm, a[0].a, target, 0, target->length, true);
        return Value();
    });
    vm.def(dataInput, "readFully:([BII)V", [](VM& vm, Value* a) {
        readInto(vm, a[0].a, arrayArg(vm, a[1].a), a[2].i, a[3].i, true);
        return Value();
    });
    vm.def(dataInput, "skipBytes:(I)I", [](VM& vm, Value* a) {
        int skipped = 0;
        while (skipped < a[1].i && readByte(vm, a[0].a) >= 0) ++skipped;
        return I(skipped);
    });
    vm.def(dataInput, "readUTF:()Ljava/lang/String;", [](VM& vm, Value* a) {
        const int length = static_cast<int>(readBig(vm, a[0].a, 2));
        std::u16string text;
        for (int i = 0; i < length;) {
            const int first = need(vm, a[0].a);
            if (first < 0x80) {
                text += static_cast<char16_t>(first);
                i += 1;
            } else if ((first & 0xE0) == 0xC0) {
                text += static_cast<char16_t>((first & 0x1F) << 6 | (need(vm, a[0].a) & 0x3F));
                i += 2;
            } else {
                const int second = need(vm, a[0].a);
                text += static_cast<char16_t>((first & 0x0F) << 12 | (second & 0x3F) << 6 | (need(vm, a[0].a) & 0x3F));
                i += 3;
            }
        }
        return A(vm.newString(std::move(text)));
    });

    const char* output = "java/io/OutputStream";
    vm.def(output, "write:(I)V", [](VM& vm, Value* a) { writeByte(vm, a[0].a, a[1].i); return Value(); });
    vm.def(output, "write:([B)V", [](VM& vm, Value* a) {
        auto* source = arrayArg(vm, a[1].a);
        for (int i = 0; i < source->length; ++i) writeByte(vm, a[0].a, source->b()[i]);
        return Value();
    });
    vm.def(output, "write:([BII)V", [](VM& vm, Value* a) {
        auto* source = arrayArg(vm, a[1].a);
        checkRange(vm, source, a[2].i, a[3].i);
        for (int i = 0; i < a[3].i; ++i) writeByte(vm, a[0].a, source->b()[a[2].i + i]);
        return Value();
    });
    vm.def(output, "flush:()V", [](VM&, Value*) { return Value(); });
    vm.def(output, "close:()V", [](VM&, Value*) { return Value(); });
    vm.defineNative("java/io/PrintStream", output);

    const char* byteOutput = "java/io/ByteArrayOutputStream";
    vm.defineNative(byteOutput, output);
    vm.def(byteOutput, "<init>:()V", [](VM&, Value* a) { attach<ByteOutput>(a[0].a); return Value(); });
    vm.def(byteOutput, "<init>:(I)V", [](VM&, Value* a) { attach<ByteOutput>(a[0].a); return Value(); });
    vm.def(byteOutput, "size:()I", [](VM& vm, Value* a) { return I(static_cast<int>(vm.payload<ByteOutput>(a[0].a).data.size())); });
    vm.def(byteOutput, "reset:()V", [](VM& vm, Value* a) { vm.payload<ByteOutput>(a[0].a).data.clear(); return Value(); });
    vm.def(byteOutput, "toByteArray:()[B", [](VM& vm, Value* a) { return A(newByteArray(vm, vm.payload<ByteOutput>(a[0].a).data)); });

    const char* dataOutput = "java/io/DataOutputStream";
    vm.defineNative(dataOutput, output);
    vm.def(dataOutput, "<init>:(Ljava/io/OutputStream;)V", [](VM&, Value* a) {
        attach<WrappedStream>(a[0].a).inner = a[1].a;
        return Value();
    });
    vm.def(dataOutput, "writeByte:(I)V", [](VM& vm, Value* a) { writeByte(vm, a[0].a, a[1].i); return Value(); });
    vm.def(dataOutput, "writeBoolean:(Z)V", [](VM& vm, Value* a) { writeByte(vm, a[0].a, a[1].i ? 1 : 0); return Value(); });
    vm.def(dataOutput, "writeShort:(I)V", [](VM& vm, Value* a) { writeBig(vm, a[0].a, a[1].i, 2); return Value(); });
    vm.def(dataOutput, "writeChar:(I)V", [](VM& vm, Value* a) { writeBig(vm, a[0].a, a[1].i, 2); return Value(); });
    vm.def(dataOutput, "writeInt:(I)V", [](VM& vm, Value* a) { writeBig(vm, a[0].a, a[1].i, 4); return Value(); });
    vm.def(dataOutput, "writeLong:(J)V", [](VM& vm, Value* a) { writeBig(vm, a[0].a, a[1].j, 8); return Value(); });
    vm.def(dataOutput, "writeUTF:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        Bytes encoded;
        for (const char16_t ch : vm.str(a[1].a)) {
            if (ch >= 1 && ch < 0x80) {
                encoded.push_back(static_cast<std::uint8_t>(ch));
            } else if (ch < 0x800) {
                encoded.push_back(static_cast<std::uint8_t>(0xC0 | ch >> 6));
                encoded.push_back(static_cast<std::uint8_t>(0x80 | (ch & 0x3F)));
            } else {
                encoded.push_back(static_cast<std::uint8_t>(0xE0 | ch >> 12));
                encoded.push_back(static_cast<std::uint8_t>(0x80 | (ch >> 6 & 0x3F)));
                encoded.push_back(static_cast<std::uint8_t>(0x80 | (ch & 0x3F)));
            }
        }
        writeBig(vm, a[0].a, static_cast<std::int64_t>(encoded.size()), 2);
        for (const auto byte : encoded) writeByte(vm, a[0].a, byte);
        return Value();
    });
}

void defineUtil(VM& vm) {
    const char* vector = "java/util/Vector";
    vm.def(vector, "<init>:()V", [](VM&, Value* a) { attach<VectorData>(a[0].a); return Value(); });
    vm.def(vector, "<init>:(I)V", [](VM&, Value* a) { attach<VectorData>(a[0].a); return Value(); });
    vm.def(vector, "size:()I", [](VM& vm, Value* a) { return I(static_cast<int>(vm.payload<VectorData>(a[0].a).items.size())); });
    vm.def(vector, "isEmpty:()Z", [](VM& vm, Value* a) { return I(vm.payload<VectorData>(a[0].a).items.empty()); });
    vm.def(vector, "addElement:(Ljava/lang/Object;)V", [](VM& vm, Value* a) {
        vm.payload<VectorData>(a[0].a).items.push_back(a[1].a);
        return Value();
    });
    vm.def(vector, "elementAt:(I)Ljava/lang/Object;", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        if (static_cast<std::uint32_t>(a[1].i) >= items.size()) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        return A(items[a[1].i]);
    });
    vm.def(vector, "setElementAt:(Ljava/lang/Object;I)V", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        if (static_cast<std::uint32_t>(a[2].i) >= items.size()) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        items[a[2].i] = a[1].a;
        return Value();
    });
    vm.def(vector, "insertElementAt:(Ljava/lang/Object;I)V", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        if (static_cast<std::uint32_t>(a[2].i) > items.size()) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        items.insert(items.begin() + a[2].i, a[1].a);
        return Value();
    });
    vm.def(vector, "removeElementAt:(I)V", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        if (static_cast<std::uint32_t>(a[1].i) >= items.size()) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        items.erase(items.begin() + a[1].i);
        return Value();
    });
    vm.def(vector, "removeElement:(Ljava/lang/Object;)Z", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (sameKey(*it, a[1].a)) {
                items.erase(it);
                return I(1);
            }
        }
        return I(0);
    });
    vm.def(vector, "removeAllElements:()V", [](VM& vm, Value* a) {
        vm.payload<VectorData>(a[0].a).items.clear();
        return Value();
    });
    vm.def(vector, "indexOf:(Ljava/lang/Object;)I", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (sameKey(items[i], a[1].a)) return I(static_cast<int>(i));
        }
        return I(-1);
    });
    vm.def(vector, "contains:(Ljava/lang/Object;)Z", [](VM& vm, Value* a) {
        auto& items = vm.payload<VectorData>(a[0].a).items;
        return I(std::any_of(items.begin(), items.end(), [&](Object* item) { return sameKey(item, a[1].a); }));
    });
    vm.def(vector, "elements:()Ljava/util/Enumeration;", [](VM& vm, Value* a) {
        return A(newEnumeration(vm, vm.payload<VectorData>(a[0].a).items));
    });

    const char* table = "java/util/Hashtable";
    vm.def(table, "<init>:()V", [](VM&, Value* a) { attach<HashtableData>(a[0].a); return Value(); });
    vm.def(table, "<init>:(I)V", [](VM&, Value* a) { attach<HashtableData>(a[0].a); return Value(); });
    vm.def(table, "size:()I", [](VM& vm, Value* a) { return I(static_cast<int>(vm.payload<HashtableData>(a[0].a).entries.size())); });
    vm.def(table, "isEmpty:()Z", [](VM& vm, Value* a) { return I(vm.payload<HashtableData>(a[0].a).entries.empty()); });
    vm.def(table, "clear:()V", [](VM& vm, Value* a) { vm.payload<HashtableData>(a[0].a).entries.clear(); return Value(); });
    vm.def(table, "get:(Ljava/lang/Object;)Ljava/lang/Object;", [](VM& vm, Value* a) {
        for (auto& [key, value] : vm.payload<HashtableData>(a[0].a).entries) {
            if (sameKey(key, a[1].a)) return A(value);
        }
        return A(nullptr);
    });
    vm.def(table, "containsKey:(Ljava/lang/Object;)Z", [](VM& vm, Value* a) {
        for (auto& [key, value] : vm.payload<HashtableData>(a[0].a).entries) {
            if (sameKey(key, a[1].a)) return I(1);
        }
        return I(0);
    });
    vm.def(table, "put:(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", [](VM& vm, Value* a) {
        if (!a[1].a || !a[2].a) vm.throwNew("java/lang/NullPointerException");
        auto& entries = vm.payload<HashtableData>(a[0].a).entries;
        for (auto& [key, value] : entries) {
            if (sameKey(key, a[1].a)) {
                auto* previous = value;
                value = a[2].a;
                return A(previous);
            }
        }
        entries.emplace_back(a[1].a, a[2].a);
        return A(nullptr);
    });
    vm.def(table, "remove:(Ljava/lang/Object;)Ljava/lang/Object;", [](VM& vm, Value* a) {
        auto& entries = vm.payload<HashtableData>(a[0].a).entries;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (sameKey(it->first, a[1].a)) {
                auto* previous = it->second;
                entries.erase(it);
                return A(previous);
            }
        }
        return A(nullptr);
    });
    vm.def(table, "keys:()Ljava/util/Enumeration;", [](VM& vm, Value* a) {
        std::vector<Object*> keys;
        for (auto& [key, value] : vm.payload<HashtableData>(a[0].a).entries) keys.push_back(key);
        return A(newEnumeration(vm, std::move(keys)));
    });
    vm.def(table, "elements:()Ljava/util/Enumeration;", [](VM& vm, Value* a) {
        std::vector<Object*> values;
        for (auto& [key, value] : vm.payload<HashtableData>(a[0].a).entries) values.push_back(value);
        return A(newEnumeration(vm, std::move(values)));
    });

    vm.defineNative("java/util/Enumeration");
    const char* items = "java/util/Enumeration$Items";
    vm.defineNative(items, "java/lang/Object", {"java/util/Enumeration"});
    vm.def(items, "hasMoreElements:()Z", [](VM& vm, Value* a) {
        auto& data = vm.payload<EnumerationData>(a[0].a);
        return I(data.at < data.items.size());
    });
    vm.def(items, "nextElement:()Ljava/lang/Object;", [](VM& vm, Value* a) {
        auto& data = vm.payload<EnumerationData>(a[0].a);
        if (data.at >= data.items.size()) vm.throwNew("java/util/NoSuchElementException");
        return A(data.items[data.at++]);
    });
    vm.defineNative("java/util/NoSuchElementException", "java/lang/RuntimeException");

    const char* random = "java/util/Random";
    vm.def(random, "<init>:()V", [](VM&, Value* a) { attach<RandomData>(a[0].a).set(now()); return Value(); });
    vm.def(random, "<init>:(J)V", [](VM&, Value* a) { attach<RandomData>(a[0].a).set(a[1].j); return Value(); });
    vm.def(random, "setSeed:(J)V", [](VM& vm, Value* a) { vm.payload<RandomData>(a[0].a).set(a[1].j); return Value(); });
    vm.def(random, "nextInt:()I", [](VM& vm, Value* a) { return I(vm.payload<RandomData>(a[0].a).next(32)); });
    vm.def(random, "nextLong:()J", [](VM& vm, Value* a) {
        auto& data = vm.payload<RandomData>(a[0].a);
        const std::int64_t high = data.next(32);
        return J(static_cast<std::int64_t>(static_cast<std::uint64_t>(high) << 32) + data.next(32));
    });
}
} // namespace

bool verbose = false;
double timeScale = 1.0;

std::int64_t gameMillis() {
    using namespace std::chrono;
    static const auto wallStart = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    static const auto start = steady_clock::now();
    return wallStart + static_cast<std::int64_t>(duration<double, std::milli>(steady_clock::now() - start).count() * timeScale);
}

Array* newByteArray(VM& vm, const Bytes& data) {
    auto* array = vm.newArray('B', static_cast<int>(data.size()));
    if (!data.empty()) std::memcpy(array->b(), data.data(), data.size());
    return array;
}

Bytes bytesOf(Array* array, int offset, int length) {
    const auto* start = reinterpret_cast<const std::uint8_t*>(array->b()) + offset;
    return Bytes(start, start + length);
}

const Bytes* findResource(VM& vm, std::string name) {
    while (!name.empty() && name[0] == '/') name.erase(0, 1);
    return vm.jar.find(name);
}

Object* openResource(VM& vm, const std::string& name) {
    const auto* data = findResource(vm, name);
    if (verbose) std::fprintf(stderr, "[runtime] resource %s%s\n", name.c_str(), data ? "" : " (missing)");
    if (!data) return nullptr;
    auto input = std::make_unique<ByteInput>();
    input->own = *data;
    return vm.make("java/io/ByteArrayInputStream", std::move(input));
}

Bytes drain(VM& vm, Object* stream) {
    Bytes data;
    for (int value; (value = readByte(vm, stream)) >= 0;) data.push_back(static_cast<std::uint8_t>(value));
    return data;
}

void defineCore(VM& vm) {
    defineLang(vm);
    defineIo(vm);
    defineUtil(vm);
}

} // namespace vm
