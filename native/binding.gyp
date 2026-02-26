{
  "targets": [
    {
      "target_name": "watermark",
      "sources": [
        "src/watermark.cc",
        "src/fft.cc",
        "src/wav.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_cc": ["-std=c++17"],
      "defines": ["NAPI_CPP_EXCEPTIONS"],
      "conditions": [
        ["OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {"ExceptionHandling": 1},
            "VCBuildConfiguration": {"PlatformToolset": "v143"}
          }
        }]
      ]
    }
  ]
}
