# CIRCT Mockturtle Plugin

This is an out-of-tree CIRCT project based on `examples/circt-standalone`.
It builds:

* `circt-mockturtle-opt`, a standalone optimizer driver.
* `CIRCTMockturtlePlugin.so`, a pass plugin loadable by `circt-opt`.
* `CIRCTMockturtle`, a small pass library that depends on mockturtle.

Configure it against a CIRCT build tree. CMake fetches mockturtle from the
upstream `lsils/mockturtle` repository by default:

```sh
cmake -G Ninja -S . -B build \
  -DCIRCT_DIR=/path/to/circt-build/lib/cmake/circt \
  -DMLIR_DIR=/path/to/mlir-build/lib/cmake/mlir
```

To use an existing local mockturtle checkout instead of fetching:

```sh
cmake -G Ninja -S . -B build \
  -DCIRCT_DIR=/path/to/circt-build/lib/cmake/circt \
  -DMLIR_DIR=/path/to/mlir-build/lib/cmake/mlir \
  -DFETCHCONTENT_SOURCE_DIR_MOCKTURTLE=/path/to/mockturtle
```

Set `MOCKTURTLE_GIT_TAG` to build against a different upstream revision.

Build and test:

```sh
ninja -C build check-circt-mockturtle
```

Use the driver directly:

```sh
build/bin/circt-mockturtle-opt input.mlir --synth-mockturtle-aig-stats
```

Or load the plugin into `circt-opt`:

```sh
circt-opt input.mlir \
  --load-pass-plugin=build/lib/CIRCTMockturtlePlugin.so \
  --pass-pipeline='builtin.module(hw.module(synth-mockturtle-aig-stats))'
```
