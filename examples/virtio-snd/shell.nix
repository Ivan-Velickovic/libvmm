# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause

let
  pkgs = import <nixpkgs> {};
in
  pkgs.mkShell {
    nativeBuildInputs =
    let
      crossInputs = with pkgs.pkgsCross.aarch64-multiplatform-musl; [
        (alsa-lib.overrideAttrs (oldAttrs: {
          configureFlags = oldAttrs.configureFlags or [] ++ [
            "--enable-shared=no"
            "--enable-static=yes"
          ];
        }))
      ];
      nativeInputs = with pkgs; [
        llvm
        dtc
        qemu
        patchelf
        clang
        lld
        gzip
        fakeroot
        cpio
        zig
        perl
      ];
    in crossInputs ++ nativeInputs;
    hardeningDisable = [ "all" ];
}

