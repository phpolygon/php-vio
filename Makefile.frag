
# VMA C++ wrapper - compiled as C++ since VMA is a C++ header-only library
$(builddir)/src/backends/vulkan/vio_vma_wrapper.lo: $(srcdir)/src/backends/vulkan/vio_vma_wrapper.cpp
	$(LIBTOOL) --tag=CXX --mode=compile $(CXX) -std=c++14 \
		-I$(srcdir)/vendor/vma -I$(srcdir)/include -I. \
		-DHAVE_CONFIG_H -fPIC -O2 \
		-c $(srcdir)/src/backends/vulkan/vio_vma_wrapper.cpp \
		-o $(builddir)/src/backends/vulkan/vio_vma_wrapper.lo

# Add VMA wrapper to the shared object list
shared_objects_vio += src/backends/vulkan/vio_vma_wrapper.lo

# Metal backend - compiled as Objective-C
$(builddir)/src/backends/metal/vio_metal.lo: $(srcdir)/src/backends/metal/vio_metal.m
	@mkdir -p $(builddir)/src/backends/metal
	$(LIBTOOL) --tag=CC --mode=compile $(CC) -x objective-c \
		$(COMMON_FLAGS) $(CFLAGS_CLEAN) $(EXTRA_CFLAGS) \
		-I$(top_srcdir) -I$(top_builddir) \
		-I$(srcdir)/include -I. \
		$(INCLUDES) \
		-DHAVE_CONFIG_H -fPIC -O2 \
		-fobjc-arc \
		-c $(srcdir)/src/backends/metal/vio_metal.m \
		-o $(builddir)/src/backends/metal/vio_metal.lo

shared_objects_vio += src/backends/metal/vio_metal.lo
