if (BUILD_DOCS)

  find_package(Doxygen)
  if (DOXYGEN_FOUND)
    # Find all the public headers
    get_target_property(HOMESCREEN_PUBLIC_HEADER_DIR ivi-homescreen INCLUDE_DIRECTORIES)
    file(GLOB_RECURSE HOMESCREEN_PUBLIC_HEADERS ${HOMESCREEN_PUBLIC_HEADER_DIR}/*.h)

    set(DOXYGEN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../docs/doxygen/Doxyfile)
    set(DOXYGEN_INDEX_FILE ${CMAKE_SOURCE_DIR}/docs/doxygen/html/index.html)

    message("Doxygen build started")
    add_custom_command(OUTPUT ${DOXYGEN_INDEX_FILE}
                        DEPENDS ${HOMESCREEN_PUBLIC_HEADERS}
                        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_FILE}
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs/doxygen
                        COMMENT "Generating API documentation with Doxygen"
                        VERBATIM)

    add_custom_target(doc_doxygen ALL DEPENDS ${DOXYGEN_INDEX_FILE})
  else ()
    message("Doxygen need to be installed to generate the doxygen documentation")
  endif ()



  find_package(Python REQUIRED)
  message(STATUS "Python...... ${PYTHON_FOUND}")
  if(PYTHON_FOUND)
    add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/docs/source/modules.rst
                        DEPENDS ${DOXYGEN_INDEX_FILE}
                        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_module_docs.py ${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs/source
                        COMMENT "Generating modules.rst"
                        VERBATIM)
    add_custom_target(doc_modules ALL DEPENDS ${CMAKE_SOURCE_DIR}/docs/source/modules.rst)
  endif ()



  find_package(Sphinx REQUIRED)
  message(STATUS "Sphinx...... ${SPHINX_FOUND}")
  if(SPHINX_FOUND)

    set(SPHINX_SOURCE ${CMAKE_SOURCE_DIR}/docs/source)
    set(SPHINX_BUILD ${CMAKE_SOURCE_DIR}/docs/sphinx)
    set(SPHINX_INDEX_FILE ${SPHINX_BUILD}/index.html)

    # Only regenerate Sphinx when:
    # - Doxygen has rerun
    # - Our doc files have been updated
    # - The Sphinx config has been updated
    add_custom_command(OUTPUT ${SPHINX_INDEX_FILE}
                       COMMAND 
                         ${SPHINX_EXECUTABLE} -b html
                         # Tell Breathe where to find the Doxygen output
                         -Dbreathe_projects.ivi-homescreen=${CMAKE_SOURCE_DIR}/docs/doxygen/xml
                       ${SPHINX_SOURCE} ${SPHINX_BUILD}
                       WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                       DEPENDS
                       ${SPHINX_SOURCE}/index.rst
                       ${DOXYGEN_INDEX_FILE}
                       MAIN_DEPENDENCY ${SPHINX_SOURCE}/conf.py
                       COMMENT "Generating documentation with Sphinx")

    # Nice named target so we can run the job easily
    add_custom_target(homescreen-sphinx ALL DEPENDS ${SPHINX_INDEX_FILE})

    # Add an install target to install the docs
    # include(GNUInstallDirs)
    # install(DIRECTORY ${SPHINX_BUILD}
    # DESTINATION ${CMAKE_INSTALL_DOCDIR})
  endif ()

endif ()
