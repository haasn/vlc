# libplacebo

PLACEBO_HASH=7eb7a054c538dfd16854b03f67a3dd1cba1a7706
PLACEBO_SNAPURL := https://github.com/haasn/libplacebo/archive/$(PLACEBO_HASH).tar.gz
PLACEBO_GITURL := https://github.com/haasn/libplacebo.git

PLACEBOCONF := --prefix="$(PREFIX)" \
	--libdir lib \
	--default-library static

$(TARBALLS)/libplacebo-$(PLACEBO_HASH).tar.xz:
	$(call download_git,$(PLACEBO_GITURL),,$(PLACEBO_HASH))

.sum-libplacebo: $(TARBALLS)/libplacebo-$(PLACEBO_HASH).tar.xz
	$(call check_githash,$(PLACEBO_HASH))
	touch $@

libplacebo: libplacebo-$(PLACEBO_HASH).tar.xz .sum-libplacebo
	$(UNPACK)
	$(MOVE)

.libplacebo: libplacebo
	cd $< && rm -rf ./build
	cd $< && $(HOSTVARS) meson $(PLACEBOCONF) build
	cd $< && cd build && ninja install
	touch $@
