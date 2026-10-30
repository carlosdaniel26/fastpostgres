{
  "targets": [
    {
      "target_name": "fastpostgres",
      "sources": [
        "src/addon.c",
        "src/connection.c",
        "src/napi_util.c",
        "src/ring_client.c",
        "src/segment_read.c",
        "src/segment_write.c",
        "src/synth.c"
      ],
      "include_dirs": [
        "../pg-extension",
        "../pg-extension/src",
        "<!@(pg_config --includedir)"
      ],
      "library_dirs": ["<!@(pg_config --libdir)"],
      "libraries": ["-lpq", "-lpthread"],
      "cflags": ["-Wall", "-Wextra", "-O3"]
    }
  ]
}
