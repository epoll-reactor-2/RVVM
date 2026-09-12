all:
	gcc -O0 -ggdb ../../src/devices/gpu-vulkan.c main.c ../../src/util/*.c -I../../src -I../../src/util -I../../include -I../../src/devices -lvulkan -lSDL2

# This environment variable needed to change renderer. My host
# Vega 8 is buggy and segfaults during pipeline creation.
run:
	VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./a.out

.PHONY: compare
compare:
	@zsh -c 'make all'
	@zsh -c 'echo "Compiled SPIR-V (vertex)..."'
	@zsh -c 'spirv-dis /tmp/vs.spv'
	@zsh -c 'echo ""'
	@zsh -c 'echo "Native SPIR-V (vertex)..."'
	@zsh -c 'spirv-dis ../draw/vert.spv'
	@zsh -c 'echo ""'
	@zsh -c 'echo "Compiled SPIR-V (fragment)..."'
	@zsh -c 'spirv-dis /tmp/fs.spv'
	@zsh -c 'echo ""'
	@zsh -c 'echo "Native SPIR-V (fragment)..."'
	@zsh -c 'spirv-dis ../draw/frag.spv'
	@zsh -c 'echo ""'
	@zsh -c 'echo "Diff (vertex)"'
	@zsh -c 'diff --color=always <(spirv-dis ../draw/vert.spv) <(spirv-dis /tmp/vs.spv)'
	@zsh -c 'echo ""'
	@zsh -c 'echo "Diff (fragment)"'
	@zsh -c 'diff --color=always <(spirv-dis ../draw/frag.spv) <(spirv-dis /tmp/fs.spv)'
