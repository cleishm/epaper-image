# ctest convert_smoke: convert a small generated image for every panel, in
# each mode, and check the frame and preview sizes.
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

function(run_rejected)
    execute_process(COMMAND ${CONVERT} ${ARGN} RESULT_VARIABLE rc ERROR_QUIET OUTPUT_QUIET)
    if(NOT rc EQUAL 2)
        message(FATAL_ERROR "convert ${ARGN}: exit ${rc}, want 2")
    endif()
endfunction()

# A PNG's width and height, from its IHDR.
function(check_png file want_w want_h)
    file(READ ${file} ihdr OFFSET 16 LIMIT 8 HEX)
    string(SUBSTRING ${ihdr} 0 8 w)
    string(SUBSTRING ${ihdr} 8 8 h)
    math(EXPR w "0x${w}")
    math(EXPR h "0x${h}")
    if(NOT w EQUAL want_w OR NOT h EQUAL want_h)
        message(FATAL_ERROR "${file}: ${w}x${h}, want ${want_w}x${want_h}")
    endif()
endfunction()

function(check_same a b)
    file(SHA256 ${a} hash_a)
    file(SHA256 ${b} hash_b)
    if(NOT hash_a STREQUAL hash_b)
        message(FATAL_ERROR "${a} and ${b} differ")
    endif()
endfunction()

run(--list-panels)
foreach(spec "el133uf1;960000;1200;1600;300;400;360;480"
             "gdey075t7-mono;48000;800;480;200;120;240;144"
             "gdey075t7-grey4;96000;800;480;200;120;240;144")
    list(GET spec 0 panel)
    list(GET spec 1 bytes)
    list(GET spec 2 full_w)
    list(GET spec 3 full_h)
    list(GET spec 4 quarter_w)
    list(GET spec 5 quarter_h)
    list(GET spec 6 tenths_w)
    list(GET spec 7 tenths_h)
    run(--panel ${panel} --preview ${WORK}/${panel}.png ${WORK}/in.ppm ${WORK}/${panel}.bin)
    check_size(${WORK}/${panel}.bin ${bytes})
    check_png(${WORK}/${panel}.png ${full_w} ${full_h})
    # A reduced preview: its size, and the same frame as without it.
    run(--panel ${panel} --preview ${WORK}/${panel}-q.png --preview-scale 0.25 ${WORK}/in.ppm ${WORK}/${panel}-q.bin)
    check_png(${WORK}/${panel}-q.png ${quarter_w} ${quarter_h})
    check_same(${WORK}/${panel}.bin ${WORK}/${panel}-q.bin)
    run(--panel ${panel} --preview ${WORK}/${panel}-t.png --preview-scale 0.3 ${WORK}/in.ppm ${WORK}/${panel}-t.bin)
    check_png(${WORK}/${panel}-t.png ${tenths_w} ${tenths_h})
    check_same(${WORK}/${panel}.bin ${WORK}/${panel}-t.bin)
    # It works with --rotate (a share of the shown image) and --compare.
    run(--panel ${panel} --rotate --preview ${WORK}/${panel}-r.png --preview-scale 0.25 ${WORK}/in.ppm ${WORK}/${panel}-r.bin)
    check_png(${WORK}/${panel}-r.png ${quarter_h} ${quarter_w})
    run(--panel ${panel} --compare --seed 1 --key ${WORK}/${panel}-c.json --preview ${WORK}/${panel}-c.png --preview-scale 0.25
        ${WORK}/in.ppm ${WORK}/${panel}-c.bin preset=Default tone=0.25,detail=0.5)
    check_png(${WORK}/${panel}-c.png ${quarter_w} ${quarter_h})
    # --nominal changes the colours, not the size.
    run(--panel ${panel} --nominal --preview ${WORK}/${panel}-n.png --preview-scale 0.25 ${WORK}/in.ppm ${WORK}/${panel}-n.bin)
    check_png(${WORK}/${panel}-n.png ${quarter_w} ${quarter_h})
    file(SHA256 ${WORK}/${panel}-q.png measured)
    file(SHA256 ${WORK}/${panel}-n.png nominal)
    if(measured STREQUAL nominal)
        message(FATAL_ERROR "${panel}: --nominal did not change the preview")
    endif()
    # Bad uses.
    run_rejected(--panel ${panel} --preview ${WORK}/bad.png --preview-scale 0 ${WORK}/in.ppm ${WORK}/bad.bin)
    run_rejected(--panel ${panel} --preview ${WORK}/bad.png --preview-scale 1.5 ${WORK}/in.ppm ${WORK}/bad.bin)
    run_rejected(--panel ${panel} --preview-scale 0.25 ${WORK}/in.ppm ${WORK}/bad.bin)
    run(--panel ${panel} --fit --rotate --filter box ${WORK}/in.ppm ${WORK}/${panel}-fit.bin)
    check_size(${WORK}/${panel}-fit.bin ${bytes})
    run(--panel ${panel} --compare --seed 1 --key ${WORK}/${panel}.json
        ${WORK}/in.ppm ${WORK}/${panel}-cmp.bin preset=Default tone=0.25,detail=0.5)
    check_size(${WORK}/${panel}-cmp.bin ${bytes})
endforeach()
