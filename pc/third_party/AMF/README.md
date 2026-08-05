# AMF SDK headers (vendored)

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/AMF
- Version: **v1.5.2** (tag, commit `eadd008`)
- Contents: `amf/public/include` only, copied verbatim into `include/`.
  No sample/common code is vendored; the runtime (`amfrt64.dll`) ships with
  the AMD Radeon driver and is loaded dynamically at run time (see
  `pc/src/amf_encoder.cpp`), so nothing is linked at build time.
- License: MIT — see `LICENSE.txt` (copied from the repository root).

To update: fetch the new tag with a sparse checkout
(`git clone --depth 1 --branch <tag> --filter=blob:none --sparse` then
`git sparse-checkout set amf/public/include`), replace `include/`, and update
the version line above.
