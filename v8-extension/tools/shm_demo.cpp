// shm-demo — reads a /dev/shm result segment written by the nativepostgress
// extension (native_query_shm) and exposes it to JS as typed arrays via
// Runtime::loadColumnarFromShm, then prints every column and a few rows.
#include <cstdio>
#include <string>

#include "v8ext/runtime.h"

using namespace v8ext;

static v8::Local<v8::String> S(v8::Isolate* iso, const char* s) {
  return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kNormal)
      .ToLocalChecked();
}

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "np_test";

  Runtime rt;
  if (!rt.init(argc, argv)) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  bool ok = true;
  {
    v8::Isolate::Scope iso_scope(rt.isolate());
    v8::HandleScope hs(rt.isolate());
    v8::Local<v8::Context> ctx = rt.context();
    v8::Context::Scope ctx_scope(ctx);

    v8::Local<v8::Object> result = rt.loadColumnarFromShm(name);
    if (result.IsEmpty()) {
      std::fprintf(stderr, "loadColumnarFromShm(\"%s\") failed\n", name);
      ok = false;
    } else {
      ctx->Global()->Set(ctx, S(rt.isolate(), "result"), result).Check();

      rt.runScript(
          "function textAt(col, r) {\n"
          "  var b = col.bytes, o = col.offsets, s = '';\n"
          "  for (var i = o[r]; i < o[r + 1]; i++) s += String.fromCharCode(b[i]);\n"
          "  return s;\n"
          "}\n"
          "var cols = result.columns, f = result.fields, n = result.rowCount;\n"
          "print('rows=' + n + ' cols=' + cols.length);\n"
          "for (var c = 0; c < cols.length; c++) {\n"
          "  var col = cols[c];\n"
          "  var sample = '';\n"
          "  if (f[c].kind === 5) {\n"
          "    for (var r = 0; r < Math.min(5, n); r++) sample += textAt(col, r) + ' ';\n"
          "  } else {\n"
          "    for (var r = 0; r < Math.min(5, n); r++) sample += col[r] + ' ';\n"
          "  }\n"
          "  print('  [' + c + '] ' + f[c].name + ' (pgtype ' + f[c].pgtype +\n"
          "        ', kind ' + f[c].kind + ') : ' + sample);\n"
          "}\n");
    }
  }

  rt.dispose();
  return ok ? 0 : 1;
}
