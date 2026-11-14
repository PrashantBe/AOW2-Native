#pragma once

// A small Java virtual machine sized for one CLDC 1.0 game: integer and long
// arithmetic, no floating point, no reflection. The game's own classes are
// interpreted from the JAR. Platform classes are implemented in C++.

#include "jar.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vm {

struct Object;
struct Class;
struct Thread;
class VM;

union Value {
    std::int32_t i;
    std::int64_t j;
    Object* a;
    Value() : j(0) {}
};
inline Value I(std::int32_t v) { Value x; x.i = v; return x; }
inline Value J(std::int64_t v) { Value x; x.j = v; return x; }
inline Value A(Object* v) { Value x; x.a = v; return x; }

// Collects the references held by an object during garbage collection.
using Marker = std::vector<Object*>;

// C++ state attached to an object of a platform class.
struct Native {
    virtual ~Native() = default;
    virtual void trace(Marker&) {}
};

struct Object {
    Class* cls = nullptr;
    Thread* lockOwner = nullptr;
    int lockCount = 0;
    bool marked = false;
    virtual ~Object() = default;
};

struct Instance : Object {
    std::unique_ptr<Value[]> fields;
    std::unique_ptr<Native> native;
};

struct Array : Object {
    std::int32_t length = 0;
    void* data = nullptr;
    ~Array() override;
    std::int8_t* b() { return static_cast<std::int8_t*>(data); }
    std::uint16_t* c() { return static_cast<std::uint16_t*>(data); }
    std::int16_t* s() { return static_cast<std::int16_t*>(data); }
    std::int32_t* i() { return static_cast<std::int32_t*>(data); }
    std::int64_t* j() { return static_cast<std::int64_t*>(data); }
    Object** a() { return static_cast<Object**>(data); }
};

struct Field {
    Class* owner = nullptr;
    std::string name;
    std::string desc;
    bool isStatic = false;
    int slot = 0;
    char type() const { return desc[0]; }
};

struct Handler {
    int start, end, handler, type;
};

using NativeFn = Value (*)(VM&, Value*);

struct Method {
    Class* owner = nullptr;
    std::string name;
    std::string desc;
    std::uint16_t flags = 0;
    Bytes code;
    int maxStack = 0;
    int maxLocals = 0;
    std::vector<Handler> handlers;
    NativeFn native = nullptr;
    int argSlots = 0; // Including the receiver.
    char ret = 'V';
};

struct CpEntry {
    std::uint8_t tag = 0;
    std::int32_t i = 0;
    std::int64_t j = 0;
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    std::string text;
    // Filled in the first time the entry is used.
    bool resolved = false;
    Class* cls = nullptr;
    Field* field = nullptr;
    Method* method = nullptr;
    Object* string = nullptr;
    std::string sig;
    int argSlots = 0;
    char ret = 'V';
    Class* cacheClass = nullptr;
    Method* cacheMethod = nullptr;
};

struct Class {
    std::string name;
    Class* super = nullptr;
    std::vector<Class*> interfaces;
    std::uint16_t flags = 0;
    bool fromJar = false;
    std::vector<CpEntry> cp;
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<std::unique_ptr<Method>> methodList;
    std::unordered_map<std::string, Method*> methods; // Keyed by "name:descriptor".
    int instanceSlots = 0;
    std::vector<char> slotIsRef;
    std::vector<Value> statics;
    std::vector<char> staticIsRef;
    bool initialized = false;
    char elem = 0; // Element type of an array class: B C S I J, or L for references.
    Class* elemClass = nullptr;
    Object* mirror = nullptr;
};

struct JavaThrow {
    Object* ex;
};

struct Thread {
    std::vector<Value> stack;
    Value* sp = nullptr;
    int budget = 0;
    std::vector<Method*> frames;
    Thread() : stack(1 << 16) { sp = stack.data(); }
};

// One Java thread runs at a time. Waiters are served in arrival order.
class Lock {
public:
    void lock();
    void unlock();

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::uint64_t next_ = 0;
    std::uint64_t serving_ = 0;
};

struct ThrowableData : Native {
    std::u16string message;
};

struct StringData : Native {
    std::u16string text;
};

struct ClassData : Native {
    Class* cls = nullptr;
};

class VM {
public:
    explicit VM(const Jar& jar);
    ~VM();

    const Jar& jar;
    Lock lock; // Held by whichever thread is running Java code or touching the heap.

    // Classes
    Class* findClass(const std::string& name);
    Class* defineNative(const std::string& name, const std::string& super = "java/lang/Object",
                        std::initializer_list<const char*> interfaces = {});
    void def(const char* cls, const char* sig, NativeFn fn, bool isStatic = false);
    void defStatic(const char* cls, const char* sig, NativeFn fn) { def(cls, sig, fn, true); }
    Field* addStaticField(Class* cls, const std::string& name, const std::string& desc, Value value);
    Method* findMethod(Class* cls, const std::string& sig);
    Field* findField(Class* cls, const std::string& name, const std::string& desc);
    bool instanceOf(Class* from, Class* to);
    void initClass(Class* cls);
    Object* mirror(Class* cls);

    // Objects
    Object* newInstance(Class* cls);
    Object* make(const char* cls, std::unique_ptr<Native> native);
    Array* newArray(Class* arrayClass, int length);
    Array* newArray(char elem, int length);
    Object* newString(std::u16string text);
    Object* newString(const std::string& latin1);
    const std::u16string& str(Object* string);
    template <class T> T& payload(Object* object) {
        if (!object) {
            throwNew("java/lang/NullPointerException");
        }
        auto* data = dynamic_cast<T*>(static_cast<Instance*>(object)->native.get());
        if (!data) {
            fatal("object of class " + object->cls->name + " lacks its native state");
        }
        return *data;
    }
    void pin(Object* object) { if (object) pins_.insert(object); }
    void unpin(Object* object) { pins_.erase(object); }
    void charge(std::size_t bytes) { allocated_ += bytes; }

    // Execution
    Value invoke(Method* method, Value* args);
    // Calls a method on the receiver's class if it exists. Returns false when it is not defined.
    bool call(Object* receiver, const std::string& sig, std::vector<Value> args = {}, Value* result = nullptr);
    [[noreturn]] void throwNew(const char* cls, const std::string& message = "");
    [[noreturn]] void fatal(const std::string& message);
    std::string describe(Object* throwable);
    void monitorEnter(Object* object);
    void monitorExit(Object* object);
    void waitOn(Object* object);
    void notifyAll() { ++notifyEpoch_; }
    void startThread(Object* runnable);
    // Lets other threads run, and collects garbage when enough has been allocated.
    void yield();
    void collect();
    Thread& self();
    std::string trace();

private:
    Value execute(Method* method, Value* args);
    Class* loadClass(const std::string& name, const Bytes& data);
    Class* arrayClass(const std::string& name);
    Class* resolveClass(Class* user, int index);
    void resolveMember(Class* user, CpEntry& entry, bool isField);
    Object* resolveString(CpEntry& entry);
    Array* newMulti(Class* arrayClass, const Value* counts, int dims);
    void track(Object* object, std::size_t bytes);

    std::unordered_map<std::string, std::unique_ptr<Class>> classes_;
    std::vector<Object*> heap_;
    std::unordered_set<Object*> heapSet_;
    std::unordered_set<Object*> pins_;
    std::vector<Thread*> threads_;
    std::size_t allocated_ = 0;
    std::uint64_t notifyEpoch_ = 0;
    Class* stringClass_ = nullptr;
};

std::string narrow(const std::u16string& text);
std::u16string widen(const std::string& latin1);
int argumentSlots(const std::string& descriptor);

} // namespace vm
