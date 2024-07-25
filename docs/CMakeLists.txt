if (BUILD_DOCS)

  find_package(Doxygen REQUIRED)
  find_package(Sphinx REQUIRED)
  find_package(Python REQUIRED)


  # Find all the public headers
  get_target_property(HOMESCREEN_PUBLIC_HEADER_DIR ivi-homescreen INCLUDE_DIRECTORIES)
  file(GLOB_RECURSE HOMESCREEN_PUBLIC_HEADERS ${HOMESCREEN_PUBLIC_HEADER_DIR}/*.h)

  set(DOXYGEN_INPUT_DIR ${PROJECT_SOURCE_DIR}/shell)
  set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/doxygen)
  set(DOXYGEN_INDEX_FILE ${DOXYGEN_OUTPUT_DIR}/html/index.html)
  set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
  set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

  #Replace variables inside @@ with the current values
  configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

  #Doxygen won't create this for us
  file(MAKE_DIRECTORY ${DOXYGEN_OUTPUT_DIR}) 

  add_custom_command(OUTPUT ${DOXYGEN_INDEX_FILE}
                     DEPENDS ${HOMESCREEN_PUBLIC_HEADERS}
                     COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE_OUT}
                     MAIN_DEPENDENCY ${DOXYFILE_OUT} ${DOXYFILE_IN}
                     COMMENT "Generating docs"
                     VERBATIM)

  add_custom_target(homescreen-doxygen ALL DEPENDS ${DOXYGEN_INDEX_FILE})

  add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/modules.rst
                      DEPENDS ${DOXYGEN_INDEX_FILE}
                      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_module_docs.py ${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml
                      COMMENT "Generating modules.rst"
                      VERBATIM)

  set(SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR})
  set(SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/sphinx)
  set(SPHINX_INDEX_FILE ${SPHINX_BUILD}/index.html)

  # Only regenerate Sphinx when:
  # - Doxygen has rerun
  # - Our doc files have been updated
  # - The Sphinx config has been updated
  add_custom_command(OUTPUT ${SPHINX_INDEX_FILE}
                     COMMAND 
                       ${SPHINX_EXECUTABLE} -b html
                       # Tell Breathe where to find the Doxygen output
                       -Dbreathe_projects.ivi-homescreen=${DOXYGEN_OUTPUT_DIR}/xml
                     ${SPHINX_SOURCE} ${SPHINX_BUILD}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                     DEPENDS
                     ${CMAKE_CURRENT_SOURCE_DIR}/index.rst
                     ${CMAKE_CURRENT_SOURCE_DIR}/modules.rst
                     ${DOXYGEN_INDEX_FILE}
                     MAIN_DEPENDENCY ${SPHINX_SOURCE}/conf.py
                     COMMENT "Generating documentation with Sphinx")

  # Nice named target so we can run the job easily
  add_custom_target(homescreen-sphinx ALL DEPENDS ${SPHINX_INDEX_FILE})

  # Add an install target to install the docs
  include(GNUInstallDirs)
  install(DIRECTORY ${SPHINX_BUILD}
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

endif ()
