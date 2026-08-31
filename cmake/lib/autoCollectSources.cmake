function(add_src_folder path)

    
    file(GLOB_RECURSE src_files CONFIGURE_DEPENDS
        "${path}/*.cpp"
    )

    #set(headers ${include_headers} ${vendor_include_headers} PARENT_SCOPE)
    set(sources ${sources} ${src_files} PARENT_SCOPE)
endfunction()

function(add_include_folder path)
    
    file(GLOB_RECURSE header_files CONFIGURE_DEPENDS
        "${path}/*.h"
    )

    set(headers ${headers} ${header_files} PARENT_SCOPE)
endfunction()
