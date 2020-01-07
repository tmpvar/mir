# CMakeList.txt : CMake project for CMakeProject2, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("MIR")

add_definitions(-DMIR_NO_GEN_DEBUG=1)

add_library(mir STATIC mir.c "mir.h")
add_library(mir_gen STATIC mir-gen.c mman.c "mir.h")
# add_library(llvm2mir STATIC llvm2mir/llvm2mir.c)

add_executable (c2m "c2mir/c2mir.c" "c2mir/c2mir.h" "c2mir/c2mir-driver.c" "mir-gen.h")
target_include_directories(c2m PRIVATE ./)
target_link_libraries(c2m mir mir_gen)

# add_executable (l2m "llvm2mir/llvm2mir-driver.c" "llvm2mir/llvm2mir.h")
# target_include_directories(l2m PRIVATE ./)
# target_link_libraries(l2m mir mir_gen llvm2mir)

# add_executable (m2b "mir-utils/m2b.c")
# target_include_directories(m2b PRIVATE ./)
# target_link_libraries(m2b mir)

# add_executable (b2m "mir-utils/b2m.c")
# target_include_directories(b2m PRIVATE ./)
# target_link_libraries(b2m mir)

# add_executable (b2ctab "mir-utils/b2ctab.c")
# target_include_directories(b2ctab PRIVATE ./)
# target_link_libraries(b2ctab mir)



# TODO: Add tests and install targets if needed.

#mir.o: mir.c $(MIR_DEPS)
#	$(CC) -c $(CFLAGS) -o $@ $<
#mir-gen.o: mir-gen.c $(MIR_GEN_DEPS)
#	$(CC) -c $(CFLAGS) -D$(TARGET) -o $@ $<
#c2m: mir.o mir-gen.o c2mir/c2mir.h c2mir/c2mir.c c2mir/c2mir-driver.c
#	$(CC) $(CFLAGS) -D$(TARGET) -I. mir-gen.o c2mir/c2mir.c c2mir/c2mir-driver.c mir.o -ldl -o $@
