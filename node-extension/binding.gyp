{
  "targets": [
    {
      "target_name": "nativepostgress",
      "sources": ["src/addon.c", "src/synth.c"],
      "include_dirs": [
        "../pg-extension",
        "<!@(pg_config --includedir)"
      ],
      "library_dirs": ["<!@(pg_config --libdir)"],
      "libraries": ["-lpq"],
      "cflags": ["-Wall", "-Wextra"]
    }
  ]
}
