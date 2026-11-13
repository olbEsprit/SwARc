# Dev-only tests of the Win32 platform; nothing here ships.

# win32_text_test: UTF-8 <-> UTF-16 conversion (win32/win32.h);
# portable.
add_executable(win32_text_test win32_text_test.cpp)
target_include_directories(win32_text_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(win32_text_test PRIVATE cxx_std_20)
add_test(NAME win32_text COMMAND win32_text_test)

# d3d9_fixed_vertex_test: the vertex shader generated for a pixel shader
# without a vertex shader, drawn on the SDL_GPU device and read back;
# skipped (77) where no GPU device can be created.
add_executable(d3d9_fixed_vertex_test d3d9_fixed_vertex_test.cpp
               ../src/win32/d3d9_fixed_vertex.cpp)
target_include_directories(d3d9_fixed_vertex_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(d3d9_fixed_vertex_test PRIVATE cxx_std_20)
target_link_libraries(d3d9_fixed_vertex_test PRIVATE recomp_gfx recomp_hlsl recomp_win32 SDL3::SDL3)
add_test(NAME d3d9_fixed_vertex COMMAND d3d9_fixed_vertex_test)
set_tests_properties(d3d9_fixed_vertex PROPERTIES SKIP_RETURN_CODE 77)

# gpu_cube_ff_test: cube textures in the SDL_GPU fixed-function pipeline
# and cube faces as render targets, read back; skipped (77) where no GPU
# device can be created.
add_executable(gpu_cube_ff_test gpu_cube_ff_test.cpp)
target_include_directories(gpu_cube_ff_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(gpu_cube_ff_test PRIVATE cxx_std_20)
target_link_libraries(gpu_cube_ff_test PRIVATE recomp_gfx recomp_win32 SDL3::SDL3)
add_test(NAME gpu_cube_ff COMMAND gpu_cube_ff_test)
set_tests_properties(gpu_cube_ff PROPERTIES SKIP_RETURN_CODE 77)

# gpu_sm1_cube_test: a ps_1_x `tex` of a cube bound at its stage, through the
# translator's cube_stages and the SDL_GPU device; skipped (77) without a GPU.
add_executable(gpu_sm1_cube_test gpu_sm1_cube_test.cpp
               ../src/win32/d3d9_fixed_vertex.cpp)
target_include_directories(gpu_sm1_cube_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(gpu_sm1_cube_test PRIVATE cxx_std_20)
target_link_libraries(gpu_sm1_cube_test PRIVATE recomp_gfx recomp_hlsl recomp_win32 SDL3::SDL3)
add_test(NAME gpu_sm1_cube COMMAND gpu_sm1_cube_test)
set_tests_properties(gpu_sm1_cube PROPERTIES SKIP_RETURN_CODE 77)

# wsock32_test: wsock32's ordinal imports (aliases of ws2_32's handlers)
# through the thunk dispatch, over loopback TCP.
add_executable(wsock32_test wsock32_test.cpp)
target_include_directories(wsock32_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(wsock32_test PRIVATE cxx_std_20)
target_link_libraries(wsock32_test PRIVATE recomp_win32)
add_test(NAME wsock32 COMMAND wsock32_test)
# dinput8_pad_test: dinput8's game controller on an SDL virtual gamepad.
add_executable(dinput8_pad_test dinput8_pad_test.cpp)
target_include_directories(dinput8_pad_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(dinput8_pad_test PRIVATE cxx_std_20)
target_link_libraries(dinput8_pad_test PRIVATE recomp_win32 recomp_dinput8 SDL3::SDL3)
add_test(NAME dinput8_pad COMMAND dinput8_pad_test)
