TSUKI_ROOT ?=

.PHONY: build ronin-module deploy-tsuki-module rollback-tsuki-module test clean

# Builds the whole tree under pkgs.pkgsi686Linux.stdenv (see
# nix-modules/default.nix) -- a genuine i686 toolchain, not -m32
# cross-compilation, so it needs no local gcc/g++/Docker at all.
build:
	nix build .#cloud-redirect -o result

ronin-module: build
	rm -f module/payload/cloud_redirect.so
	cp result/cloud_redirect.so module/payload/cloud_redirect.so
	chmod 644 module/payload/cloud_redirect.so

deploy-tsuki-module: ronin-module
	@test -n "$(TSUKI_ROOT)" || \
		{ echo "usage: make deploy-tsuki-module TSUKI_ROOT=/path/to/tsuki"; exit 2; }
	sh scripts/deploy-tsuki-module.sh "$(TSUKI_ROOT)"

rollback-tsuki-module:
	@test -n "$(TSUKI_ROOT)" || \
		{ echo "usage: make rollback-tsuki-module TSUKI_ROOT=/path/to/tsuki"; exit 2; }
	sh scripts/deploy-tsuki-module.sh --rollback "$(TSUKI_ROOT)"

# The two Ronin-added tests are header-only, ABI-independent logic
# (lua_discovery.h, init_stop.h) and build fine as native binaries; they
# don't need the 32-bit steamclient-matching toolchain build above.
test:
	nix develop --command bash -c ' \
		cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release && \
		cmake --build build-test --target linux_lua_discovery_tests linux_init_stop_tests -j$$(nproc) && \
		./build-test/linux_lua_discovery_tests && \
		./build-test/linux_init_stop_tests'

clean:
	rm -rf result build-test module/payload/cloud_redirect.so
