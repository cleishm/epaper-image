# ctest convert_smoke: convert a small generated image for every panel, in
# each mode, and check the frame sizes.
#   cmake -DCONVERT=<convert> -DWORK=<scratch dir> -P smoke_test.cmake

file(MAKE_DIRECTORY ${WORK})
# A 6x4 binary PPM (stb_image reads PNM). Sample values stay in 1..127 so
# string(ASCII) gives one byte each.
string(ASCII 80 54 10 54 32 52 10 50 53 53 10 ppm) # "P6\n6 4\n255\n"
foreach(i RANGE 23)
    math(EXPR r "1 + (${i} * 11) % 127")
    math(EXPR g "1 + (${i} * 53) % 127")
    math(EXPR b "127 - ${i} * 5")
    string(ASCII ${r} ${g} ${b} px)
    string(APPEND ppm "${px}")
endforeach()
file(WRITE ${WORK}/in.ppm "${ppm}")

function(run)
    execute_process(COMMAND ${CONVERT} ${ARGN} RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "convert ${ARGN}: exit ${rc}\n${err}")
    endif()
endfunction()

function(check_size file want)
    file(SIZE ${file} got)
    if(NOT got EQUAL want)
        message(FATAL_ERROR "${file}: ${got} bytes, want ${want}")
    endif()
endfunction()

run(--list-panels)
foreach(spec "el133uf1;960000" "gdey075t7-mono;48000" "gdey075t7-grey4;96000")
    list(GET spec 0 panel)
    list(GET spec 1 bytes)
    run(--panel ${panel} --preview ${WORK}/${panel}.png ${WORK}/in.ppm ${WORK}/${panel}.bin)
    check_size(${WORK}/${panel}.bin ${bytes})
    run(--panel ${panel} --fit --rotate --filter box ${WORK}/in.ppm ${WORK}/${panel}-fit.bin)
    check_size(${WORK}/${panel}-fit.bin ${bytes})
    run(--panel ${panel} --compare --seed 1 --key ${WORK}/${panel}.json
        ${WORK}/in.ppm ${WORK}/${panel}-cmp.bin preset=Default tone=0.25,detail=0.5)
    check_size(${WORK}/${panel}-cmp.bin ${bytes})
endforeach()
