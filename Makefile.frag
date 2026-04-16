
# VMA C++ wrapper - only compiled when Vulkan is available
ifeq ($(VIO_HAS_VULKAN),yes)
$(builddir)/src/backends/vulkan/vio_vma_wrapper.lo: $(srcdir)/src/backends/vulkan/vio_vma_wrapper.cpp
	$(LIBTOOL) --tag=CXX --mode=compile $(CXX) -std=c++14 \
		-I$(srcdir)/vendor/vma -I$(srcdir)/include -I. \
		$(VIO_VULKAN_CFLAGS) \
		-DHAVE_CONFIG_H -fPIC -O2 \
		-c $(srcdir)/src/backends/vulkan/vio_vma_wrapper.cpp \
		-o $(builddir)/src/backends/vulkan/vio_vma_wrapper.lo

shared_objects_vio += src/backends/vulkan/vio_vma_wrapper.lo
endif

# Metal backend - only compiled on macOS when Metal is available
ifeq ($(VIO_HAS_METAL),yes)
$(builddir)/src/backends/metal/vio_metal.lo: $(srcdir)/src/backends/metal/vio_metal.m
	@mkdir -p $(builddir)/src/backends/metal
	$(LIBTOOL) --tag=CC --mode=compile $(CC) -x objective-c \
		$(CFLAGS) $(EXTRA_CFLAGS) \
		$(INCLUDES) \
		-fobjc-arc \
		-c $(srcdir)/src/backends/metal/vio_metal.m \
		-o $(builddir)/src/backends/metal/vio_metal.lo

shared_objects_vio += src/backends/metal/vio_metal.lo
PHP_GLOBAL_OBJS += ext/vio/src/backends/metal/vio_metal.lo
endif
