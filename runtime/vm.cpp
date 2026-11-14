#include "vm.hpp"

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace vm {

namespace {
constexpr std::size_t CollectAfterBytes = 24u << 20;
constexpr int YieldAfterBranches = 50000;
constexpr std::uint16_t AccStatic = 0x0008;
constexpr std::uint16_t AccSynchronized = 0x0020;

thread_local Thread* current = nullptr;

struct Reader {
    const Bytes& data;
    std::size_t at = 0;
    std::uint32_t u(int size) {
        if (at + size > data.size()) {
            throw std::runtime_error("class file is truncated");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < size; ++i) {
            value = value << 8 | data[at++];
        }
        return value;
    }
    std::string bytes(std::size_t size) {
        if (at + size > data.size()) {
            throw std::runtime_error("class file is truncated");
        }
        std::string text(data.begin() + at, data.begin() + at + size);
        at += size;
        return text;
    }
};

// Class files store strings in Java's modified UTF-8.
std::u16string decodeUtf(const std::string& text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char a = text[i];
        if (a < 0x80) {
            out += static_cast<char16_t>(a);
            i += 1;
        } else if ((a & 0xE0) == 0xC0 && i + 1 < text.size()) {
            out += static_cast<char16_t>((a & 0x1F) << 6 | (text[i + 1] & 0x3F));
            i += 2;
        } else if (i + 2 < text.size()) {
            out += static_cast<char16_t>((a & 0x0F) << 12 | (text[i + 1] & 0x3F) << 6 | (text[i + 2] & 0x3F));
            i += 3;
        } else {
            break;
        }
    }
    return out;
}

char returnType(const std::string& descriptor) {
    const char type = descriptor[descriptor.find(')') + 1];
    return type == 'V' || type == 'J' ? type : (type == 'L' || type == '[' ? 'L' : 'I');
}

int elementSize(char elem) {
    switch (elem) {
    case 'B': case 'Z': return 1;
    case 'C': case 'S': return 2;
    case 'I': return 4;
    default: return 8;
    }
}
} // namespace

std::string narrow(const std::u16string& text) {
    std::string out;
    for (const char16_t ch : text) {
        out += ch < 256 ? static_cast<char>(ch) : '?';
    }
    return out;
}

std::u16string widen(const std::string& latin1) {
    std::u16string out;
    for (const unsigned char ch : latin1) {
        out += static_cast<char16_t>(ch);
    }
    return out;
}

int argumentSlots(const std::string& descriptor) {
    int slots = 0;
    for (std::size_t i = 1; descriptor[i] != ')'; ++i) {
        const bool isArray = descriptor[i] == '[';
        while (descriptor[i] == '[') {
            ++i;
        }
        if (descriptor[i] == 'L') {
            i = descriptor.find(';', i);
        }
        slots += !isArray && (descriptor[i] == 'J' || descriptor[i] == 'D') ? 2 : 1;
    }
    return slots;
}

Array::~Array() {
    std::free(data);
}

void Lock::lock() {
    std::unique_lock<std::mutex> guard(mutex_);
    const auto ticket = next_++;
    ready_.wait(guard, [&] { return serving_ == ticket; });
}

void Lock::unlock() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        ++serving_;
    }
    ready_.notify_all();
}

VM::VM(const Jar& source) : jar(source) {
    auto object = std::make_unique<Class>();
    object->name = "java/lang/Object";
    object->initialized = true;
    classes_[object->name] = std::move(object);
    stringClass_ = defineNative("java/lang/String");
    defineNative("java/lang/Class");
    defineNative("java/lang/Throwable");
}

VM::~VM() {
    for (auto* object : heap_) {
        delete object;
    }
}

Thread& VM::self() {
    if (!current) {
        current = new Thread();
        threads_.push_back(current);
    }
    return *current;
}

std::string VM::trace() {
    std::string out;
    if (current) {
        for (auto it = current->frames.rbegin(); it != current->frames.rend(); ++it) {
            out += "    at " + (*it)->owner->name + "." + (*it)->name + (*it)->desc + "\n";
        }
    }
    return out;
}

void VM::fatal(const std::string& message) {
    std::fprintf(stderr, "Runtime error: %s\n%s", message.c_str(), trace().c_str());
    throw std::runtime_error(message);
}

std::string VM::describe(Object* throwable) {
    std::string out = throwable->cls->name;
    if (throwable->cls->elem == 0) {
        if (auto* data = dynamic_cast<ThrowableData*>(static_cast<Instance*>(throwable)->native.get())) {
            if (!data->message.empty()) {
                out += ": " + narrow(data->message);
            }
        }
    }
    return out;
}

void VM::throwNew(const char* cls, const std::string& message) {
    auto data = std::make_unique<ThrowableData>();
    data->message = widen(message);
    throw JavaThrow{make(cls, std::move(data))};
}

// ---------------------------------------------------------------- classes

Class* VM::defineNative(const std::string& name, const std::string& super,
                        std::initializer_list<const char*> interfaces) {
    const auto found = classes_.find(name);
    if (found != classes_.end()) {
        return found->second.get();
    }
    auto cls = std::make_unique<Class>();
    cls->name = name;
    cls->super = findClass(super);
    cls->initialized = true;
    for (const char* interface : interfaces) {
        cls->interfaces.push_back(findClass(interface));
    }
    auto* result = cls.get();
    classes_[name] = std::move(cls);
    return result;
}

void VM::def(const char* cls, const char* sig, NativeFn fn, bool isStatic) {
    auto* owner = defineNative(cls);
    auto method = std::make_unique<Method>();
    const std::string text = sig;
    const auto colon = text.find(':');
    method->owner = owner;
    method->name = text.substr(0, colon);
    method->desc = text.substr(colon + 1);
    method->flags = isStatic ? AccStatic : 0;
    method->native = fn;
    method->argSlots = argumentSlots(method->desc) + (isStatic ? 0 : 1);
    method->ret = returnType(method->desc);
    owner->methods[text] = method.get();
    owner->methodList.push_back(std::move(method));
}

Field* VM::addStaticField(Class* cls, const std::string& name, const std::string& desc, Value value) {
    auto field = std::make_unique<Field>();
    field->owner = cls;
    field->name = name;
    field->desc = desc;
    field->isStatic = true;
    field->slot = static_cast<int>(cls->statics.size());
    cls->statics.push_back(value);
    cls->staticIsRef.push_back(desc[0] == 'L' || desc[0] == '[');
    cls->fields.push_back(std::move(field));
    return cls->fields.back().get();
}

Class* VM::arrayClass(const std::string& name) {
    auto cls = std::make_unique<Class>();
    cls->name = name;
    cls->super = findClass("java/lang/Object");
    cls->initialized = true;
    const char type = name[1];
    if (type == 'L') {
        cls->elem = 'L';
        cls->elemClass = findClass(name.substr(2, name.size() - 3));
    } else if (type == '[') {
        cls->elem = 'L';
        cls->elemClass = findClass(name.substr(1));
    } else {
        cls->elem = type == 'Z' ? 'B' : type;
    }
    auto* result = cls.get();
    classes_[name] = std::move(cls);
    return result;
}

Class* VM::findClass(const std::string& name) {
    const auto found = classes_.find(name);
    if (found != classes_.end()) {
        return found->second.get();
    }
    if (name[0] == '[') {
        return arrayClass(name);
    }
    if (const auto* data = jar.find(name + ".class")) {
        return loadClass(name, *data);
    }
    std::fprintf(stderr, "Note: platform class %s is not implemented; using an empty stand-in\n", name.c_str());
    return defineNative(name);
}

Class* VM::loadClass(const std::string& name, const Bytes& data) {
    Reader in{data};
    if (in.u(4) != 0xCAFEBABE) {
        throw std::runtime_error(name + " is not a class file");
    }
    in.u(4);
    auto owned = std::make_unique<Class>();
    auto* cls = owned.get();
    cls->name = name;
    cls->fromJar = true;
    classes_[name] = std::move(owned);

    const int count = in.u(2);
    cls->cp.resize(count);
    for (int i = 1; i < count; ++i) {
        auto& entry = cls->cp[i];
        entry.tag = in.u(1);
        switch (entry.tag) {
        case 1: entry.text = in.bytes(in.u(2)); break;
        case 3: case 4: entry.i = static_cast<std::int32_t>(in.u(4)); break;
        case 5: case 6: {
            const std::uint64_t high = in.u(4);
            entry.j = static_cast<std::int64_t>(high << 32 | in.u(4));
            ++i; // Longs take two pool slots.
            break;
        }
        case 7: case 8: entry.a = in.u(2); break;
        case 9: case 10: case 11: case 12:
            entry.a = in.u(2);
            entry.b = in.u(2);
            break;
        default: throw std::runtime_error(name + " has an unknown constant type");
        }
    }
    cls->flags = in.u(2);
    in.u(2);
    const int superIndex = in.u(2);
    cls->super = findClass(superIndex ? cls->cp[cls->cp[superIndex].a].text : "java/lang/Object");
    for (int i = in.u(2); i > 0; --i) {
        cls->interfaces.push_back(findClass(cls->cp[cls->cp[in.u(2)].a].text));
    }
    cls->instanceSlots = cls->super->instanceSlots;
    cls->slotIsRef = cls->super->slotIsRef;

    for (int i = in.u(2); i > 0; --i) {
        auto field = std::make_unique<Field>();
        const auto flags = in.u(2);
        field->owner = cls;
        field->name = cls->cp[in.u(2)].text;
        field->desc = cls->cp[in.u(2)].text;
        field->isStatic = flags & AccStatic;
        const bool isRef = field->desc[0] == 'L' || field->desc[0] == '[';
        if (field->isStatic) {
            field->slot = static_cast<int>(cls->statics.size());
            cls->statics.emplace_back();
            cls->staticIsRef.push_back(isRef);
        } else {
            field->slot = cls->instanceSlots++;
            cls->slotIsRef.push_back(isRef);
        }
        for (int attributes = in.u(2); attributes > 0; --attributes) {
            const auto& attribute = cls->cp[in.u(2)].text;
            const auto length = in.u(4);
            if (attribute == "ConstantValue" && field->isStatic) {
                auto& constant = cls->cp[in.u(2)];
                auto& slot = cls->statics[field->slot];
                if (constant.tag == 3) slot.i = constant.i;
                else if (constant.tag == 5) slot.j = constant.j;
                else if (constant.tag == 8) slot.a = resolveString(cls->cp[constant.a]);
            } else {
                in.at += length;
            }
        }
        cls->fields.push_back(std::move(field));
    }

    for (int i = in.u(2); i > 0; --i) {
        auto method = std::make_unique<Method>();
        method->owner = cls;
        method->flags = in.u(2);
        method->name = cls->cp[in.u(2)].text;
        method->desc = cls->cp[in.u(2)].text;
        method->argSlots = argumentSlots(method->desc) + (method->flags & AccStatic ? 0 : 1);
        method->ret = returnType(method->desc);
        for (int attributes = in.u(2); attributes > 0; --attributes) {
            const auto& attribute = cls->cp[in.u(2)].text;
            const auto length = in.u(4);
            if (attribute != "Code") {
                in.at += length;
                continue;
            }
            method->maxStack = in.u(2);
            method->maxLocals = in.u(2);
            const auto size = in.u(4);
            method->code.assign(data.begin() + in.at, data.begin() + in.at + size);
            in.at += size;
            for (int handlers = in.u(2); handlers > 0; --handlers) {
                Handler handler{};
                handler.start = in.u(2);
                handler.end = in.u(2);
                handler.handler = in.u(2);
                handler.type = in.u(2);
                method->handlers.push_back(handler);
            }
            for (int nested = in.u(2); nested > 0; --nested) {
                in.u(2);
                in.at += in.u(4);
            }
        }
        cls->methods[method->name + ":" + method->desc] = method.get();
        cls->methodList.push_back(std::move(method));
    }
    return cls;
}

Method* VM::findMethod(Class* cls, const std::string& sig) {
    for (auto* c = cls; c; c = c->super) {
        const auto found = c->methods.find(sig);
        if (found != c->methods.end()) {
            return found->second;
        }
    }
    return nullptr;
}

Field* VM::findField(Class* cls, const std::string& name, const std::string& desc) {
    for (auto* c = cls; c; c = c->super) {
        for (const auto& field : c->fields) {
            if (field->name == name && field->desc == desc) {
                return field.get();
            }
        }
    }
    return nullptr;
}

bool VM::instanceOf(Class* from, Class* to) {
    if (from == to || to->super == nullptr) {
        return true;
    }
    if (from->elem && to->elem) {
        return from->elem == 'L' && to->elem == 'L' && instanceOf(from->elemClass, to->elemClass);
    }
    for (auto* c = from; c; c = c->super) {
        if (c == to) {
            return true;
        }
        for (auto* interface : c->interfaces) {
            if (instanceOf(interface, to)) {
                return true;
            }
        }
    }
    return false;
}

void VM::initClass(Class* cls) {
    if (cls->initialized) {
        return;
    }
    cls->initialized = true;
    if (cls->super) {
        initClass(cls->super);
    }
    const auto found = cls->methods.find("<clinit>:()V");
    if (found != cls->methods.end()) {
        invoke(found->second, nullptr);
    }
}

Object* VM::mirror(Class* cls) {
    if (!cls->mirror) {
        auto data = std::make_unique<ClassData>();
        data->cls = cls;
        cls->mirror = make("java/lang/Class", std::move(data));
    }
    return cls->mirror;
}

Class* VM::resolveClass(Class* user, int index) {
    auto& entry = user->cp[index];
    if (!entry.cls) {
        entry.cls = findClass(user->cp[entry.a].text);
    }
    return entry.cls;
}

void VM::resolveMember(Class* user, CpEntry& entry, bool isField) {
    auto* cls = resolveClass(user, entry.a);
    const auto& pair = user->cp[entry.b];
    const auto& name = user->cp[pair.a].text;
    const auto& desc = user->cp[pair.b].text;
    if (isField) {
        entry.field = findField(cls, name, desc);
        if (!entry.field) {
            fatal("missing field " + cls->name + "." + name);
        }
    } else {
        entry.sig = name + ":" + desc;
        entry.method = findMethod(cls, entry.sig);
        entry.argSlots = argumentSlots(desc);
        entry.ret = returnType(desc);
    }
    entry.cls = cls;
    entry.resolved = true;
}

Object* VM::resolveString(CpEntry& entry) {
    if (!entry.string) {
        entry.string = newString(decodeUtf(entry.text));
    }
    return entry.string;
}

// ---------------------------------------------------------------- objects

void VM::track(Object* object, std::size_t bytes) {
    heap_.push_back(object);
    heapSet_.insert(object);
    allocated_ += bytes;
}

Object* VM::newInstance(Class* cls) {
    auto* object = new Instance();
    object->cls = cls;
    object->fields = std::make_unique<Value[]>(cls->instanceSlots);
    track(object, 64 + 8 * cls->instanceSlots);
    return object;
}

Object* VM::make(const char* cls, std::unique_ptr<Native> native) {
    auto* object = static_cast<Instance*>(newInstance(findClass(cls)));
    object->native = std::move(native);
    return object;
}

Array* VM::newArray(Class* arrayClass, int length) {
    if (length < 0) {
        throwNew("java/lang/NegativeArraySizeException");
    }
    auto* array = new Array();
    array->cls = arrayClass;
    array->length = length;
    const auto size = static_cast<std::size_t>(elementSize(arrayClass->elem));
    array->data = std::calloc(static_cast<std::size_t>(length) + 1, size);
    track(array, 48 + size * length);
    return array;
}

Array* VM::newArray(char elem, int length) {
    return newArray(findClass(std::string("[") + elem), length);
}

Array* VM::newMulti(Class* arrayClass, const Value* counts, int dims) {
    auto* array = newArray(arrayClass, counts[0].i);
    if (dims > 1) {
        // The partly built outer array is not reachable yet, so keep it alive by hand.
        pin(array);
        for (int i = 0; i < array->length; ++i) {
            array->a()[i] = newMulti(arrayClass->elemClass, counts + 1, dims - 1);
        }
        unpin(array);
    }
    return array;
}

Object* VM::newString(std::u16string text) {
    auto data = std::make_unique<StringData>();
    allocated_ += 2 * text.size();
    data->text = std::move(text);
    auto* object = static_cast<Instance*>(newInstance(stringClass_));
    object->native = std::move(data);
    return object;
}

Object* VM::newString(const std::string& latin1) {
    return newString(widen(latin1));
}

const std::u16string& VM::str(Object* string) {
    return payload<StringData>(string).text;
}

// ---------------------------------------------------------------- threads

void VM::monitorEnter(Object* object) {
    if (!object) {
        throwNew("java/lang/NullPointerException");
    }
    auto* me = &self();
    while (object->lockOwner && object->lockOwner != me) {
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
    }
    object->lockOwner = me;
    ++object->lockCount;
}

void VM::monitorExit(Object* object) {
    if (object && object->lockOwner == &self() && --object->lockCount == 0) {
        object->lockOwner = nullptr;
    }
}

void VM::waitOn(Object* object) {
    if (!object || object->lockOwner != &self()) {
        throwNew("java/lang/IllegalMonitorStateException");
    }
    const int held = object->lockCount;
    object->lockOwner = nullptr;
    object->lockCount = 0;
    const auto epoch = notifyEpoch_;
    while (notifyEpoch_ == epoch) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        lock.lock();
    }
    monitorEnter(object);
    object->lockCount = held;
}

void VM::startThread(Object* runnable) {
    pin(runnable);
    std::thread([this, runnable] {
        lock.lock();
        try {
            call(runnable, "run:()V");
        } catch (const JavaThrow& thrown) {
            std::fprintf(stderr, "Uncaught exception in a game thread: %s\n", describe(thrown.ex).c_str());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Game thread stopped: %s\n", error.what());
            std::_Exit(1);
        }
        unpin(runnable);
        for (auto it = threads_.begin(); it != threads_.end(); ++it) {
            if (*it == current) {
                threads_.erase(it);
                break;
            }
        }
        delete current;
        current = nullptr;
        lock.unlock();
    }).detach();
}

void VM::yield() {
    if (allocated_ > CollectAfterBytes) {
        collect();
    }
    lock.unlock();
    lock.lock();
}

void VM::collect() {
    Marker work;
    const auto mark = [&](Object* object) {
        if (object && !object->marked) {
            object->marked = true;
            work.push_back(object);
        }
    };
    for (auto& [name, cls] : classes_) {
        for (std::size_t i = 0; i < cls->statics.size(); ++i) {
            if (cls->staticIsRef[i]) mark(cls->statics[i].a);
        }
        for (auto& entry : cls->cp) {
            mark(entry.string);
        }
        mark(cls->mirror);
    }
    for (auto* object : pins_) {
        mark(object);
    }
    // Stack slots are untyped, so anything that looks like a live object is kept.
    for (auto* thread : threads_) {
        for (Value* slot = thread->stack.data(); slot < thread->sp; ++slot) {
            if (heapSet_.count(slot->a)) mark(slot->a);
        }
    }
    Marker found;
    while (!work.empty()) {
        auto* object = work.back();
        work.pop_back();
        found.clear();
        if (object->cls->elem == 'L') {
            auto* array = static_cast<Array*>(object);
            found.assign(array->a(), array->a() + array->length);
        } else if (object->cls->elem == 0) {
            auto* instance = static_cast<Instance*>(object);
            for (int i = 0; i < object->cls->instanceSlots; ++i) {
                if (object->cls->slotIsRef[i]) found.push_back(instance->fields[i].a);
            }
            if (instance->native) instance->native->trace(found);
        }
        for (auto* reference : found) {
            mark(reference);
        }
    }
    std::size_t kept = 0;
    for (auto* object : heap_) {
        if (object->marked) {
            object->marked = false;
            heap_[kept++] = object;
        } else {
            heapSet_.erase(object);
            delete object;
        }
    }
    heap_.resize(kept);
    allocated_ = 0;
}

// ---------------------------------------------------------------- execution

bool VM::call(Object* receiver, const std::string& sig, std::vector<Value> args, Value* result) {
    auto* method = findMethod(receiver->cls, sig);
    if (!method) {
        return false;
    }
    args.insert(args.begin(), A(receiver));
    const Value value = invoke(method, args.data());
    if (result) {
        *result = value;
    }
    return true;
}

Value VM::invoke(Method* method, Value* args) {
    if (method->native) {
        return method->native(*this, args);
    }
    if (method->code.empty()) {
        fatal("abstract method " + method->owner->name + "." + method->name + " was called");
    }
    if (!(method->flags & AccSynchronized)) {
        return execute(method, args);
    }
    auto* guard = method->flags & AccStatic ? mirror(method->owner) : args[0].a;
    monitorEnter(guard);
    struct Release {
        VM& vm;
        Object* object;
        ~Release() { vm.monitorExit(object); }
    } release{*this, guard};
    return execute(method, args);
}

#define U1(n) (code[pc + (n)])
#define U2(n) (code[pc + (n)] << 8 | code[pc + (n) + 1])
#define S2(n) (static_cast<std::int16_t>(U2(n)))
#define S4(p) (static_cast<std::int32_t>(static_cast<std::uint32_t>(code[p]) << 24 | code[(p) + 1] << 16 | code[(p) + 2] << 8 | code[(p) + 3]))
#define PUSHI(v) (*sp++ = I(v))
#define PUSHA(v) (*sp++ = A(v))
#define PUSHJ(v) (sp[0] = J(v), sp[1].j = 0, sp += 2)
#define NPE() throwNew("java/lang/NullPointerException")
#define BRANCH(cond) \
    if (cond) { const int offset = S2(1); if (offset < 0 && --thread.budget < 0) { thread.budget = YieldAfterBranches; yield(); } pc += offset; } \
    else { pc += 3; } \
    break
#define ARRAY(ref, index) \
    auto* array = static_cast<Array*>(ref); \
    if (!array) NPE(); \
    if (static_cast<std::uint32_t>(index) >= static_cast<std::uint32_t>(array->length)) \
        throwNew("java/lang/ArrayIndexOutOfBoundsException", std::to_string(index))

Value VM::execute(Method* method, Value* args) {
    auto& thread = self();
    Value* const locals = thread.sp;
    Value* const base = locals + method->maxLocals;
    Value* const limit = base + method->maxStack;
    if (limit + 4 > thread.stack.data() + thread.stack.size()) {
        throwNew("java/lang/StackOverflowError");
    }
    for (int i = 0; i < method->argSlots; ++i) {
        locals[i] = args[i];
    }
    for (Value* slot = locals + method->argSlots; slot < limit; ++slot) {
        slot->j = 0;
    }
    thread.sp = limit;
    thread.frames.push_back(method);
    struct Leave {
        Thread& thread;
        Value* saved;
        ~Leave() {
            thread.sp = saved;
            thread.frames.pop_back();
        }
    } leave{thread, locals};

    Class* const cls = method->owner;
    auto& cp = cls->cp;
    const std::uint8_t* const code = method->code.data();
    Value* sp = base;
    int pc = 0;

    for (;;) {
        try {
            for (;;) {
                switch (code[pc]) {
                case 0: pc += 1; break;
                case 1: PUSHA(nullptr); pc += 1; break;
                case 2: case 3: case 4: case 5: case 6: case 7: case 8:
                    PUSHI(code[pc] - 3); pc += 1; break;
                case 9: case 10: PUSHJ(code[pc] - 9); pc += 1; break;
                case 16: PUSHI(static_cast<std::int8_t>(U1(1))); pc += 2; break;
                case 17: PUSHI(S2(1)); pc += 3; break;
                case 18: case 19: {
                    const bool wideIndex = code[pc] == 19;
                    auto& entry = cp[wideIndex ? U2(1) : U1(1)];
                    if (entry.tag == 3) PUSHI(entry.i);
                    else if (entry.tag == 8) PUSHA(resolveString(cp[entry.a]));
                    else fatal("unsupported constant");
                    pc += wideIndex ? 3 : 2;
                    break;
                }
                case 20: PUSHJ(cp[U2(1)].j); pc += 3; break;
                case 21: case 25: *sp++ = locals[U1(1)]; pc += 2; break;
                case 22: PUSHJ(locals[U1(1)].j); pc += 2; break;
                case 26: case 27: case 28: case 29: *sp++ = locals[code[pc] - 26]; pc += 1; break;
                case 30: case 31: case 32: case 33: PUSHJ(locals[code[pc] - 30].j); pc += 1; break;
                case 42: case 43: case 44: case 45: *sp++ = locals[code[pc] - 42]; pc += 1; break;
                case 46: { ARRAY(sp[-2].a, sp[-1].i); const std::int32_t loaded = array->i()[sp[-1].i]; sp -= 2; PUSHI(loaded); pc += 1; break; }
                case 47: { ARRAY(sp[-2].a, sp[-1].i); const std::int64_t loaded = array->j()[sp[-1].i]; sp -= 2; PUSHJ(loaded); pc += 1; break; }
                case 50: { ARRAY(sp[-2].a, sp[-1].i); Object* const loaded = array->a()[sp[-1].i]; sp -= 2; PUSHA(loaded); pc += 1; break; }
                case 51: { ARRAY(sp[-2].a, sp[-1].i); const std::int32_t loaded = array->b()[sp[-1].i]; sp -= 2; PUSHI(loaded); pc += 1; break; }
                case 52: { ARRAY(sp[-2].a, sp[-1].i); const std::int32_t loaded = array->c()[sp[-1].i]; sp -= 2; PUSHI(loaded); pc += 1; break; }
                case 53: { ARRAY(sp[-2].a, sp[-1].i); const std::int32_t loaded = array->s()[sp[-1].i]; sp -= 2; PUSHI(loaded); pc += 1; break; }
                case 54: case 58: locals[U1(1)] = *--sp; pc += 2; break;
                case 55: sp -= 2; locals[U1(1)] = sp[0]; pc += 2; break;
                case 59: case 60: case 61: case 62: locals[code[pc] - 59] = *--sp; pc += 1; break;
                case 63: case 64: case 65: case 66: sp -= 2; locals[code[pc] - 63] = sp[0]; pc += 1; break;
                case 75: case 76: case 77: case 78: locals[code[pc] - 75] = *--sp; pc += 1; break;
                case 79: { ARRAY(sp[-3].a, sp[-2].i); array->i()[sp[-2].i] = sp[-1].i; sp -= 3; pc += 1; break; }
                case 80: { ARRAY(sp[-4].a, sp[-3].i); array->j()[sp[-3].i] = sp[-2].j; sp -= 4; pc += 1; break; }
                case 83: { ARRAY(sp[-3].a, sp[-2].i); array->a()[sp[-2].i] = sp[-1].a; sp -= 3; pc += 1; break; }
                case 84: { ARRAY(sp[-3].a, sp[-2].i); array->b()[sp[-2].i] = static_cast<std::int8_t>(sp[-1].i); sp -= 3; pc += 1; break; }
                case 85: { ARRAY(sp[-3].a, sp[-2].i); array->c()[sp[-2].i] = static_cast<std::uint16_t>(sp[-1].i); sp -= 3; pc += 1; break; }
                case 86: { ARRAY(sp[-3].a, sp[-2].i); array->s()[sp[-2].i] = static_cast<std::int16_t>(sp[-1].i); sp -= 3; pc += 1; break; }
                case 87: sp -= 1; pc += 1; break;
                case 88: sp -= 2; pc += 1; break;
                case 89: sp[0] = sp[-1]; sp += 1; pc += 1; break;
                case 90: { const Value top = sp[-1]; sp[-1] = sp[-2]; sp[-2] = top; sp[0] = top; sp += 1; pc += 1; break; }
                case 91: { const Value top = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = top; sp[0] = top; sp += 1; pc += 1; break; }
                case 92: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; pc += 1; break;
                case 93: {
                    const Value one = sp[-1], two = sp[-2], three = sp[-3];
                    sp[-3] = two; sp[-2] = one; sp[-1] = three; sp[0] = two; sp[1] = one;
                    sp += 2; pc += 1; break;
                }
                case 94: {
                    const Value one = sp[-1], two = sp[-2], three = sp[-3], four = sp[-4];
                    sp[-4] = two; sp[-3] = one; sp[-2] = four; sp[-1] = three; sp[0] = two; sp[1] = one;
                    sp += 2; pc += 1; break;
                }
                case 95: { const Value top = sp[-1]; sp[-1] = sp[-2]; sp[-2] = top; pc += 1; break; }

#define INT_OP(expr) { const std::uint32_t y = static_cast<std::uint32_t>(sp[-1].i), x = static_cast<std::uint32_t>(sp[-2].i); (void)y; (void)x; sp -= 2; PUSHI(static_cast<std::int32_t>(expr)); pc += 1; break; }
#define LONG_OP(expr) { const std::uint64_t y = static_cast<std::uint64_t>(sp[-2].j), x = static_cast<std::uint64_t>(sp[-4].j); sp -= 4; PUSHJ(static_cast<std::int64_t>(expr)); pc += 1; break; }
#define LONG_SHIFT(expr) { const int y = sp[-1].i & 63; const std::int64_t x = sp[-3].j; sp -= 3; PUSHJ(static_cast<std::int64_t>(expr)); pc += 1; break; }
                case 96: INT_OP(x + y)
                case 97: LONG_OP(x + y)
                case 100: INT_OP(x - y)
                case 101: LONG_OP(x - y)
                case 104: INT_OP(x * y)
                case 105: LONG_OP(x * y)
                case 108: case 112: {
                    const std::int32_t y = sp[-1].i, x = sp[-2].i;
                    if (y == 0) throwNew("java/lang/ArithmeticException", "/ by zero");
                    sp -= 2;
                    if (code[pc] == 108) PUSHI(y == -1 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(x)) : x / y);
                    else PUSHI(y == -1 ? 0 : x % y);
                    pc += 1; break;
                }
                case 109: case 113: {
                    const std::int64_t y = sp[-2].j, x = sp[-4].j;
                    if (y == 0) throwNew("java/lang/ArithmeticException", "/ by zero");
                    sp -= 4;
                    if (code[pc] == 109) PUSHJ(y == -1 ? static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(x)) : x / y);
                    else PUSHJ(y == -1 ? 0 : x % y);
                    pc += 1; break;
                }
                case 116: sp[-1] = I(static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(sp[-1].i))); pc += 1; break;
                case 117: sp[-2] = J(static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(sp[-2].j))); pc += 1; break;
                case 120: INT_OP(x << (y & 31))
                case 121: LONG_SHIFT(static_cast<std::uint64_t>(x) << y)
                case 122: INT_OP(static_cast<std::int32_t>(x) >> (y & 31))
                case 123: LONG_SHIFT(x >> y)
                case 124: INT_OP(x >> (y & 31))
                case 125: LONG_SHIFT(static_cast<std::uint64_t>(x) >> y)
                case 126: INT_OP(x & y)
                case 127: LONG_OP(x & y)
                case 128: INT_OP(x | y)
                case 129: LONG_OP(x | y)
                case 130: INT_OP(x ^ y)
                case 131: LONG_OP(x ^ y)
                case 132: {
                    auto& slot = locals[U1(1)];
                    slot = I(static_cast<std::int32_t>(static_cast<std::uint32_t>(slot.i) + static_cast<std::int8_t>(U1(2))));
                    pc += 3; break;
                }
                case 133: { const std::int32_t value = (--sp)->i; PUSHJ(value); pc += 1; break; }
                case 136: { sp -= 2; const std::int64_t value = sp[0].j; PUSHI(static_cast<std::int32_t>(value)); pc += 1; break; }
                case 145: sp[-1] = I(static_cast<std::int8_t>(sp[-1].i)); pc += 1; break;
                case 146: sp[-1] = I(static_cast<std::uint16_t>(sp[-1].i)); pc += 1; break;
                case 147: sp[-1] = I(static_cast<std::int16_t>(sp[-1].i)); pc += 1; break;
                case 148: {
                    const std::int64_t y = sp[-2].j, x = sp[-4].j;
                    sp -= 4; PUSHI(x < y ? -1 : x > y ? 1 : 0); pc += 1; break;
                }
                case 153: sp -= 1; BRANCH(sp[0].i == 0);
                case 154: sp -= 1; BRANCH(sp[0].i != 0);
                case 155: sp -= 1; BRANCH(sp[0].i < 0);
                case 156: sp -= 1; BRANCH(sp[0].i >= 0);
                case 157: sp -= 1; BRANCH(sp[0].i > 0);
                case 158: sp -= 1; BRANCH(sp[0].i <= 0);
                case 159: sp -= 2; BRANCH(sp[0].i == sp[1].i);
                case 160: sp -= 2; BRANCH(sp[0].i != sp[1].i);
                case 161: sp -= 2; BRANCH(sp[0].i < sp[1].i);
                case 162: sp -= 2; BRANCH(sp[0].i >= sp[1].i);
                case 163: sp -= 2; BRANCH(sp[0].i > sp[1].i);
                case 164: sp -= 2; BRANCH(sp[0].i <= sp[1].i);
                case 165: sp -= 2; BRANCH(sp[0].a == sp[1].a);
                case 166: sp -= 2; BRANCH(sp[0].a != sp[1].a);
                case 167: BRANCH(true);
                case 198: sp -= 1; BRANCH(sp[0].a == nullptr);
                case 199: sp -= 1; BRANCH(sp[0].a != nullptr);
                case 200: pc += S4(pc + 1); break;
                case 170: {
                    const int table = (pc + 4) & ~3;
                    const std::int32_t value = (--sp)->i, low = S4(table + 4), high = S4(table + 8);
                    pc += value < low || value > high ? S4(table) : S4(table + 12 + 4 * (value - low));
                    break;
                }
                case 171: {
                    const int table = (pc + 4) & ~3;
                    const std::int32_t value = (--sp)->i, pairs = S4(table + 4);
                    int offset = S4(table);
                    for (int i = 0; i < pairs; ++i) {
                        if (S4(table + 8 + 8 * i) == value) {
                            offset = S4(table + 12 + 8 * i);
                            break;
                        }
                    }
                    pc += offset;
                    break;
                }
                case 172: case 176: return sp[-1];
                case 173: return sp[-2];
                case 177: return Value();
                case 178: case 179: {
                    auto& entry = cp[U2(1)];
                    if (!entry.resolved) {
                        resolveMember(cls, entry, true);
                        initClass(entry.field->owner);
                    }
                    auto& slot = entry.field->owner->statics[entry.field->slot];
                    const bool isLong = entry.field->type() == 'J';
                    if (code[pc] == 178) {
                        if (isLong) PUSHJ(slot.j); else *sp++ = slot;
                    } else {
                        sp -= isLong ? 2 : 1;
                        slot = sp[0];
                    }
                    pc += 3; break;
                }
                case 180: {
                    auto& entry = cp[U2(1)];
                    if (!entry.resolved) resolveMember(cls, entry, true);
                    auto* object = static_cast<Instance*>(sp[-1].a);
                    if (!object) NPE();
                    const Value value = object->fields[entry.field->slot];
                    sp -= 1;
                    if (entry.field->type() == 'J') PUSHJ(value.j); else *sp++ = value;
                    pc += 3; break;
                }
                case 181: {
                    auto& entry = cp[U2(1)];
                    if (!entry.resolved) resolveMember(cls, entry, true);
                    sp -= entry.field->type() == 'J' ? 2 : 1;
                    auto* object = static_cast<Instance*>(sp[-1].a);
                    if (!object) NPE();
                    object->fields[entry.field->slot] = sp[0];
                    sp -= 1;
                    pc += 3; break;
                }
                case 182: case 183: case 184: case 185: {
                    const int op = code[pc];
                    auto& entry = cp[U2(1)];
                    if (!entry.resolved) resolveMember(cls, entry, false);
                    Value* const callArgs = sp - entry.argSlots - (op == 184 ? 0 : 1);
                    Method* target = entry.method;
                    if (op == 184) {
                        if (target) initClass(target->owner);
                    } else {
                        auto* receiver = callArgs[0].a;
                        if (!receiver) NPE();
                        if (op != 183) {
                            if (receiver->cls != entry.cacheClass) {
                                entry.cacheMethod = findMethod(receiver->cls, entry.sig);
                                entry.cacheClass = receiver->cls;
                            }
                            target = entry.cacheMethod;
                        }
                    }
                    if (!target) {
                        fatal("missing method " + (op == 182 || op == 185 ? callArgs[0].a->cls->name : entry.cls->name)
                              + "." + entry.sig);
                    }
                    const Value result = invoke(target, callArgs);
                    sp = callArgs;
                    if (entry.ret == 'J') PUSHJ(result.j);
                    else if (entry.ret != 'V') *sp++ = result;
                    pc += op == 185 ? 5 : 3;
                    break;
                }
                case 187: {
                    auto* target = resolveClass(cls, U2(1));
                    initClass(target);
                    PUSHA(newInstance(target));
                    pc += 3; break;
                }
                case 188: {
                    static const char types[] = {0, 0, 0, 0, 'B', 'C', 0, 0, 'B', 'S', 'I', 'J'};
                    const char type = U1(1) < 12 ? types[U1(1)] : 0;
                    if (!type) fatal("unsupported array type");
                    sp[-1] = A(newArray(type, sp[-1].i));
                    pc += 2; break;
                }
                case 189: {
                    auto* element = resolveClass(cls, U2(1));
                    auto* type = findClass(element->name[0] == '[' ? "[" + element->name : "[L" + element->name + ";");
                    sp[-1] = A(newArray(type, sp[-1].i));
                    pc += 3; break;
                }
                case 190: {
                    auto* array = static_cast<Array*>(sp[-1].a);
                    if (!array) NPE();
                    sp[-1] = I(array->length);
                    pc += 1; break;
                }
                case 191: {
                    auto* thrown = sp[-1].a;
                    if (!thrown) NPE();
                    throw JavaThrow{thrown};
                }
                case 192: {
                    auto* object = sp[-1].a;
                    if (object && !instanceOf(object->cls, resolveClass(cls, U2(1)))) {
                        throwNew("java/lang/ClassCastException", object->cls->name);
                    }
                    pc += 3; break;
                }
                case 193: {
                    auto* object = sp[-1].a;
                    sp[-1] = I(object && instanceOf(object->cls, resolveClass(cls, U2(1))) ? 1 : 0);
                    pc += 3; break;
                }
                case 194: monitorEnter((--sp)->a); pc += 1; break;
                case 195: monitorExit((--sp)->a); pc += 1; break;
                case 196: {
                    const int op = U1(1), index = U2(2);
                    if (op == 132) {
                        locals[index] = I(static_cast<std::int32_t>(static_cast<std::uint32_t>(locals[index].i) + S2(4)));
                        pc += 6;
                    } else {
                        if (op == 21 || op == 25) *sp++ = locals[index];
                        else if (op == 22) PUSHJ(locals[index].j);
                        else if (op == 54 || op == 58) locals[index] = *--sp;
                        else if (op == 55) { sp -= 2; locals[index] = sp[0]; }
                        else fatal("unsupported wide instruction");
                        pc += 4;
                    }
                    break;
                }
                case 197: {
                    const int dims = U1(3);
                    sp -= dims;
                    auto* array = newMulti(resolveClass(cls, U2(1)), sp, dims);
                    PUSHA(array);
                    pc += 4; break;
                }
                default:
                    fatal("unsupported instruction " + std::to_string(code[pc]) + " in " + cls->name + "." + method->name);
                }
            }
        } catch (const JavaThrow& thrown) {
            const Handler* match = nullptr;
            for (const auto& handler : method->handlers) {
                if (pc >= handler.start && pc < handler.end
                    && (handler.type == 0 || instanceOf(thrown.ex->cls, resolveClass(cls, handler.type)))) {
                    match = &handler;
                    break;
                }
            }
            if (!match) {
                throw;
            }
            sp = base;
            PUSHA(thrown.ex);
            pc = match->handler;
        }
    }
}

} // namespace vm
