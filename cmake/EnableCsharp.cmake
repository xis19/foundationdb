function(enable_csharp)
  if(WIN32)
    # C# is currently only supported on Windows. On other platforms we find mono
    # manually
    enable_language(CSharp)
    return()
  endif()

  find_package(dotnet 9.0)
  if (DOTNET_FOUND)
	return()
  endif()

  find_package(mono REQUIRED)
endfunction()

enable_csharp()
