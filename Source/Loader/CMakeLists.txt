# ================================================================================================
#  DS3OS
#  Copyright (C) 2021 Tim Leonard
# ================================================================================================

find_program(DOTNET_EXECUTABLE dotnet)
if(NOT DOTNET_EXECUTABLE)
        message(FATAL_ERROR "The .NET SDK is required to build the Loader.")
endif()

set(LOADER_DOTNET_HOME "${ENV_ROOT_PATH}/intermediate/dotnet-home")

add_custom_target(
        Loader ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LOADER_DOTNET_HOME}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "DOTNET_CLI_HOME=${LOADER_DOTNET_HOME}"
                "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
                "DOTNET_NOLOGO=1"
                "${DOTNET_EXECUTABLE}" restore "${CMAKE_CURRENT_SOURCE_DIR}/Loader.csproj"
                --ignore-failed-sources
                -p:Platform=${ENV_ARCHITECTURE}
                -p:Configuration=$<CONFIG>
                -p:NuGetAudit=false
        COMMAND "${CMAKE_COMMAND}" -E env
                "DOTNET_CLI_HOME=${LOADER_DOTNET_HOME}"
                "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
                "DOTNET_NOLOGO=1"
                "${DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/Loader.csproj"
                --no-restore
                -c $<CONFIG>
                -p:Platform=${ENV_ARCHITECTURE}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building Loader"
        VERBATIM
)

util_setup_folder_structure(Loader SOURCES "DS3OS")
