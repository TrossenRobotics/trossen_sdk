/**
 * @file lerobot_codec.cpp
 * @brief Implementation of the pinned LeRobot pickle/torch decoder.
 *
 * Layout (grown one function per step):
 *   1. ByteReader        — bounds-checked cursor over the raw bytes
 *   2. Value model       — C++ representation of unpickled Python objects
 *   3. Unpickler         — stack-machine VM over the pinned opcode subset
 *   4. torch handlers    — _rebuild_tensor_v2 / legacy storage decode
 *   5. decode_actions()  — run VM, walk list[TimedAction] into DecodedActions
 */

#include "trossen_sdk/hw/policy/lerobot_codec.hpp"

#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace trossen::hw::policy {
namespace {

// The reader/writer integer primitives memcpy raw bytes and assume the host is
// little-endian (pickle integers and the torch storage payload are LE). Fail
// the build on a big-endian host rather than silently producing garbage.
static_assert(std::endian::native == std::endian::little,
              "lerobot_codec assumes a little-endian host");

// ---------------------------------------------------------------------------
// ByteReader: a bounds-checked cursor over the raw pickle bytes.
//
// Every multi-byte read goes through memcpy (the buffer has no alignment
// guarantee) and through need() (a malformed/truncated stream must become a
// clean exception with a byte offset, never an out-of-bounds read). Pickle
// integers are little-endian; BINFLOAT alone is big-endian.
// ---------------------------------------------------------------------------
class ByteReader {
public:
  ByteReader(const uint8_t * data, std::size_t size)
  : data_(data), size_(size) {}

  uint8_t u8()
  {
    need(1);
    return data_[pos_++];
  }

  uint16_t u16le()
  {
    uint16_t v;
    std::memcpy(&v, bytes(2), 2);
    return v;
  }

  uint32_t u32le()
  {
    uint32_t v;
    std::memcpy(&v, bytes(4), 4);
    return v;
  }

  uint64_t u64le()
  {
    uint64_t v;
    std::memcpy(&v, bytes(8), 8);
    return v;
  }

  int64_t i64le()
  {
    int64_t v;
    std::memcpy(&v, bytes(8), 8);
    return v;
  }

  /// BINFLOAT: big-endian IEEE-754 double (pickle's one big-endian field).
  double f64be()
  {
    const uint8_t * p = bytes(8);
    uint8_t swapped[8];
    for (int i = 0; i < 8; ++i) {
      swapped[i] = p[7 - i];
    }
    double v;
    std::memcpy(&v, swapped, 8);
    return v;
  }

  /// Borrow @p n raw bytes (no copy); valid for the buffer's lifetime.
  const uint8_t * bytes(std::size_t n)
  {
    need(n);
    const uint8_t * p = data_ + pos_;
    pos_ += n;
    return p;
  }

  std::string str(std::size_t n)
  {
    const uint8_t * p = bytes(n);
    return std::string(reinterpret_cast<const char *>(p), n);
  }

  std::size_t pos() const {return pos_;}

  // Bytes left to read. Used to bound wire-supplied element counts against the
  // actual payload before allocating, so a malicious size cannot drive a huge
  // reservation ahead of the read that would reject it.
  std::size_t remaining() const {return size_ - pos_;}

private:
  void need(std::size_t n) const
  {
    if (size_ - pos_ < n) {
      throw std::runtime_error(
              "lerobot_codec: truncated stream (need " + std::to_string(n) +
              " bytes at offset " + std::to_string(pos_) + " of " +
              std::to_string(size_) + ")");
    }
  }

  const uint8_t * data_;
  std::size_t size_;
  std::size_t pos_{0};
};

// ---------------------------------------------------------------------------
// ByteWriter: append-only cursor building a pickle byte stream — the emit-side
// mirror of ByteReader. Raw byte primitives only (no opcode knowledge); the
// opcode emitters compose on top. Pickle integers are little-endian; BINFLOAT
// alone is big-endian, so f64be byte-swaps (same asymmetry ByteReader handles).
// ---------------------------------------------------------------------------
class ByteWriter {
public:
  void u8(uint8_t b) {buf_.push_back(b);}

  void raw(const uint8_t * p, std::size_t n) {buf_.insert(buf_.end(), p, p + n);}

  void raw(const std::string & s)
  {
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void u16le(uint16_t v)
  {
    uint8_t b[2];
    std::memcpy(b, &v, 2);  // host is little-endian (codec is LE-pinned, see header)
    raw(b, 2);
  }

  void u32le(uint32_t v)
  {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    raw(b, 4);
  }

  void u64le(uint64_t v)
  {
    uint8_t b[8];
    std::memcpy(b, &v, 8);
    raw(b, 8);
  }

  /// BINFLOAT payload: big-endian IEEE-754 double (pickle's one big-endian field).
  void f64be(double v)
  {
    uint8_t b[8];
    std::memcpy(b, &v, 8);
    uint8_t swapped[8];
    for (int i = 0; i < 8; ++i) {
      swapped[i] = b[7 - i];
    }
    raw(swapped, 8);
  }

  /// Overwrite 8 little-endian bytes at @p offset (FRAME length back-patch).
  void patch_u64le(std::size_t offset, uint64_t v)
  {
    uint8_t b[8];
    std::memcpy(b, &v, 8);
    for (std::size_t i = 0; i < 8; ++i) {
      buf_[offset + i] = b[i];
    }
  }

  std::size_t size() const {return buf_.size();}
  std::vector<uint8_t> take() {return std::move(buf_);}

private:
  std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// Opcode emit primitives: leaf emitters, the inverse of the decoder's scalar /
// string / bytes cases. Opcodes match the pinned fixture disassembly so the
// output stays inside the decoder's taught subset and reads cleanly under
// pickletools.dis. No memo is emitted (pickle.loads does not require it; see
// the encode entry points) — repeated strings are simply re-emitted.
// ---------------------------------------------------------------------------

void emit_proto(ByteWriter & w)
{
  w.u8(0x80);  // PROTO
  w.u8(4);     // protocol 4 (the pinned default; see versions.json)
}

void emit_stop(ByteWriter & w) {w.u8('.');}

void emit_none(ByteWriter & w) {w.u8('N');}

void emit_bool(ByteWriter & w, bool b) {w.u8(b ? 0x88 : 0x89);}  // NEWTRUE / NEWFALSE

/// Narrowest integer opcode for @p v, matching CPython's pickler choices.
void emit_int(ByteWriter & w, int64_t v)
{
  if (v >= 0 && v < 256) {
    w.u8('K');                                  // BININT1 (unsigned 1 byte)
    w.u8(static_cast<uint8_t>(v));
  } else if (v >= 256 && v < 65536) {
    w.u8('M');                                  // BININT2 (unsigned 2 bytes)
    w.u16le(static_cast<uint16_t>(v));
  } else if (v >= INT32_MIN && v <= INT32_MAX) {
    w.u8('J');                                  // BININT (SIGNED 4 bytes)
    w.u32le(static_cast<uint32_t>(static_cast<int32_t>(v)));
  } else {
    // LONG1: 8-byte two's-complement little-endian (covers full int64).
    w.u8(0x8a);
    w.u8(8);
    w.u64le(static_cast<uint64_t>(v));
  }
}

void emit_float(ByteWriter & w, double v)
{
  w.u8('G');     // BINFLOAT
  w.f64be(v);    // big-endian (pickle's lone big-endian field)
}

void emit_unicode(ByteWriter & w, const std::string & s)
{
  if (s.size() < 256) {
    w.u8(0x8c);                            // SHORT_BINUNICODE <1-byte len>
    w.u8(static_cast<uint8_t>(s.size()));
  } else {
    w.u8('X');                             // BINUNICODE <4-byte len>
    w.u32le(static_cast<uint32_t>(s.size()));
  }
  w.raw(s);
}

void emit_bytes(ByteWriter & w, const uint8_t * p, std::size_t n)
{
  if (n < 256) {
    w.u8('C');                             // SHORT_BINBYTES <1-byte len>
    w.u8(static_cast<uint8_t>(n));
  } else {
    w.u8('B');                             // BINBYTES <4-byte len>
    w.u32le(static_cast<uint32_t>(n));
  }
  w.raw(p, n);
}

// ---------------------------------------------------------------------------
// Structural emitters: thin opcode wrappers the encode_* entry points compose
// to spell out each pinned payload's shape inline (no general value model —
// the pinned, purpose-built stance the decode side also takes).
// ---------------------------------------------------------------------------

void emit_mark(ByteWriter & w) {w.u8('(');}
void emit_empty_dict(ByteWriter & w) {w.u8('}');}
void emit_setitems(ByteWriter & w) {w.u8('u');}      // fill dict from MARK..top
void emit_empty_list(ByteWriter & w) {w.u8(']');}
void emit_appends(ByteWriter & w) {w.u8('e');}       // list.extend(MARK..top)
void emit_empty_tuple(ByteWriter & w) {w.u8(')');}
void emit_tuple1(ByteWriter & w) {w.u8(0x85);}       // pop 1 -> 1-tuple
void emit_tuple3(ByteWriter & w) {w.u8(0x87);}       // pop 3 -> 3-tuple
void emit_tuple(ByteWriter & w) {w.u8('t');}         // collapse MARK..top -> tuple
void emit_reduce(ByteWriter & w) {w.u8('R');}        // callable(*argtuple)
void emit_build(ByteWriter & w) {w.u8('b');}         // obj.__setstate__/__dict__ update

/// STACK_GLOBAL: push <module>, push <qualname>, then resolve. The decoder's
/// 0x93 case is the exact inverse.
void emit_global(ByteWriter & w, const std::string & module, const std::string & qualname)
{
  emit_unicode(w, module);
  emit_unicode(w, qualname);
  w.u8(0x93);  // STACK_GLOBAL
}

/// Opener common to every dataclass instance: the empty NEWOBJ shell whose
/// __dict__ a following state dict + BUILD fills (see the fixture's
/// TimedObservation: STACK_GLOBAL, EMPTY_TUPLE, NEWOBJ, <dict>, BUILD).
void emit_newobj_shell(ByteWriter & w, const std::string & module, const std::string & cls)
{
  emit_global(w, module, cls);
  emit_empty_tuple(w);  // __new__ takes no args for these dataclasses
  w.u8(0x81);           // NEWOBJ
}

/// Emit a C-contiguous uint8 ndarray reproducing the SAME structure numpy 2.2.6
/// (the pin) pickles: _reconstruct shell, then BUILD with state
/// (1, shape, dtype, False, raw), where dtype is numpy.dtype('u1', False, True)
/// + BUILD. It is NOT byte-identical to numpy's own output: numpy emits a memo
/// table (MEMOIZE/BINGET) and spells the shape tuple as TUPLE3, whereas this
/// emitter writes no memo (repeats strings) and uses MARK/TUPLE; the byte
/// offsets therefore differ from the observation_basic fixture. Equivalent
/// under pickle.loads, which is all the server needs. uint8 only — RGB HWC
/// images; any other dtype is intentionally unsupported (own dtype state).
///
/// The emitted module path "numpy._core.multiarray" is numpy >= 2.0 only (the
/// numpy.core -> numpy._core rename landed in numpy 2.0), so the SERVER that
/// unpickles this must run numpy >= 2.0 — which matches the pinned stack.
void emit_numpy_uint8_array(
  ByteWriter & w, const std::vector<int64_t> & shape, const uint8_t * data, std::size_t n)
{
  std::size_t expect_n = 1;
  for (int64_t d : shape) {
    expect_n *= static_cast<std::size_t>(d);
  }
  if (n != expect_n) {
    throw std::runtime_error("lerobot_codec: numpy array data size != product(shape)");
  }

  // ① empty shell: numpy._core.multiarray._reconstruct(numpy.ndarray, (0,), b'b')
  // "numpy._core" is the numpy >= 2.0 module path (renamed from numpy.core in
  // 2.0); the unpickling server must run numpy >= 2.0.
  emit_global(w, "numpy._core.multiarray", "_reconstruct");
  emit_global(w, "numpy", "ndarray");
  emit_int(w, 0);
  emit_tuple1(w);                       // (0,)
  const uint8_t b_char = 'b';
  emit_bytes(w, &b_char, 1);            // b'b' (base dtype-kind hint)
  emit_tuple3(w);                       // (ndarray, (0,), b'b')
  emit_reduce(w);                       // -> empty ndarray

  // ② BUILD state tuple: (version=1, shape, dtype, fortran_order=False, raw)
  emit_mark(w);
  emit_int(w, 1);                       // ndarray pickle version
  emit_mark(w);                         // shape tuple (any rank)
  for (int64_t d : shape) {
    emit_int(w, d);
  }
  emit_tuple(w);

  // dtype = numpy.dtype('u1', False, True), then BUILD with the u1 state.
  emit_global(w, "numpy", "dtype");
  emit_unicode(w, "u1");
  emit_bool(w, false);                  // align
  emit_bool(w, true);                   // copy
  emit_tuple3(w);
  emit_reduce(w);
  emit_mark(w);                         // dtype __setstate__ tuple (verbatim u1 constants)
  emit_int(w, 3);                       // dtype pickle version
  emit_unicode(w, "|");                 // byteorder: not-applicable (1 byte wide)
  emit_none(w);                         // subarray
  emit_none(w);                         // names
  emit_none(w);                         // fields
  emit_int(w, -1);                      // elsize: default
  emit_int(w, -1);                      // alignment: default
  emit_int(w, 0);                       // flags
  emit_tuple(w);
  emit_build(w);                        // dtype.__setstate__(...)

  emit_bool(w, false);                  // fortran_order
  emit_bytes(w, data, n);               // raw little-endian payload
  emit_tuple(w);                        // collapse -> 5-tuple
  emit_build(w);                        // ndarray.__setstate__(...)
}

// ---------------------------------------------------------------------------
// Value model: one Value is one unpickled Python object.
//
// Containers hold shared_ptr<Value> (ValuePtr) for two reasons:
//  - recursion: a list holds values which may be lists (a type cannot contain
//    itself by value);
//  - aliasing: pickle's memo table re-pushes the SAME object (BINGET), and all
//    tensors in a chunk share ONE Storage. Copies would silently break both.
// ---------------------------------------------------------------------------
struct Value;
using ValuePtr = std::shared_ptr<Value>;

/// A resolved module.qualname callable, e.g. "torch._utils._rebuild_tensor_v2".
/// The VM never executes these; a whitelist handler interprets them.
struct Global
{
  std::string name;
};

/// Flat float32 buffer decoded from the nested torch blob.
struct Storage
{
  std::vector<float> data;
};
using StoragePtr = std::shared_ptr<Storage>;  ///< shared: tensors are views

/// What _rebuild_tensor_v2 produces: a VIEW into a Storage (no float copied).
/// Element (i) of a 1-D tensor lives at storage->data[offset + i * stride[0]].
struct Tensor
{
  StoragePtr storage;
  int64_t offset{0};
  std::vector<int64_t> shape;
  std::vector<int64_t> stride;
};

/// A class instance (NEWOBJ + BUILD), e.g. one TimedAction: the class name it
/// was constructed from plus its attribute dict. No behavior, just state.
struct Object
{
  std::string cls;
  std::map<std::string, ValuePtr> attrs;
};

using List = std::vector<ValuePtr>;            ///< list and tuple alike
using Dict = std::map<std::string, ValuePtr>;  ///< string-keyed dicts only

struct Value
{
  std::variant<
    std::monostate,            // None
    bool,
    int64_t,
    double,
    std::string,
    std::vector<uint8_t>,      // bytes
    List,
    Dict,
    Global,
    StoragePtr,
    Tensor,
    Object> v;
};

/// Wrap any alternative into a heap-allocated Value.
template<typename T>
ValuePtr make_value(T && x)
{
  auto p = std::make_shared<Value>();
  p->v = std::forward<T>(x);
  return p;
}

/// std::get with a codec-flavored error: names what was expected instead of
/// throwing an opaque std::bad_variant_access. The fail-loud workhorse.
template<typename T>
const T & expect(const ValuePtr & p, const char * what)
{
  if (!p || !std::holds_alternative<T>(p->v)) {
    throw std::runtime_error(std::string("lerobot_codec: expected ") + what);
  }
  return std::get<T>(p->v);
}

// ---------------------------------------------------------------------------
// Unpickler: a stack-machine VM over the pinned pickle opcode subset.
//
// One instance executes ONE pickle program: run() fetches opcodes until STOP
// and returns the single remaining stack value. The nested torch blob holds
// several consecutive programs in one buffer, so callers re-run a fresh
// Unpickler over the same ByteReader for each (fresh memo per program,
// matching Python's behavior).
//
// Any opcode outside the taught subset throws, naming the byte and offset.
// ---------------------------------------------------------------------------
class Unpickler {
public:
  /// REDUCE delegate: apply a named global to its argument tuple.
  using GlobalHandler = std::function<ValuePtr (const Global &, const List &)>;
  /// BINPERSID delegate: resolve a persistent-id tuple (torch storage refs).
  using PersistentHandler = std::function<ValuePtr (const ValuePtr &)>;

  explicit Unpickler(ByteReader & r)
  : r_(r) {}

  void set_global_handler(GlobalHandler h) {global_handler_ = std::move(h);}
  void set_persistent_handler(PersistentHandler h) {persistent_handler_ = std::move(h);}

  ValuePtr run()
  {
    while (true) {
      const std::size_t op_pos = r_.pos();  // remembered for error messages
      const uint8_t op = r_.u8();           // FETCH
      switch (op) {                         // DECODE + EXECUTE
        // --- group 1: framing -----------------------------------------------
        case 0x80: {  // PROTO <1-byte version>
            const uint8_t proto = r_.u8();
            if (proto < 2 || proto > 4) {
              fail(op_pos, "PROTO", "unsupported pickle protocol " + std::to_string(proto));
            }
            break;
          }
        case 0x95:  // FRAME <8-byte length>: an I/O hint only; content follows inline.
          r_.u64le();
          break;
        case '.': {  // STOP: the program's result is the single remaining value.
            if (stack_.size() != 1) {
              fail(op_pos, "STOP", "stack has " + std::to_string(stack_.size()) + " values");
            }
            return stack_.back();
          }

        // --- group 1: scalars -------------------------------------------------
        case 'N': push(std::monostate{}); break;  // None
        case 0x88: push(true); break;             // NEWTRUE
        case 0x89: push(false); break;            // NEWFALSE
        case 'K': push(static_cast<int64_t>(r_.u8())); break;     // BININT1
        case 'M': push(static_cast<int64_t>(r_.u16le())); break;  // BININT2
        case 'J': {  // BININT: signed 32-bit little-endian
            const uint32_t raw = r_.u32le();
            int32_t v;
            std::memcpy(&v, &raw, 4);
            push(static_cast<int64_t>(v));
            break;
          }
        case 0x8a: {  // LONG1 <1-byte n> <n bytes, two's-complement little-endian>
            const uint8_t n = r_.u8();
            const uint8_t * p = r_.bytes(n);
            if (n <= 8) {
              uint64_t u = 0;
              for (int i = n - 1; i >= 0; --i) {
                u = (u << 8) | p[i];
              }
              // Sign-extend from bit (8n - 1).
              if (n > 0 && n < 8 && (p[n - 1] & 0x80)) {
                u |= ~uint64_t{0} << (8 * n);
              }
              push(static_cast<int64_t>(u));
            } else {
              // Wider than int64 (torch's 10-byte legacy magic number): keep raw
              // little-endian bytes; callers only ever equality-check it.
              push(std::vector<uint8_t>(p, p + n));
            }
            break;
          }
        case 'G': push(r_.f64be()); break;  // BINFLOAT (big-endian, unlike the ints)

        // --- group 2: containers (tuples are modeled as List) -----------------
        case ']': push(List{}); break;   // EMPTY_LIST
        case '}': push(Dict{}); break;   // EMPTY_DICT
        case ')': push(List{}); break;   // EMPTY_TUPLE
        case 0x85: push(take_tuple(op_pos, 1)); break;  // TUPLE1
        case 0x86: push(take_tuple(op_pos, 2)); break;  // TUPLE2
        case 0x87: push(take_tuple(op_pos, 3)); break;  // TUPLE3
        case '(': marks_.push_back(stack_.size()); break;  // MARK
        case 't': {  // TUPLE: everything since the last MARK
            List items = pop_to_mark(op_pos);
            push(std::move(items));
            break;
          }
        case 'a': {  // APPEND: list.append(top)
            ValuePtr item = pop(op_pos);
            as_list(op_pos, "APPEND").push_back(std::move(item));
            break;
          }
        case 'e': {  // APPENDS: list.extend(items since MARK)
            List items = pop_to_mark(op_pos);
            List & dst = as_list(op_pos, "APPENDS");
            dst.insert(dst.end(), items.begin(), items.end());
            break;
          }
        case 's': {  // SETITEM: dict[key] = value
            ValuePtr value = pop(op_pos);
            ValuePtr key = pop(op_pos);
            set_item(op_pos, "SETITEM", key, std::move(value));
            break;
          }
        case 'u': {  // SETITEMS: alternating key/value pairs since MARK
            List items = pop_to_mark(op_pos);
            if (items.size() % 2 != 0) {
              fail(op_pos, "SETITEMS", "odd number of stack items");
            }
            for (std::size_t i = 0; i < items.size(); i += 2) {
              set_item(op_pos, "SETITEMS", items[i], items[i + 1]);
            }
            break;
          }

        // --- group 5: globals, objects, calls ----------------------------------
        case 0x93: {  // STACK_GLOBAL: pop <qualname>, pop <module>
            const std::string name = expect_str(pop(op_pos), op_pos, "STACK_GLOBAL");
            const std::string module = expect_str(pop(op_pos), op_pos, "STACK_GLOBAL");
            push(Global{module + "." + name});
            break;
          }
        case 'c': {  // GLOBAL: two newline-terminated names (protocol-2 form)
            const std::string module = read_line();
            const std::string name = read_line();
            push(Global{module + "." + name});
            break;
          }
        case 'R': {  // REDUCE: callable(*args) — routed to the whitelist hook
            ValuePtr args = pop(op_pos);
            ValuePtr callable = pop(op_pos);
            push_ptr(call_global(op_pos, "REDUCE", callable, args));
            break;
          }
        case 0x81: {  // NEWOBJ: cls.__new__(cls, *args) — empty instance shell
            ValuePtr args = pop(op_pos);
            ValuePtr cls = pop(op_pos);
            if (!std::holds_alternative<Global>(cls->v)) {
              fail(op_pos, "NEWOBJ", "class is not a global");
            }
            if (!expect<List>(args, "NEWOBJ args tuple").empty()) {
              fail(op_pos, "NEWOBJ", "non-empty constructor args unsupported");
            }
            push(Object{std::get<Global>(cls->v).name, {}});
            break;
          }
        case 'b': {  // BUILD: obj.__dict__.update(state)
            ValuePtr state = pop(op_pos);
            ValuePtr obj = pop(op_pos);
            if (!std::holds_alternative<Object>(obj->v)) {
              fail(op_pos, "BUILD", "target is not an object instance");
            }
            for (const auto & [k, v] : expect<Dict>(state, "BUILD state dict")) {
              std::get<Object>(obj->v).attrs[k] = v;
            }
            push_ptr(std::move(obj));
            break;
          }
        case 'Q': {  // BINPERSID: persistent_load(pid) — torch storage indirection
            ValuePtr pid = pop(op_pos);
            if (!persistent_handler_) {
              fail(op_pos, "BINPERSID", "no persistent-id handler installed");
            }
            push_ptr(persistent_handler_(pid));
            break;
          }

        // --- group 4: strings & bytes (length-prefixed raw reads) -------------
        // Unicode opcodes: pinned payloads are pure-ASCII identifiers, so the
        // UTF-8 bytes are stored as-is (no decode step to get wrong).
        case 0x8c: {  // SHORT_BINUNICODE <1-byte len>
            const uint8_t n = r_.u8();
            push(r_.str(n));
            break;
          }
        case 'X': {  // BINUNICODE <4-byte len> (protocol-2 form, in the torch blob)
            const uint32_t n = r_.u32le();
            push(r_.str(n));
            break;
          }
        case 'B': {  // BINBYTES <4-byte len>: owned copy (the nested torch blob)
            const uint32_t n = r_.u32le();
            const uint8_t * p = r_.bytes(n);
            push(std::vector<uint8_t>(p, p + n));
            break;
          }

        // --- group 3: memo (object cache; entries alias, never copy) ----------
        case 0x94: memo_put(memo_.size(), op_pos, "MEMOIZE"); break;  // next free index
        case 'q': memo_put(r_.u8(), op_pos, "BINPUT"); break;         // explicit index
        case 'r': memo_put(r_.u32le(), op_pos, "LONG_BINPUT"); break;
        case 'h': memo_get(r_.u8(), op_pos, "BINGET"); break;
        case 'j': memo_get(r_.u32le(), op_pos, "LONG_BINGET"); break;

        default:
          fail(op_pos, "?", "unknown opcode 0x" + to_hex(op));
      }
    }
  }

private:
  // --- stack primitives -----------------------------------------------------
  template<typename T>
  void push(T && v) {stack_.push_back(make_value(std::forward<T>(v)));}
  void push_ptr(ValuePtr p) {stack_.push_back(std::move(p));}

  ValuePtr pop(std::size_t op_pos)
  {
    if (stack_.empty()) {
      fail(op_pos, "?", "pop from empty stack");
    }
    ValuePtr v = std::move(stack_.back());
    stack_.pop_back();
    return v;
  }

  // --- container helpers --------------------------------------------------------
  /// Pop and return everything pushed since the most recent MARK.
  List pop_to_mark(std::size_t op_pos)
  {
    if (marks_.empty()) {
      fail(op_pos, "?", "no MARK on stack");
    }
    const std::size_t base = marks_.back();
    marks_.pop_back();
    List items(stack_.begin() + static_cast<std::ptrdiff_t>(base), stack_.end());
    stack_.resize(base);
    return items;
  }

  /// Pop exactly n values into a tuple (TUPLE1/2/3 — no MARK involved).
  List take_tuple(std::size_t op_pos, std::size_t n)
  {
    if (stack_.size() < n) {
      fail(op_pos, "TUPLEn", "stack underflow");
    }
    List items(stack_.end() - static_cast<std::ptrdiff_t>(n), stack_.end());
    stack_.resize(stack_.size() - n);
    return items;
  }

  /// The list APPEND/APPENDS mutate sits just below the popped items.
  List & as_list(std::size_t op_pos, const char * tag)
  {
    if (stack_.empty() || !std::holds_alternative<List>(stack_.back()->v)) {
      fail(op_pos, tag, "no list under items");
    }
    return std::get<List>(stack_.back()->v);
  }

  /// dict[key] = value on the dict at top-of-stack; string keys only.
  void set_item(std::size_t op_pos, const char * tag, const ValuePtr & key, ValuePtr value)
  {
    if (stack_.empty() || !std::holds_alternative<Dict>(stack_.back()->v)) {
      fail(op_pos, tag, "no dict under items");
    }
    if (!std::holds_alternative<std::string>(key->v)) {
      fail(op_pos, tag, "non-string dict key");
    }
    std::get<Dict>(stack_.back()->v)[std::get<std::string>(key->v)] = std::move(value);
  }

  // --- memo helpers -------------------------------------------------------------
  /// Cache top-of-stack at @p idx (peek, not pop — caching is a side effect).
  void memo_put(std::size_t idx, std::size_t op_pos, const char * tag)
  {
    if (stack_.empty()) {
      fail(op_pos, tag, "empty stack");
    }
    // The memo index is wire-supplied (BINPUT/LONG_BINPUT carry it directly), so
    // a hostile stream could request a 4-billion index and force a multi-GB
    // resize. Every memoized value costs at least one opcode byte to produce, so
    // a valid index can never exceed the number of bytes consumed so far. Bound
    // it by that to keep the memo allocation proportional to the input length.
    if (idx > r_.pos()) {
      fail(op_pos, tag, "memo index out of range");
    }
    if (idx >= memo_.size()) {
      memo_.resize(idx + 1);
    }
    memo_[idx] = stack_.back();  // shares the object — BINGET must alias
  }

  void memo_get(std::size_t idx, std::size_t op_pos, const char * tag)
  {
    if (idx >= memo_.size() || !memo_[idx]) {
      fail(op_pos, tag, "memo index " + std::to_string(idx) + " unset");
    }
    push_ptr(memo_[idx]);
  }

  // --- call helpers ---------------------------------------------------------------
  ValuePtr call_global(
    std::size_t op_pos, const char * tag, const ValuePtr & callable, const ValuePtr & args)
  {
    if (!std::holds_alternative<Global>(callable->v)) {
      fail(op_pos, tag, "callable is not a global");
    }
    const std::string & name = std::get<Global>(callable->v).name;
    if (!global_handler_) {
      fail(op_pos, tag, "no handler for global '" + name + "'");
    }
    return global_handler_(std::get<Global>(callable->v), expect<List>(args, "call args tuple"));
  }

  std::string expect_str(const ValuePtr & p, std::size_t op_pos, const char * tag)
  {
    if (!std::holds_alternative<std::string>(p->v)) {
      fail(op_pos, tag, "expected string");
    }
    return std::get<std::string>(p->v);
  }

  /// For the GLOBAL opcode: bytes up to (not including) the next '\n'.
  std::string read_line()
  {
    std::string s;
    for (uint8_t c = r_.u8(); c != '\n'; c = r_.u8()) {
      s.push_back(static_cast<char>(c));
    }
    return s;
  }

  // --- error reporting --------------------------------------------------------
  static std::string to_hex(uint8_t b)
  {
    static constexpr char digits[] = "0123456789abcdef";
    return std::string{digits[b >> 4], digits[b & 0xf]};
  }

  [[noreturn]] void fail(std::size_t op_pos, const char * tag, const std::string & why) const
  {
    throw std::runtime_error(
            "lerobot_codec: opcode " + std::string(tag) + " at byte " +
            std::to_string(op_pos) + ": " + why);
  }

  // --- machine state ----------------------------------------------------------
  ByteReader & r_;                   ///< instruction pointer (shared with caller)
  std::vector<ValuePtr> stack_;      ///< the workspace
  std::vector<std::size_t> marks_;   ///< saved stack depths, one per open MARK
  std::vector<ValuePtr> memo_;       ///< object cache; entries alias, never copy
  GlobalHandler global_handler_;     ///< empty by default: every REDUCE throws
  PersistentHandler persistent_handler_;
};

// ---------------------------------------------------------------------------
// torch handlers
// ---------------------------------------------------------------------------

/// The meaning of torch.storage._load_from_bytes(blob): parse torch's LEGACY
/// (pre-ZIP) torch.save layout — four consecutive protocol-2 pickle programs
/// (magic, version, sysinfo, storage descriptor, key list) followed by the raw
/// float32 payload. Every section is verified, not skipped: a format drift
/// must throw, never mis-read floats.
StoragePtr load_storage_from_bytes(const std::vector<uint8_t> & blob)
{
  ByteReader r(blob.data(), blob.size());

  // ① magic number: 0x1950a86a20f9469cfc6c as a 10-byte LONG1 (kept as raw
  // little-endian bytes by the VM since it exceeds int64).
  static constexpr uint8_t kMagic[10] =
  {0x6c, 0xfc, 0x9c, 0x46, 0xf9, 0x20, 0x6a, 0xa8, 0x50, 0x19};
  {
    const auto magic =
      expect<std::vector<uint8_t>>(Unpickler(r).run(), "legacy torch magic (10-byte long)");
    if (magic.size() != sizeof(kMagic) || std::memcmp(magic.data(), kMagic, sizeof(kMagic)) != 0) {
      throw std::runtime_error("lerobot_codec: torch legacy magic number mismatch");
    }
  }

  // ② serialization protocol version.
  if (const int64_t v = expect<int64_t>(Unpickler(r).run(), "torch protocol version"); v != 1001) {
    throw std::runtime_error(
            "lerobot_codec: unsupported torch legacy protocol version " + std::to_string(v));
  }

  // ③ sysinfo: the writer's byte order. Raw float reads below assume
  // little-endian; big-endian data would decode to plausible garbage.
  {
    const ValuePtr sysinfo_v = Unpickler(r).run();  // named: keeps the Value alive
    const Dict & sysinfo = expect<Dict>(sysinfo_v, "torch sysinfo dict");
    const auto it = sysinfo.find("little_endian");
    if (it == sysinfo.end() || !expect<bool>(it->second, "sysinfo little_endian")) {
      throw std::runtime_error("lerobot_codec: torch payload is not little-endian");
    }
  }

  // ④ storage descriptor: ends in BINPERSID with the persistent-id tuple
  // ('storage', <type global>, <key>, <location>, <numel>). The lambda hook
  // validates it and captures key/numel into our locals.
  std::string key;
  int64_t numel = -1;
  {
    Unpickler vm(r);
    vm.set_persistent_handler(
      [&](const ValuePtr & pid) -> ValuePtr {
        const List & t = expect<List>(pid, "persistent-id tuple");
        if (t.size() < 5 || expect<std::string>(t[0], "persistent-id tag") != "storage") {
          throw std::runtime_error("lerobot_codec: unexpected persistent-id (want 'storage')");
        }
        // float32 pin: any other storage type (Double, Long, Half...) throws.
        const std::string & type = expect<Global>(t[1], "storage type global").name;
        if (type != "torch.FloatStorage") {
          throw std::runtime_error("lerobot_codec: unsupported storage type '" + type + "'");
        }
        key = expect<std::string>(t[2], "storage key");
        numel = expect<int64_t>(t[4], "storage numel");
        return make_value(std::string(key));  // placeholder result for the stack
      });
    vm.run();
  }
  if (numel < 0) {
    throw std::runtime_error("lerobot_codec: storage descriptor carried no persistent id");
  }

  // ⑤ key list: data-block order in the raw section. Pinned payloads carry
  // exactly one storage per blob; anything else is an untested layout.
  {
    const ValuePtr keys_v = Unpickler(r).run();  // named: keeps the Value alive
    const List & keys = expect<List>(keys_v, "torch storage key list");
    if (keys.size() != 1 || expect<std::string>(keys[0], "storage key") != key) {
      throw std::runtime_error("lerobot_codec: expected exactly one storage key");
    }
  }

  // ⑥ raw payload: int64 element count, then count * 4 bytes of float32.
  const int64_t count = r.i64le();
  if (count != numel) {
    throw std::runtime_error(
            "lerobot_codec: storage count mismatch (descriptor " + std::to_string(numel) +
            ", payload " + std::to_string(count) + ")");
  }
  // Bound the element count against the bytes actually left in the stream
  // BEFORE allocating: a float32 storage cannot hold more elements than
  // remaining()/4. This makes the reservation provably bounded by the payload,
  // so a hostile descriptor (e.g. numel = INT64_MAX) fails here instead of
  // attempting a multi-gigabyte allocation. Also rejects the count*4 overflow.
  if (count < 0 ||
      static_cast<uint64_t>(count) > r.remaining() / 4) {
    throw std::runtime_error(
            "lerobot_codec: storage element count " + std::to_string(count) +
            " exceeds the bytes remaining in the stream (" +
            std::to_string(r.remaining()) + ")");
  }
  const std::size_t byte_count = static_cast<std::size_t>(count) * 4;
  auto storage = std::make_shared<Storage>();
  storage->data.resize(static_cast<std::size_t>(count));
  std::memcpy(storage->data.data(), r.bytes(byte_count), byte_count);
  return storage;
}

/// Unpickle an int tuple (e.g. a shape "(14,)") into a plain vector.
std::vector<int64_t> int_vector(const ValuePtr & p, const char * what)
{
  std::vector<int64_t> out;
  for (const ValuePtr & d : expect<List>(p, what)) {
    out.push_back(expect<int64_t>(d, what));
  }
  return out;
}

/// The meaning of torch._utils._rebuild_tensor_v2(storage, storage_offset,
/// size, stride, requires_grad, backward_hooks): a Tensor VIEW over the shared
/// storage — no float copied. Trailing args are training-time artifacts and
/// are deliberately ignored (>= 4 keeps a harmless upstream addition working).
Tensor rebuild_tensor_v2(const List & args)
{
  if (args.size() < 4) {
    throw std::runtime_error(
            "lerobot_codec: _rebuild_tensor_v2 expects >= 4 args, got " +
            std::to_string(args.size()));
  }
  Tensor t;
  t.storage = expect<StoragePtr>(args[0], "_rebuild_tensor_v2 storage");
  t.offset = expect<int64_t>(args[1], "_rebuild_tensor_v2 storage_offset");
  t.shape = int_vector(args[2], "_rebuild_tensor_v2 size");
  t.stride = int_vector(args[3], "_rebuild_tensor_v2 stride");
  return t;
}

/// The REDUCE whitelist: the only three callables a pinned payload names.
/// Anything else — a new torch helper, a tampered stream — is refused by name.
ValuePtr torch_global_handler(const Global & g, const List & args)
{
  if (g.name == "torch._utils._rebuild_tensor_v2") {
    return make_value(rebuild_tensor_v2(args));
  }
  if (g.name == "torch.storage._load_from_bytes") {
    if (args.size() != 1) {
      throw std::runtime_error("lerobot_codec: _load_from_bytes expects 1 arg");
    }
    return make_value(
      StoragePtr(load_storage_from_bytes(
        expect<std::vector<uint8_t>>(args[0], "_load_from_bytes blob"))));
  }
  if (g.name == "collections.OrderedDict") {
    return make_value(Dict{});  // empty backward_hooks; never read, must exist
  }
  throw std::runtime_error("lerobot_codec: refusing to call global '" + g.name + "'");
}

}  // namespace

namespace {

// --- pickle framing -----------------------------------------------------------
// A protocol-4 stream is PROTO 4, FRAME <len>, <body>, STOP. The frame length
// is the byte count after its own 8-byte field through STOP (inclusive), so it
// is back-patched once the body is built.

/// Emit PROTO + FRAME placeholder; return the offset of the length field.
std::size_t begin_pickle(ByteWriter & w)
{
  emit_proto(w);
  w.u8(0x95);                 // FRAME
  const std::size_t off = w.size();
  w.u64le(0);                 // placeholder, patched by end_pickle
  return off;
}

void end_pickle(ByteWriter & w, std::size_t frame_len_off)
{
  emit_stop(w);
  w.patch_u64le(frame_len_off, static_cast<uint64_t>(w.size() - (frame_len_off + 8)));
}

/// Emit a tuple of ints as MARK..items..TUPLE (any rank).
void emit_int_tuple(ByteWriter & w, const std::vector<int64_t> & dims)
{
  emit_mark(w);
  for (int64_t d : dims) {
    emit_int(w, d);
  }
  emit_tuple(w);
}

/// Emit a Python ``list[str]``: EMPTY_LIST MARK <items> APPENDS.
void emit_string_list(ByteWriter & w, const std::vector<std::string> & items)
{
  emit_empty_list(w);
  emit_mark(w);
  for (const auto & s : items) {
    emit_unicode(w, s);
  }
  emit_appends(w);
}

/// One LeRobot *dataset* feature dict: {"dtype": str, "shape": tuple,
/// "names": [str, ...]}. This is exactly what the async_inference server reads
/// in build_dataset_frame (ft["dtype"]/["shape"]/["names"]) — it assembles
/// observation.state by gathering values[name] for name in names — so this is
/// a plain dict, NOT a PolicyFeature dataclass.
void emit_dataset_feature(ByteWriter & w, const LerobotFeature & f)
{
  emit_empty_dict(w);
  emit_mark(w);
  emit_unicode(w, "dtype");
  emit_unicode(w, f.dtype);
  emit_unicode(w, "shape");
  emit_int_tuple(w, f.shape);
  emit_unicode(w, "names");
  emit_string_list(w, f.names);
  emit_setitems(w);
}

}  // namespace

std::vector<uint8_t> encode_policy_setup(const LerobotPolicyConfig & cfg)
{
  ByteWriter w;
  const std::size_t frame = begin_pickle(w);

  emit_newobj_shell(w, "lerobot.async_inference.helpers", "RemotePolicyConfig");
  emit_empty_dict(w);
  emit_mark(w);                              // RemotePolicyConfig.__dict__ items

  emit_unicode(w, "policy_type");
  emit_unicode(w, cfg.policy_type);

  emit_unicode(w, "pretrained_name_or_path");
  emit_unicode(w, cfg.pretrained_name_or_path);

  emit_unicode(w, "lerobot_features");       // dict[str, dataset-feature dict]
  emit_empty_dict(w);
  emit_mark(w);
  for (const auto & [key, feat] : cfg.lerobot_features) {
    emit_unicode(w, key);
    emit_dataset_feature(w, feat);
  }
  emit_setitems(w);

  emit_unicode(w, "actions_per_chunk");
  emit_int(w, cfg.actions_per_chunk);

  emit_unicode(w, "device");
  emit_unicode(w, cfg.device);

  emit_unicode(w, "rename_map");             // dict[str, str]
  emit_empty_dict(w);
  emit_mark(w);
  for (const auto & [key, val] : cfg.rename_map) {
    emit_unicode(w, key);
    emit_unicode(w, val);
  }
  emit_setitems(w);

  emit_setitems(w);                          // close RemotePolicyConfig.__dict__
  emit_build(w);

  end_pickle(w, frame);
  return w.take();
}

std::vector<uint8_t> encode_observation(const LerobotObservation & obs)
{
  ByteWriter w;
  const std::size_t frame = begin_pickle(w);

  emit_newobj_shell(w, "lerobot.async_inference.helpers", "TimedObservation");
  emit_empty_dict(w);
  emit_mark(w);                              // TimedObservation.__dict__ items

  emit_unicode(w, "timestamp");
  emit_float(w, obs.timestamp);

  emit_unicode(w, "timestep");
  emit_int(w, obs.timestep);

  emit_unicode(w, "observation");            // the raw robot observation dict
  emit_empty_dict(w);
  emit_mark(w);
  for (const auto & [key, value] : obs.state) {
    emit_unicode(w, key);
    emit_float(w, value);                    // per-motor float (server -> torch.tensor)
  }
  for (const auto & img : obs.images) {
    emit_unicode(w, img.key);
    emit_numpy_uint8_array(w, img.shape, img.data.data(), img.data.size());
  }
  emit_unicode(w, "task");
  emit_unicode(w, obs.task);
  emit_setitems(w);                          // close observation dict

  emit_unicode(w, "must_go");
  emit_bool(w, obs.must_go);

  emit_setitems(w);                          // close TimedObservation.__dict__
  emit_build(w);

  end_pickle(w, frame);
  return w.take();
}

DecodedActions decode_actions(const uint8_t * data, std::size_t size)
{
  DecodedActions out;
  if (size == 0) {
    return out;  // empty reply: "nothing this poll" (T == 0)
  }

  ByteReader reader(data, size);
  Unpickler vm(reader);
  vm.set_global_handler(torch_global_handler);
  const ValuePtr root = vm.run();  // named: owns everything we reference below

  const List & actions = expect<List>(root, "list[TimedAction] reply");
  if (actions.empty()) {
    return out;
  }

  for (std::size_t row = 0; row < actions.size(); ++row) {
    const Object & ta = expect<Object>(actions[row], "TimedAction object");
    if (ta.cls != "lerobot.async_inference.helpers.TimedAction") {
      throw std::runtime_error("lerobot_codec: unexpected list element class '" + ta.cls + "'");
    }
    const auto ts_it = ta.attrs.find("timestep");
    const auto act_it = ta.attrs.find("action");
    if (ts_it == ta.attrs.end() || act_it == ta.attrs.end()) {
      throw std::runtime_error("lerobot_codec: TimedAction missing timestep/action attr");
    }
    const int64_t timestep = expect<int64_t>(ts_it->second, "TimedAction timestep");
    const Tensor & t = expect<Tensor>(act_it->second, "TimedAction action tensor");

    // The pinned action tensor is 1-D with positive stride.
    if (t.shape.size() != 1 || t.stride.size() != 1 || t.stride[0] < 1) {
      throw std::runtime_error("lerobot_codec: action tensor is not 1-D with stride >= 1");
    }
    const int64_t n = t.shape[0];
    // Reject a lying offset/shape before any element is read.
    const int64_t last = t.offset + (n - 1) * t.stride[0];
    if (n < 1 || t.offset < 0 || last >= static_cast<int64_t>(t.storage->data.size())) {
      throw std::runtime_error("lerobot_codec: tensor view exceeds its storage");
    }

    if (row == 0) {
      out.base_timestep = timestep;
      out.N = static_cast<int>(n);
      out.data.reserve(actions.size() * static_cast<std::size_t>(n));
    } else {
      if (n != out.N) {
        throw std::runtime_error("lerobot_codec: inconsistent action width across rows");
      }
      // Downstream alignment plays row i at base_timestep + i; a gap would
      // silently shift actions in time, so consecutive stamps are required.
      if (timestep != out.base_timestep + static_cast<int64_t>(row)) {
        throw std::runtime_error("lerobot_codec: non-consecutive TimedAction timesteps");
      }
    }

    for (int64_t i = 0; i < n; ++i) {  // the view gather (offset + stride)
      out.data.push_back(t.storage->data[static_cast<std::size_t>(t.offset + i * t.stride[0])]);
    }
  }

  out.T = static_cast<int>(actions.size());
  return out;
}

}  // namespace trossen::hw::policy
