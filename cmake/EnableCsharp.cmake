function(enable_csharp)
  if(WIN32)
    # C# is currently only supported on Windows. On other platforms we find mono
    # manually
    enable_language(CSharp)
    return()
  endif()

  find_program(_MONO_EXECUTABLE mono)
  find_program(_MCS_EXECUTABLE mcs)
  if(_MONO_EXECUTABLE AND _MCS_EXECUTABLE)
    set(CSHARP_USE_MONO
        TRUE
        PARENT_SCOPE)
    set(MONO_EXECUTABLE
        ${_MONO_EXECUTABBLE}
        PARENT_SCOPE)
    set(MCS_EXECUTABLE
        ${_MONO_EXECUTABBLE}
        PARENT_SCOPE)
    return()
  endif()

  find_package(dotnet 9.0 REQUIRED)
  set(CSHARP_USE_MONO
      FALSE
      PARENT_SCOPE)
endfunction()

enable_csharp()
