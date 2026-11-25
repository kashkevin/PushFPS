# PushFPS Package 16 ("Lower delay", item 4 d): compile one GPU DIS shader float-only (`-D double=float`), run with
# cmake -P from cmake/VeyraGpuDis.cmake. Never fails the build: a shader DXC refuses becomes an empty file, the
# predicted frame's start-up self-check then cannot create the float flow and the strict (FP64) one is used.
# Inputs: DXC, SRC, OUT, ARGS (the recipe's arguments joined with '|').
string(REPLACE "|" ";" recipe_args "${ARGS}")
get_filename_component(out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")
execute_process(COMMAND "${DXC}" ${recipe_args} -D double=float -Fo "${OUT}" "${SRC}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(WARNING "float-only GPU DIS shader not built (${SRC}); the strict one stays in use: ${errors}")
  file(WRITE "${OUT}" "")
endif()
