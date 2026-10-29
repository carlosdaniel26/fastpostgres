// v8ext — the nativepostgress client: embeds V8 and decodes a columnar result
// segment (/dev/shm) into JS typed arrays. See README.md.
#pragma once

#include <memory>
#include <string>

#include <libplatform/libplatform.h>
#include <v8.h>

#include "v8ext/np_shm.h"

namespace v8ext {

class Runtime {
 public:
  Runtime() = default;
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Initialize the V8 platform, isolate and a fresh context.
  bool init(int argc = 0, char** argv = nullptr);

  // Run a JS source string on the isolate thread.
  bool runScript(const std::string& src, const std::string& name = "<script>");

  void dispose();

  // Read a /dev/shm result segment written by the nativepostgress extension
  // (native_query_shm) and decode it into a JS object:
  //   { rowCount, fields: [{name, pgtype, kind}, ...], columns: [...] }
  // Numeric/bool columns become typed arrays (Int32Array / BigInt64Array /
  // Float64Array / Uint8Array); text columns become { bytes, offsets }.
  v8::Local<v8::Object> loadColumnarFromShm(const std::string& name);

  // Accessors so a tool can drive the isolate directly.
  v8::Isolate* isolate() const { return isolate_; }
  v8::Local<v8::Context> context() const { return context_.Get(isolate_); }

 private:
  void reportException(v8::TryCatch& tc);

  std::unique_ptr<v8::Platform> platform_;
  v8::Isolate::CreateParams create_params_;
  v8::Isolate* isolate_ = nullptr;
  v8::Global<v8::Context> context_;
};

}  // namespace v8ext
