# The examples of the guide "Listen-only participants" against the files the tests build: in DOC
# (docs/j1939-guide.md; scripts/check-docs.py keeps the Japanese page's blocks the same), the
# block after the line "<!-- tests/spy/cal_messages.yaml -->" is MESSAGES without its comment
# lines, and the blocks after "<!-- tests/spy/guide_example.hpp -->" and
# "<!-- tests/spy/guide_analysis.hpp -->" are the lines of EXAMPLE and ANALYSIS between
# "// guide: begin" and "// guide: end". Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var DOC EXAMPLE ANALYSIS MESSAGES)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "guide_example.cmake: ${var} is required")
  endif()
endforeach()

function(read_lf out path)
  file(READ "${path}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The fenced block after the line "<!-- MARKER -->" in `doc`, without the fences.
function(block_after out doc marker)
  read_lf(text "${doc}")
  string(FIND "${text}" "<!-- ${marker} -->\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${doc} has no line <!-- ${marker} -->: put it back before the block it marks")
  endif()
  string(SUBSTRING "${text}" ${at} -1 text)
  string(FIND "${text}" "```" open)
  string(SUBSTRING "${text}" ${open} -1 text)
  string(FIND "${text}" "\n" line_end)
  math(EXPR body_start "${line_end} + 1")
  string(SUBSTRING "${text}" ${body_start} -1 text)
  string(FIND "${text}" "```" close)
  string(SUBSTRING "${text}" 0 ${close} text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

read_lf(messages "${MESSAGES}")
string(REGEX REPLACE "(^|\n)#[^\n]*" "" messages "${messages}")
string(REGEX REPLACE "^\n" "" messages "${messages}")

# The lines of `path` between "// guide: begin" and "// guide: end".
function(guide_lines out path)
  read_lf(text "${path}")
  string(FIND "${text}" "// guide: begin\n" begin)
  string(FIND "${text}" "// guide: end\n" end)
  if(begin EQUAL -1 OR end EQUAL -1 OR end LESS begin)
    message(FATAL_ERROR "${path} has no lines \"// guide: begin\" and \"// guide: end\" (in this order) around the guide's block")
  endif()
  string(LENGTH "// guide: begin\n" marker_length)
  math(EXPR body_start "${begin} + ${marker_length}")
  math(EXPR body_length "${end} - ${body_start}")
  string(SUBSTRING "${text}" ${body_start} ${body_length} text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()
guide_lines(example "${EXAMPLE}")
guide_lines(analysis "${ANALYSIS}")

set(failures "")
foreach(doc IN ITEMS "${DOC}")
  foreach(pair "tests/spy/cal_messages.yaml|messages|${MESSAGES} without its comment lines"
               "tests/spy/guide_example.hpp|example|the lines of ${EXAMPLE} between the markers"
               "tests/spy/guide_analysis.hpp|analysis|the lines of ${ANALYSIS} between the markers")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 marker)
    list(GET pair 1 variable)
    list(GET pair 2 what)
    block_after(shown "${doc}" "${marker}")
    if(NOT shown STREQUAL ${variable})
      string(APPEND failures "${doc} shows, after <!-- ${marker} -->,\n${shown}\nbut ${what} is\n${${variable}}\n")
    endif()
  endforeach()
endforeach()
if(failures)
  message(FATAL_ERROR "${failures}Make them the same: the guide shows what the tests build (tests/spy/test_spy_watch.cpp, test_spy_analysis.cpp)")
endif()
message(STATUS "guide_example: the examples of the guide are the ones the tests build")
