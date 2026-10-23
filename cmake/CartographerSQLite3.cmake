if(TARGET CartoSQLite3)
    return()
endif()

if(WIN32)
    if(NOT CARTO_WINSQLITE3_SDK_ROOT)
        if(DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
            set(CARTO_WINSQLITE3_SDK_ROOT "$ENV{WindowsSdkDir}" CACHE PATH
                "Windows SDK root containing WinSQLite3" FORCE)
        else()
            set(CARTO_WINSQLITE3_SDK_ROOT
                "C:/Program Files (x86)/Windows Kits/10" CACHE PATH
                "Windows SDK root containing WinSQLite3")
        endif()
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(_carto_sqlite_arch arm64)
        else()
            set(_carto_sqlite_arch x64)
        endif()
    else()
        set(_carto_sqlite_arch x86)
    endif()

    file(GLOB _carto_sdk_include_versions LIST_DIRECTORIES true
        "${CARTO_WINSQLITE3_SDK_ROOT}/Include/*")
    list(SORT _carto_sdk_include_versions COMPARE NATURAL ORDER DESCENDING)
    foreach(_carto_sdk_include_version IN LISTS _carto_sdk_include_versions)
        get_filename_component(_carto_sdk_version
            "${_carto_sdk_include_version}" NAME)
        set(_carto_sqlite_header
            "${_carto_sdk_include_version}/um/winsqlite/winsqlite3.h")
        set(_carto_sqlite_shared_include
            "${_carto_sdk_include_version}/shared")
        set(_carto_sqlite_import_library
            "${CARTO_WINSQLITE3_SDK_ROOT}/Lib/${_carto_sdk_version}/um/${_carto_sqlite_arch}/winsqlite3.lib")
        if(EXISTS "${_carto_sqlite_header}" AND
           EXISTS "${_carto_sqlite_shared_include}/sdkddkver.h" AND
           EXISTS "${_carto_sqlite_import_library}")
            add_library(CartoSQLite3 UNKNOWN IMPORTED GLOBAL)
            set_target_properties(CartoSQLite3 PROPERTIES
                IMPORTED_LOCATION "${_carto_sqlite_import_library}")
            set(CARTO_SQLITE3_INCLUDE_DIRECTORIES
                "${_carto_sdk_include_version}/um/winsqlite;${_carto_sqlite_shared_include}")
            set(CARTO_USE_WINSQLITE3 ON)
            break()
        endif()
    endforeach()

    if(NOT TARGET CartoSQLite3)
        message(FATAL_ERROR
            "Cartographer requires WinSQLite3 from the Windows SDK on Windows. "
            "Install the SDK or set CARTO_WINSQLITE3_SDK_ROOT to its root.")
    endif()
else()
    find_package(SQLite3 REQUIRED)
    add_library(CartoSQLite3 INTERFACE IMPORTED GLOBAL)
    set_property(TARGET CartoSQLite3 PROPERTY
        INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:SQLite::SQLite3>")
    set(CARTO_SQLITE3_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
endif()
