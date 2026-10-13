all:
	gcc -O0 -ggdb \
		decode-brw.c \
		../../src/devices/gpu-vulkan.c \
		../../release.linux.x86_64/obj/src/core/*.o \
		../../release.linux.x86_64/obj/src/cpu/*.o \
		../../release.linux.x86_64/obj/src/rvjit/*.o \
		../../release.linux.x86_64/obj/src/util/*.o \
		-I../../src \
		-I../../src/util \
		-I../../include \
		-I../../src/devices \
		-D RVJIT_X86 \
		-lvulkan -lSDL2 -lm
