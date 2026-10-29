#include "v8ext/runtime.h"

#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v8ext/np_shm.h"

namespace v8ext {

namespace {

std::string toStdString(v8::Isolate* iso, v8::Local<v8::Value> v) {
  v8::String::Utf8Value utf8(iso, v);
  if (*utf8 == nullptr) return "";
  return std::string(*utf8, utf8.length());
}

v8::Local<v8::String> v8str(v8::Isolate* iso, const char* s) {
  return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kNormal)
      .ToLocalChecked();
}

uint32_t rd32(const uint8_t*& p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  p += 4;
  return v;
}
uint64_t rd64(const uint8_t*& p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  p += 8;
  return v;
}

// Wrap raw little-endian bytes as a typed array (zero-copy). kind: 1=bool,
// 2=int32, 3=int64, 4=float64.
v8::Local<v8::Value> wrapTyped(v8::Isolate* iso, const uint8_t* data,
                               size_t bytes, uint8_t kind) {
  using v8::BackingStoreInitializationMode;
  auto bs = v8::ArrayBuffer::NewBackingStore(
      iso, bytes, BackingStoreInitializationMode::kUninitialized);
  std::memcpy(bs->Data(), data, bytes);
  auto ab = v8::ArrayBuffer::New(
      iso, std::shared_ptr<v8::BackingStore>(std::move(bs)));
  switch (kind) {
    case 1:
      return v8::Uint8Array::New(ab, 0, bytes);
    case 2:
      return v8::Int32Array::New(ab, 0, bytes / 4);
    case 3:
      return v8::BigInt64Array::New(ab, 0, bytes / 8);
    case 4:
      return v8::Float64Array::New(ab, 0, bytes / 8);
    default:
      return v8::Undefined(iso);
  }
}

// `print` global so demo JS can print without console.
void Print(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* iso = args.GetIsolate();
  std::string line;
  for (int i = 0; i < args.Length(); ++i) {
    if (i) line += " ";
    line += toStdString(iso, args[i]);
  }
  std::cout << line << std::endl;
}

}  // namespace

Runtime::~Runtime() { dispose(); }

bool Runtime::init(int argc, char** argv) {
  (void)argc;
  (void)argv;

  platform_ = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform_.get());
  v8::V8::Initialize();

  create_params_.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  isolate_ = v8::Isolate::New(create_params_);

  v8::Isolate::Scope iso_scope(isolate_);
  v8::HandleScope hs(isolate_);

  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate_);
  global->Set(isolate_, "print", v8::FunctionTemplate::New(isolate_, Print));

  v8::Local<v8::Context> ctx = v8::Context::New(isolate_, nullptr, global);
  context_.Reset(isolate_, ctx);
  return true;
}

bool Runtime::runScript(const std::string& src, const std::string& name) {
  if (!isolate_) return false;

  v8::Isolate::Scope iso_scope(isolate_);
  v8::HandleScope hs(isolate_);
  v8::Local<v8::Context> ctx = context_.Get(isolate_);
  v8::Context::Scope ctx_scope(ctx);

  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(isolate_, src.c_str(), v8::NewStringType::kNormal,
                              static_cast<int>(src.size()))
          .ToLocalChecked();
  v8::Local<v8::String> fname =
      v8::String::NewFromUtf8(isolate_, name.c_str(), v8::NewStringType::kNormal,
                              static_cast<int>(name.size()))
          .ToLocalChecked();
  v8::ScriptOrigin origin(fname);

  v8::TryCatch tc(isolate_);
  v8::MaybeLocal<v8::Script> script = v8::Script::Compile(ctx, source, &origin);
  if (script.IsEmpty()) {
    reportException(tc);
    return false;
  }
  v8::MaybeLocal<v8::Value> result = script.ToLocalChecked()->Run(ctx);
  if (result.IsEmpty()) {
    reportException(tc);
    return false;
  }
  return true;
}

v8::Local<v8::Object> Runtime::loadColumnarFromShm(const std::string& name) {
  v8::Local<v8::Context> ctx = context_.Get(isolate_);
  std::string path = "/" + name;

  int fd = shm_open(path.c_str(), O_RDONLY, 0);
  if (fd < 0) return v8::Local<v8::Object>();
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    ::close(fd);
    return v8::Local<v8::Object>();
  }
  size_t sz = static_cast<size_t>(st.st_size);
  uint8_t* map =
      static_cast<uint8_t*>(mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0));
  ::close(fd);
  if (map == MAP_FAILED) return v8::Local<v8::Object>();
  (void) madvise(map, sz, MADV_HUGEPAGE);

  NpShmHeader* h = reinterpret_cast<NpShmHeader*>(map);
  const uint8_t* p = map + NP_SHM_HEADER_SIZE;
  uint32_t ncols = 0;
  uint64_t nrows = 0;
  if (std::memcmp(h->magic, NP_SHM_MAGIC, 8) == 0 &&
      std::memcmp(p, "NPCOLUMN", 8) == 0) {
    p += 8;
    rd32(p); /* version */
    rd32(p); /* flags */
    ncols = rd32(p);
    nrows = rd64(p);
  } else {
    munmap(map, sz);
    return v8::Local<v8::Object>();
  }

  v8::Local<v8::Array> columns = v8::Array::New(isolate_, ncols);
  v8::Local<v8::Array> fields = v8::Array::New(isolate_, ncols);

  for (uint32_t c = 0; c < ncols; ++c) {
    uint32_t name_len = rd32(p);
    std::string colname(reinterpret_cast<const char*>(p), name_len);
    p += name_len;
    uint8_t kind = *p++;
    uint32_t pgtype = rd32(p);
    p += (nrows + 7) / 8; /* null bitmap */
    uint64_t data_len = rd64(p);
    const uint8_t* data = p;
    p += data_len;

    v8::Local<v8::Object> f = v8::Object::New(isolate_);
    f->Set(ctx, v8str(isolate_, "name"), v8str(isolate_, colname.c_str()))
        .Check();
    f->Set(ctx, v8str(isolate_, "pgtype"),
           v8::Integer::NewFromUnsigned(isolate_, pgtype))
        .Check();
    f->Set(ctx, v8str(isolate_, "kind"),
           v8::Integer::NewFromUnsigned(isolate_, kind))
        .Check();
    fields->Set(ctx, c, f).Check();

    v8::Local<v8::Value> col;
    if (kind == 5) {
      // text: [offsets int32[nrows+1]][raw bytes]
      size_t offs_bytes = static_cast<size_t>(nrows + 1) * 4;
      size_t text_bytes = static_cast<size_t>(data_len) - offs_bytes;
      v8::Local<v8::Value> bytes_v =
          wrapTyped(isolate_, data + offs_bytes, text_bytes, 1 /*Uint8*/);
      v8::Local<v8::Value> offsets_v =
          wrapTyped(isolate_, data, offs_bytes, 2 /*Int32*/);
      v8::Local<v8::Object> to = v8::Object::New(isolate_);
      to->Set(ctx, v8str(isolate_, "bytes"), bytes_v).Check();
      to->Set(ctx, v8str(isolate_, "offsets"), offsets_v).Check();
      col = to;
    } else {
      col = wrapTyped(isolate_, data, static_cast<size_t>(data_len), kind);
    }
    columns->Set(ctx, c, col).Check();
  }

  munmap(map, sz);

  v8::Local<v8::Object> out = v8::Object::New(isolate_);
  out->Set(ctx, v8str(isolate_, "rowCount"),
           v8::Number::New(isolate_, static_cast<double>(nrows)))
      .Check();
  out->Set(ctx, v8str(isolate_, "fields"), fields).Check();
  out->Set(ctx, v8str(isolate_, "columns"), columns).Check();
  return out;
}

void Runtime::reportException(v8::TryCatch& tc) {
  v8::Isolate* iso = isolate_;
  v8::Local<v8::Context> ctx = context_.Get(iso);

  std::cerr << "[v8ext] exception: " << toStdString(iso, tc.Exception())
            << "\n";

  v8::MaybeLocal<v8::Value> stack = tc.StackTrace(ctx);
  if (!stack.IsEmpty()) {
    v8::Local<v8::Value> sv = stack.ToLocalChecked();
    if (sv->IsString()) std::cerr << toStdString(iso, sv) << "\n";
  }
}

void Runtime::dispose() {
  if (!isolate_) return;

  {
    v8::Isolate::Scope iso_scope(isolate_);
    v8::HandleScope hs(isolate_);
    context_.Reset();
  }

  isolate_->Dispose();
  isolate_ = nullptr;

  delete create_params_.array_buffer_allocator;
  create_params_.array_buffer_allocator = nullptr;

  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  platform_.reset();
}

}  // namespace v8ext
